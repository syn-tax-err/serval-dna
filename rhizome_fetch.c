/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <time.h>
#include <arpa/inet.h>
#include <assert.h>
#include "serval.h"
#include "rhizome.h"
#include "str.h"

/* Represents a queued fetch of a bundle payload, for which the manifest is already known.
 */
struct rhizome_fetch_candidate {
  rhizome_manifest *manifest;
  struct sockaddr_in peer;
  int priority;
};

/* Represents an active fetch (in progress) of a bundle payload (.manifest != NULL) or of a bundle
 * manifest (.manifest == NULL).
 */
struct rhizome_fetch_slot {
  struct sched_ent alarm; // must be first element in struct
  rhizome_manifest *manifest;
  struct sockaddr_in peer;
  int state;
#define RHIZOME_FETCH_FREE 0
#define RHIZOME_FETCH_CONNECTING 1
#define RHIZOME_FETCH_SENDINGHTTPREQUEST 2
#define RHIZOME_FETCH_RXHTTPHEADERS 3
#define RHIZOME_FETCH_RXFILE 4
  FILE *file;
  char filename[1024];
  char request[1024];
  int request_len;
  int request_ofs;
  int64_t file_len;
  int64_t file_ofs;
};

/* Represents a queue of fetch candidates and a single active fetch for bundle payloads whose size
 * is less than a given threshold.
 */
struct rhizome_fetch_queue {
  struct rhizome_fetch_slot active; // must be first element in struct
  int candidate_queue_size;
  struct rhizome_fetch_candidate *candidate_queue;
  long long size_threshold; // will only hold fetches of fewer than this many bytes
};

struct rhizome_fetch_candidate queue0[5];
struct rhizome_fetch_candidate queue1[4];
struct rhizome_fetch_candidate queue2[3];
struct rhizome_fetch_candidate queue3[2];
struct rhizome_fetch_candidate queue4[1];

#define NELS(a) (sizeof (a) / sizeof *(a))

struct rhizome_fetch_queue rhizome_fetch_queues[] = {
  // Must be in order of ascending size_threshold.
  { .candidate_queue_size = NELS(queue0), .candidate_queue = queue0, .size_threshold =     10000, .active = { .state = RHIZOME_FETCH_FREE } },
  { .candidate_queue_size = NELS(queue1), .candidate_queue = queue1, .size_threshold =    100000, .active = { .state = RHIZOME_FETCH_FREE } },
  { .candidate_queue_size = NELS(queue2), .candidate_queue = queue2, .size_threshold =   1000000, .active = { .state = RHIZOME_FETCH_FREE } },
  { .candidate_queue_size = NELS(queue3), .candidate_queue = queue3, .size_threshold =  10000000, .active = { .state = RHIZOME_FETCH_FREE } },
  { .candidate_queue_size = NELS(queue4), .candidate_queue = queue4, .size_threshold =        -1, .active = { .state = RHIZOME_FETCH_FREE } }
};

#define NQUEUES	    NELS(rhizome_fetch_queues)

struct profile_total fetch_stats;

/* Find a queue suitable for a fetch of the given number of bytes.  If there is no suitable queue,
 * return NULL.
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
static struct rhizome_fetch_queue *rhizome_find_queue(long long size)
{
  int i;
  for (i = 0; i < NQUEUES; ++i) {
    struct rhizome_fetch_queue *q = &rhizome_fetch_queues[i];
    if (q->size_threshold < 0 || size < q->size_threshold)
      return q;
  }
  return NULL;
}

/* Find a free fetch slot suitable for fetching the given number of bytes.  This could be a slot in
 * any queue that would accept the candidate, ie, with a larger size threshold.  Returns NULL if
 * there is no suitable free slot.
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
static struct rhizome_fetch_slot *rhizome_find_fetch_slot(long long size)
{
  int i;
  for (i = 0; i < NQUEUES; ++i) {
    struct rhizome_fetch_queue *q = &rhizome_fetch_queues[i];
    if ((q->size_threshold < 0 || size < q->size_threshold) && q->active.state == RHIZOME_FETCH_FREE)
      return &q->active;
  }
  return NULL;
}

static struct rhizome_fetch_candidate *rhizome_fetch_insert(struct rhizome_fetch_queue *q, int i)
{
  assert(i >= 0 && i < q->candidate_queue_size);
  struct rhizome_fetch_candidate *c = &q->candidate_queue[i];
  struct rhizome_fetch_candidate *e = &q->candidate_queue[q->candidate_queue_size - 1];
  if (debug & DEBUG_RHIZOME_RX)
    DEBUGF("insert queue[%d] candidate[%d]", q - rhizome_fetch_queues, i);
  if (e->manifest)
    rhizome_manifest_free(e->manifest);
  for (; e > c; --e)
    e[0] = e[-1];
  assert(e == c);
  c->manifest = NULL;
  return c;
}

static void rhizome_fetch_unqueue(struct rhizome_fetch_queue *q, int i)
{
  assert(i >= 0 && i < q->candidate_queue_size);
  struct rhizome_fetch_candidate *c = &q->candidate_queue[i];
  if (debug & DEBUG_RHIZOME_RX)
    DEBUGF("unqueue queue[%d] candidate[%d] manifest=%p", q - rhizome_fetch_queues, i, c->manifest);
  if (c->manifest) {
    rhizome_manifest_free(c->manifest);
    c->manifest = NULL;
  }
  struct rhizome_fetch_candidate *e = &q->candidate_queue[q->candidate_queue_size - 1];
  for (; c < e && c[1].manifest; ++c)
    c[0] = c[1];
  c->manifest = NULL;
}

/* Return true if there are any active fetches currently in progress.
 *
 * @author Andrew Bettison <andrew@servalproject.com>
 */
int rhizome_any_fetch_active()
{
  int i;
  for (i = 0; i < NQUEUES; ++i)
    if (rhizome_fetch_queues[i].active.state != RHIZOME_FETCH_FREE)
      return 1;
  return 0;
}

/*
   Queue a manifest for importing.

   There are three main cases that can occur here:

   1. The manifest has a nil payload (filesize=0);
   2. The associated payload is already in our database; or
   3. The associated payload is not already in our database, and so we need
   to fetch it before we can import it.

   Cases (1) and (2) are more or less identical, and all we need to do is to
   import the manifest into the database.

   Case (3) requires that we fetch the associated file.

   This is where life gets interesting.

   First, we need to make sure that we can free up enough space in the database
   for the file.

   Second, we need to work out how we are going to get the file.
   If we are on an IPv4 wifi network, then HTTP is probably the way to go.
   If we are not on an IPv4 wifi network, then HTTP is not an option, and we need
   to use a Rhizome/Overlay protocol to fetch it.  It might even be HTTP over MDP
   (Serval Mesh Datagram Protocol) or MTCP (Serval Mesh Transmission Control Protocol
   -- yet to be specified).

   For efficiency, the MDP transfer protocol should allow multiple listeners to
   receive the data. In contrast, it would be nice to have the data auth-crypted, if
   only to deal with packet errors (but also naughty people who might want to mess
   with the transfer.

   For HTTP over IPv4, the IPv4 address and port number of the sender is sent as part of the
   advertisement.
*/

static int rhizome_fetch(rhizome_manifest *m, const struct sockaddr_in *peerip);
#define STARTED (0)
#define SLOTBUSY (1)
#define SAMEBUNDLE (2)
#define SAMEPAYLOAD (3)
#define SUPERSEDED (4)
#define OLDERBUNDLE (5)
#define NEWERBUNDLE (6)
#define IMPORTED (7)

/* As defined below uses 64KB */
#define RHIZOME_VERSION_CACHE_NYBLS 2 /* 256=2^8=2nybls */
#define RHIZOME_VERSION_CACHE_SHIFT 1
#define RHIZOME_VERSION_CACHE_SIZE 128
#define RHIZOME_VERSION_CACHE_ASSOCIATIVITY 16

struct rhizome_manifest_version_cache_slot {
  unsigned char idprefix[24];
  long long version;
};

struct rhizome_manifest_version_cache_slot rhizome_manifest_version_cache[RHIZOME_VERSION_CACHE_SIZE][RHIZOME_VERSION_CACHE_ASSOCIATIVITY];

int rhizome_manifest_version_cache_store(rhizome_manifest *m)
{
  int bin=0;
  int slot;
  int i;

  char *id=rhizome_manifest_get(m,"id",NULL,0);
  if (!id) return 1; // dodgy manifest, so don't suggest that we want to RX it.

  /* Work out bin number in cache */
  for(i=0;i<RHIZOME_VERSION_CACHE_NYBLS;i++)
    {
      int nybl=hexvalue(id[i]);
      bin=(bin<<4)|nybl;
    }
  bin=bin>>RHIZOME_VERSION_CACHE_SHIFT;

  slot=random()%RHIZOME_VERSION_CACHE_ASSOCIATIVITY;
  struct rhizome_manifest_version_cache_slot *entry
    =&rhizome_manifest_version_cache[bin][slot];
  unsigned long long manifest_version = rhizome_manifest_get_ll(m,"version");

  entry->version=manifest_version;
  for(i=0;i<24;i++)
    {
      int byte=(hexvalue(id[(i*2)])<<4)|hexvalue(id[(i*2)+1]);
      entry->idprefix[i]=byte;
    }

  return 0;
}

int rhizome_manifest_version_cache_lookup(rhizome_manifest *m)
{
  int bin=0;
  int slot;
  int i;

  char id[RHIZOME_MANIFEST_ID_STRLEN + 1];
  if (!rhizome_manifest_get(m, "id", id, sizeof id))
    // dodgy manifest, we don't want to receive it
    return WHY("Ignoring bad manifest (no ID field)");
  str_toupper_inplace(id);
  m->version = rhizome_manifest_get_ll(m, "version");
  
  // skip the cache for now
  long long dbVersion = -1;
  if (sqlite_exec_int64(&dbVersion, "SELECT version FROM MANIFESTS WHERE id='%s';", id) == -1)
    return WHY("Select failure");
  if (dbVersion >= m->version) {
    if (0) WHYF("We already have %s (%lld vs %lld)", id, dbVersion, m->version);
    return -1;
  }
  return 0;

  /* Work out bin number in cache */
  for(i=0;i<RHIZOME_VERSION_CACHE_NYBLS;i++)
    {
      int nybl=hexvalue(id[i]);
      bin=(bin<<4)|nybl;
    }
  bin=bin>>RHIZOME_VERSION_CACHE_SHIFT;
  
  for(slot=0;slot<RHIZOME_VERSION_CACHE_ASSOCIATIVITY;slot++)
    {
      struct rhizome_manifest_version_cache_slot *entry
	=&rhizome_manifest_version_cache[bin][slot];
      for(i=0;i<24;i++)
	{
	  int byte=
	    (hexvalue(id[(i*2)])<<4)
	    |hexvalue(id[(i*2)+1]);
	  if (byte!=entry->idprefix[i]) break;
	}
      if (i==24) {
	/* Entries match -- so check version */
	long long rev = rhizome_manifest_get_ll(m,"version");
	if (1) DEBUGF("cached version %lld vs manifest version %lld", entry->version,rev);
	if (rev > entry->version) {
	  /* If we only have an old version, try refreshing the cache
	     by querying the database */
	  if (sqlite_exec_int64(&entry->version, "select version from manifests where id='%s'", id) != 1)
	    return WHY("failed to select stored manifest version");
	  DEBUGF("Refreshed stored version from database: entry->version=%lld", entry->version);
	}
	if (rev < entry->version) {
	  /* the presented manifest is older than we have.
	     This allows the caller to know that they can tell whoever gave them the
	     manifest it's time to get with the times.  May or not ever be
	     implemented, but it would be nice. XXX */
	  WHYF("cached version is NEWER than presented version (%lld is newer than %lld)",
	      entry->version,rev);
	  return -2;
	} else if (rev<=entry->version) {
	  /* the presented manifest is already stored. */	   
	  if (1) DEBUG("cached version is NEWER/SAME as presented version");
	  return -1;
	} else {
	  /* the presented manifest is newer than we have */
	  DEBUG("cached version is older than presented version");
	  return 0;
	}
      }
    }

  DEBUG("Not in manifest cache");

  /* Not in cache, so all is well, well, maybe.
     What we do know is that it is unlikely to be in the database, so it probably
     doesn't hurt to try to receive it.  

     Of course, we can just ask the database if it is there already, and populate
     the cache in the process if we find it.  The tradeoff is that the whole point
     of the cache is to AVOID database lookups, not incurr them whenever the cache
     has a negative result.  But if we don't ask the database, then we can waste
     more effort fetching the file associated with the manifest, and will ultimately
     incurr a database lookup (and more), so while it seems a little false economy
     we need to do the lookup now.

     What this all suggests is that we need fairly high associativity so that misses
     are rare events. But high associativity then introduces a linear search cost,
     although that is unlikely to be nearly as much cost as even thinking about a
     database query.

     It also says that on a busy network that things will eventually go pear-shaped
     and require regular database queries, and that memory allowing, we should use
     a fairly large cache here.
 */
  long long manifest_version = rhizome_manifest_get_ll(m, "version");
  long long count;
  switch (sqlite_exec_int64(&count, "select count(*) from manifests where id='%s' and version>=%lld", id, manifest_version)) {
    case -1:
      return WHY("database error reading stored manifest version");
    case 1:
      if (count) {
	/* Okay, we have a stored version which is newer, so update the cache
	  using a random replacement strategy. */
	long long stored_version;
	if (sqlite_exec_int64(&stored_version, "select version from manifests where id='%s'", id) < 1)
	  return WHY("database error reading stored manifest version"); // database is broken, we can't confirm that it is here
	DEBUGF("stored version=%lld, manifest_version=%lld (not fetching; remembering in cache)",
	    stored_version,manifest_version);
	slot=random()%RHIZOME_VERSION_CACHE_ASSOCIATIVITY;
	struct rhizome_manifest_version_cache_slot *entry = &rhizome_manifest_version_cache[bin][slot];
	entry->version=stored_version;
	for(i=0;i<24;i++)
	  {
	    int byte=(hexvalue(id[(i*2)])<<4)|hexvalue(id[(i*2)+1]);
	    entry->idprefix[i]=byte;
	  }
	/* Finally, say that it isn't worth RXing this manifest */
	return stored_version > manifest_version ? -2 : -1;
      }
      break;
    default:
      return WHY("bad select result");
  }
  /* At best we hold an older version of this manifest, and at worst we
     don't hold any copy. */
  return 0;
}

typedef struct ignored_manifest {
  unsigned char bid[crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES];
  struct sockaddr_in peer;
  time_ms_t timeout;
} ignored_manifest;

#define IGNORED_BIN_SIZE 8
#define IGNORED_BIN_COUNT 64
#define IGNORED_BIN_BITS 6
typedef struct ignored_manifest_bin {
  int bins_used;
  ignored_manifest m[IGNORED_BIN_SIZE];
} ignored_manifest_bin;

typedef struct ignored_manifest_cache {
  ignored_manifest_bin bins[IGNORED_BIN_COUNT];
} ignored_manifest_cache;

/* used uninitialised, since the probability of
   a collision is exceedingly remote */
ignored_manifest_cache ignored;

int rhizome_ignore_manifest_check(rhizome_manifest *m, const struct sockaddr_in *peerip)
{
  int bin = m->cryptoSignPublic[0]>>(8-IGNORED_BIN_BITS);
  int slot;
  for(slot = 0; slot != IGNORED_BIN_SIZE; ++slot)
    {
      if (!memcmp(ignored.bins[bin].m[slot].bid,
		  m->cryptoSignPublic,
		  crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES))
	{
	  if (ignored.bins[bin].m[slot].timeout>gettime_ms())
	    return 1;
	  else 
	    return 0;
	}
    }
  return 0;
}

int rhizome_queue_ignore_manifest(rhizome_manifest *m, const struct sockaddr_in *peerip, int timeout)
{
  /* The supplied manifest from a given IP has errors, so remember 
     that it isn't worth considering */
  int bin = m->cryptoSignPublic[0]>>(8-IGNORED_BIN_BITS);
  int slot;
  for(slot = 0; slot != IGNORED_BIN_SIZE; ++slot)
    {
      if (!memcmp(ignored.bins[bin].m[slot].bid,
		  m->cryptoSignPublic,
		  crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES))
	break;
    }
  if (slot>=IGNORED_BIN_SIZE) slot=random()%IGNORED_BIN_SIZE;
  bcopy(&m->cryptoSignPublic[0],
	&ignored.bins[bin].m[slot].bid[0],
	crypto_sign_edwards25519sha512batch_PUBLICKEYBYTES);
  /* ignore for a while */
  ignored.bins[bin].m[slot].timeout=gettime_ms()+timeout;
  bcopy(peerip,
	&ignored.bins[bin].m[slot].peer,
	sizeof(struct sockaddr_in));
  return 0;

}

static int rhizome_import_received_bundle(struct rhizome_manifest *m)
{
  m->finalised = 1;
  m->manifest_bytes = m->manifest_all_bytes; // store the signatures too
  if (debug & DEBUG_RHIZOME_RX) {
    DEBUGF("manifest len=%d has %d signatories", m->manifest_bytes, m->sig_count);
    dump("manifest", m->manifestdata, m->manifest_all_bytes);
  }
  return rhizome_bundle_import(m, m->ttl - 1 /* TTL */);
}

/* Queue a fetch for the payload of the given manifest.  If 'peerip' is not NULL, then it is used as
 * the port and IP address of an HTTP server from which the fetch is performed.  Otherwise the fetch
 * is performed over MDP.
 *
 * If the fetch cannot be queued for any reason (error, queue full, no suitable queue) then the
 * manifest is freed and returns -1.  Otherwise, the pointer to the manifest is stored in the queue
 * entry and the manifest is freed when the fetch has completed or is abandoned for any reason.
 *
 * Verifies manifests as late as possible to avoid wasting time.
 */
int rhizome_suggest_queue_manifest_import(rhizome_manifest *m, const struct sockaddr_in *peerip)
{
  IN();
  const char *bid = alloca_tohex_bid(m->cryptoSignPublic);
  int priority=100; /* normal priority */

  if (debug & DEBUG_RHIZOME_RX)
    DEBUGF("Considering manifest import bid=%s version=%lld size=%lld priority=%d:", bid, m->version, m->fileLength, priority);

  if (rhizome_manifest_version_cache_lookup(m)) {
    if (debug & DEBUG_RHIZOME_RX)
      DEBUG("   already have that version or newer");
    rhizome_manifest_free(m);
    RETURN(-1);
  }

  if (debug & DEBUG_RHIZOME_RX) {
    long long stored_version;
    if (sqlite_exec_int64(&stored_version, "select version from manifests where id='%s'", bid) > 0)
      DEBUGF("   is new (have version %lld)", stored_version);
  }

  if (m->fileLength == 0) {
    if (rhizome_manifest_verify(m) != 0) {
      WHY("Error verifying manifest when considering for import");
      /* Don't waste time looking at this manifest again for a while */
      rhizome_queue_ignore_manifest(m, peerip, 60000);
      rhizome_manifest_free(m);
      RETURN(-1);
    }
    rhizome_import_received_bundle(m);
    RETURN(0);
  }

  // Find the proper queue for the payload.  If there is none suitable, it is an error.
  struct rhizome_fetch_queue *qi = rhizome_find_queue(m->fileLength);
  if (!qi) {
    WHYF("No suitable fetch queue for bundle size=%lld", m->fileLength);
    rhizome_manifest_free(m);
    RETURN(-1);
  }

  // Search all the queues for the same manifest (it could be in any queue because its payload size
  // may have changed between versions.) If a newer or the same version is already queued, then
  // ignore this one.  Otherwise, unqueue all older candidates.
  int ci = -1;
  int i, j;
  for (i = 0; i < NQUEUES; ++i) {
    struct rhizome_fetch_queue *q = &rhizome_fetch_queues[i];
    for (j = 0; j < q->candidate_queue_size; ) {
      struct rhizome_fetch_candidate *c = &q->candidate_queue[j];
      if (c->manifest) {
	if (memcmp(m->cryptoSignPublic, c->manifest->cryptoSignPublic, RHIZOME_MANIFEST_ID_BYTES) == 0) {
	  if (c->manifest->version >= m->version) {
	    rhizome_manifest_free(m);
	    RETURN(0);
	  }
	  if (!m->selfSigned && rhizome_manifest_verify(m)) {
	    WHY("Error verifying manifest when considering queuing for import");
	    /* Don't waste time looking at this manifest again for a while */
	    rhizome_queue_ignore_manifest(m, peerip, 60000);
	    rhizome_manifest_free(m);
	    RETURN(-1);
	  }
	  rhizome_fetch_unqueue(q, j);
	} else {
	  if (ci == -1 && q == qi && c->priority < priority)
	    ci = j;
	  ++j;
	}
      } else {
	if (ci == -1 && q == qi)
	  ci = j;
	break;
      }
    }
  }
  // No duplicate was found, so if no free queue place was found either then bail out.
  if (ci == -1) {
    rhizome_manifest_free(m);
    RETURN(1);
  }

  if (!m->selfSigned && rhizome_manifest_verify(m)) {
    WHY("Error verifying manifest when considering queuing for import");
    /* Don't waste time looking at this manifest again for a while */
    rhizome_queue_ignore_manifest(m, peerip, 60000);
    rhizome_manifest_free(m);
    RETURN(-1);
  }

  struct rhizome_fetch_candidate *c = rhizome_fetch_insert(qi, j);
  c->manifest = m;
  c->peer = *peerip;
  c->priority = priority;

  if (debug & DEBUG_RHIZOME_RX) {
    DEBUG("Rhizome fetch queues:");
    int i, j;
    for (i = 0; i < NQUEUES; ++i) {
      struct rhizome_fetch_queue *q = &rhizome_fetch_queues[i];
      for (j = 0; j < q->candidate_queue_size; ++j) {
	struct rhizome_fetch_candidate *c = &q->candidate_queue[j];
	if (!c->manifest)
	  break;
	DEBUGF("%d:%d manifest=%p bid=%s priority=%d size=%lld", i, j,
	    c->manifest,
	    alloca_tohex_bid(c->manifest->cryptoSignPublic),
	    c->priority,
	    (long long) c->manifest->fileLength
	  );
      }
    }
  }

  RETURN(0);
}

static void rhizome_start_next_queued_fetch(struct rhizome_fetch_queue *q)
{
  struct rhizome_fetch_candidate *c = &q->candidate_queue[0];
  int result;
  while (c->manifest && (result = rhizome_fetch(c->manifest, &c->peer)) != SLOTBUSY) {
    if (result == STARTED)
      c->manifest = NULL;
    rhizome_fetch_unqueue(q, 0);
  }
}

void rhizome_start_next_queued_fetches(struct sched_ent *alarm)
{
  int i;
  for (i = 0; i < NQUEUES; ++i)
    rhizome_start_next_queued_fetch(&rhizome_fetch_queues[i]);
  alarm->alarm = gettime_ms() + rhizome_fetch_interval_ms;
  alarm->deadline = alarm->alarm + rhizome_fetch_interval_ms*3;
  schedule(alarm);
}

static int rhizome_start_fetch(struct rhizome_fetch_slot *slot)
{
  int sock = -1;
  FILE *file = NULL;
  /* TODO Don't forget to implement resume */
  /* TODO We should stream file straight into the database */
  if (create_rhizome_import_dir() == -1)
    goto bail;
  if ((file = fopen(slot->filename, "w")) == NULL) {
    WHYF_perror("fopen(`%s`, \"w\")", slot->filename);
    goto bail;
  }
  if (slot->peer.sin_family == AF_INET) {
    /* Transfer via HTTP over IPv4 */
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      WHY_perror("socket");
      goto bail;
    }
    if (set_nonblock(sock) == -1)
      goto bail;
    char buf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &slot->peer.sin_addr, buf, sizeof buf) == NULL) {
      buf[0] = '*';
      buf[1] = '\0';
    }
    if (connect(sock, (struct sockaddr*)&slot->peer, sizeof slot->peer) == -1) {
      if (errno == EINPROGRESS) {
	if (debug & DEBUG_RHIZOME_RX)
	  DEBUGF("connect() returned EINPROGRESS");
      } else {
	WHYF_perror("connect(%d, %s:%u)", sock, buf, ntohs(slot->peer.sin_port));
	goto bail;
      }
    }
    INFOF("RHIZOME HTTP REQUEST family=%u addr=%s port=%u %s",
	slot->peer.sin_family, buf, ntohs(slot->peer.sin_port), alloca_str_toprint(slot->request)
      );
    slot->alarm.poll.fd = sock;
    slot->request_ofs=0;
    slot->state=RHIZOME_FETCH_CONNECTING;
    slot->file = file;
    slot->file_len=-1;
    slot->file_ofs=0;
    /* Watch for activity on the socket */
    slot->alarm.function=rhizome_fetch_poll;
    fetch_stats.name="rhizome_fetch_poll";
    slot->alarm.stats=&fetch_stats;
    slot->alarm.poll.events=POLLIN|POLLOUT;
    watch(&slot->alarm);
    /* And schedule a timeout alarm */
    slot->alarm.alarm=gettime_ms() + RHIZOME_IDLE_TIMEOUT;
    slot->alarm.deadline = slot->alarm.alarm + RHIZOME_IDLE_TIMEOUT;
    schedule(&slot->alarm);
    return 0;
  } else {
    /* TODO: Fetch via overlay */
    WHY("Rhizome fetching via overlay not implemented");
  }
bail:
  if (sock != -1)
    close(sock);
  if (file != NULL) {
    fclose(file);
    unlink(slot->filename);
  }
  return -1;
}

/* Returns STARTED (0) if the fetch was started (the caller should not free the manifest in this
 * case, because the fetch slot now has a copy of the pointer, and the manifest will be freed once
 * the fetch finishes or is terminated).
 * Returns SAMEPAYLOAD if a fetch of the same payload (file ID) is already active.
 * Returns SUPERSEDED if the fetch was not started because a newer version of the same bundle is
 * already present.
 * Returns SLOTBUSY if the fetch was not queued because no slots are available.
 * Returns SAMEBUNDLE if a fetch of the same bundle is already active.
 * Returns OLDERBUNDLE if a fetch of an older version of the same bundle is already active.
 * Returns NEWERBUNDLE if a fetch of a newer version of the same bundle is already active.
 * Returns IMPORTED if a fetch was not started because the payload is nil or already in the
 * Rhizome store, so the import was performed instead.
 * Returns -1 on error.
 */
static int rhizome_fetch(rhizome_manifest *m, const struct sockaddr_in *peerip)
{
  const char *bid = alloca_tohex_bid(m->cryptoSignPublic);

  /* Do the quick rejection tests first, before the more expensive ones,
     like querying the database for manifests.

     We probably need a cache of recently rejected manifestid:versionid
     pairs so that we can avoid database lookups in most cases.  Probably
     the first 64bits of manifestid is sufficient to make it resistant to
     collission attacks, but using 128bits or the full 256 bits would be safer.
     Let's make the cache use 256 bit (32byte) entries for power of two
     efficiency, and so use the last 64bits for version id, thus using 192 bits
     for collision avoidance --- probably sufficient for many years yet (from
     time of writing in 2012).  We get a little more than 192 bits by using
     the cache slot number to implicitly store the first bits.
  */

  if (1||(debug & DEBUG_RHIZOME_RX))
    DEBUGF("Fetching bundle bid=%s version=%lld size=%lld peerip=%s", bid, m->version, m->fileLength, alloca_sockaddr(peerip));

  // If the payload is empty, no need to fetch, so import now.
  if (m->fileLength == 0) {
    if (debug & DEBUG_RHIZOME_RX)
      DEBUGF("   manifest fetch not started -- nil payload, so importing instead");
    if (rhizome_import_received_bundle(m) == -1)
      return WHY("bundle import failed");
    return IMPORTED;
  }

  // Ensure there is a slot available before doing more expensive checks.
  struct rhizome_fetch_slot *slot = rhizome_find_fetch_slot(m->fileLength);
  if (slot == NULL) {
    if (debug & DEBUG_RHIZOME_RX)
      DEBUG("   fetch not started - all slots full");
    return SLOTBUSY;
  }

  // If we already have this version or newer, do not fetch.
  if (rhizome_manifest_version_cache_lookup(m)) {
    if (debug & DEBUG_RHIZOME_RX)
      DEBUG("   fetch not started -- already have that version or newer");
    return SUPERSEDED;
  }
  if (debug & DEBUG_RHIZOME_RX)
    DEBUGF("   is new");

  /* Don't fetch if already in progress.  If a fetch of an older version is already in progress,
   * then this logic will let it run to completion before the fetch of the newer version is queued.
   * This avoids the problem of indefinite postponement of fetching if new versions are constantly
   * being published faster than we can fetch them.
   */
  int i;
  for (i = 0; i < NQUEUES; ++i) {
    struct rhizome_fetch_slot *as = &rhizome_fetch_queues[i].active;
    const rhizome_manifest *am = as->manifest;
    if (as->state != RHIZOME_FETCH_FREE && memcmp(m->cryptoSignPublic, am->cryptoSignPublic, RHIZOME_MANIFEST_ID_BYTES) == 0) {
      if (am->version < m->version) {
	if (debug & DEBUG_RHIZOME_RX)
	  DEBUGF("   fetch already in progress -- older version");
	return OLDERBUNDLE;
      } else if (am->version > m->version) {
	if (debug & DEBUG_RHIZOME_RX)
	  DEBUGF("   fetch already in progress -- newer version");
	return NEWERBUNDLE;
      } else {
	if (debug & DEBUG_RHIZOME_RX)
	  DEBUGF("   fetch already in progress -- same version");
	return SAMEBUNDLE;
      }
    }
  }

  if (!m->fileHashedP)
    return WHY("Manifest missing filehash");

  // If the payload is already available, no need to fetch, so import now.
  long long gotfile = 0;
  if (sqlite_exec_int64(&gotfile, "SELECT COUNT(*) FROM FILES WHERE ID='%s' and datavalid=1;", m->fileHexHash) != 1)
    return WHY("select failed");
  if (gotfile) {
    if (debug & DEBUG_RHIZOME_RX)
      DEBUGF("   fetch not started - payload already present, so importing instead");
    if (rhizome_bundle_import(m, m->ttl-1) == -1)
      return WHY("bundle import failed");
    return IMPORTED;
  }

  // Fetch the file, unless already queued.
  for (i = 0; i < NQUEUES; ++i) {
    struct rhizome_fetch_slot *as = &rhizome_fetch_queues[i].active;
    const rhizome_manifest *am = as->manifest;
    if (as->state != RHIZOME_FETCH_FREE && strcasecmp(m->fileHexHash, am->fileHexHash) == 0) {
      if (debug & DEBUG_RHIZOME_RX)
	DEBUGF("   fetch already in progress, slot=%d filehash=%s", i, m->fileHexHash);
      return SAMEPAYLOAD;
    }
  }

  // Start the fetch.
  //dump("peerip", peerip, sizeof *peerip);
  slot->peer = *peerip;
  strbuf r = strbuf_local(slot->request, sizeof slot->request);
  strbuf_sprintf(r, "GET /rhizome/file/%s HTTP/1.0\r\n\r\n", m->fileHexHash);
  if (strbuf_overrun(r))
    return WHY("request overrun");
  slot->request_len = strbuf_len(r);
  if (!FORM_RHIZOME_IMPORT_PATH(slot->filename, "payload.%s", bid))
    return -1;
  m->dataFileName = strdup(slot->filename);
  m->dataFileUnlinkOnFree = 0;
  slot->manifest = m;
  if (rhizome_start_fetch(slot) == -1) {
    slot->filename[0] = '\0';
    return -1;
  }
  if (debug & DEBUG_RHIZOME_RX) 
    DEBUGF("   started fetch into %s, slot=%d filehash=%s", slot->manifest->dataFileName, slot - &rhizome_fetch_queues[0].active, m->fileHexHash);
  return STARTED;
}

int rhizome_fetch_request_manifest_by_prefix(const struct sockaddr_in *peerip, const unsigned char *prefix, size_t prefix_length)
{
  assert(peerip);
  struct rhizome_fetch_slot *slot = rhizome_find_fetch_slot(MAX_MANIFEST_BYTES);
  if (slot == NULL)
    return SLOTBUSY;
  slot->peer = *peerip;
  slot->manifest = NULL;
  strbuf r = strbuf_local(slot->request, sizeof slot->request);
  strbuf_sprintf(r, "GET /rhizome/manifestbyprefix/%s HTTP/1.0\r\n\r\n", alloca_tohex(prefix, prefix_length));
  if (strbuf_overrun(r))
    return WHY("request overrun");
  slot->request_len = strbuf_len(r);
  if (!FORM_RHIZOME_IMPORT_PATH(slot->filename, "manifest.%s", alloca_tohex(prefix, prefix_length)))
    return -1;
  if (rhizome_start_fetch(slot) == -1) {
    slot->filename[0] = '\0';
    return -1;
  }
  return STARTED;
}

static int rhizome_fetch_close(struct rhizome_fetch_slot *slot)
{
  if (debug & DEBUG_RHIZOME_RX) 
    DEBUGF("Close Rhizome fetch slot=%d", slot - &rhizome_fetch_queues[0].active);
  assert(slot->state != RHIZOME_FETCH_FREE);

  /* close socket and stop watching it */
  unwatch(&slot->alarm);
  unschedule(&slot->alarm);
  close(slot->alarm.poll.fd);
  slot->alarm.poll.fd = -1;

  /* Free ephemeral data */
  if (slot->file)
    fclose(slot->file);
  slot->file = NULL;
  if (slot->manifest)
    rhizome_manifest_free(slot->manifest);
  slot->manifest = NULL;
  if (slot->filename[0])
    unlink(slot->filename);
  slot->filename[0] = '\0';

  // Release the fetch slot.
  slot->state = RHIZOME_FETCH_FREE;

  // Activate the next queued fetch that is eligible for this slot.  Try starting candidates from
  // all queues with the same or smaller size thresholds until the slot is taken.
  struct rhizome_fetch_queue *q;
  for (q = (struct rhizome_fetch_queue *) slot; slot->state == RHIZOME_FETCH_FREE && q >= rhizome_fetch_queues; --q)
    rhizome_start_next_queued_fetch(q);

  return 0;
}

void rhizome_fetch_write(struct rhizome_fetch_slot *slot)
{
  if (debug & DEBUG_RHIZOME_RX)
    DEBUGF("write_nonblock(%d, %s)", slot->alarm.poll.fd, alloca_toprint(-1, &slot->request[slot->request_ofs], slot->request_len-slot->request_ofs));
  int bytes = write_nonblock(slot->alarm.poll.fd, &slot->request[slot->request_ofs], slot->request_len-slot->request_ofs);
  if (bytes == -1) {
    WHY("Got error while sending HTTP request.  Closing.");
    rhizome_fetch_close(slot);
  } else {
    // reset timeout
    unschedule(&slot->alarm);
    slot->alarm.alarm=gettime_ms() + RHIZOME_IDLE_TIMEOUT;
    slot->alarm.deadline = slot->alarm.alarm + RHIZOME_IDLE_TIMEOUT;
    schedule(&slot->alarm);
    slot->request_ofs+=bytes;
    if (slot->request_ofs>=slot->request_len) {
      /* Sent all of request.  Switch to listening for HTTP response headers.
       */
      slot->request_len=0; slot->request_ofs=0;
      slot->state=RHIZOME_FETCH_RXHTTPHEADERS;
      slot->alarm.poll.events=POLLIN;
      watch(&slot->alarm);
    }else if(slot->state==RHIZOME_FETCH_CONNECTING)
      slot->state = RHIZOME_FETCH_SENDINGHTTPREQUEST;
  }
}

void rhizome_write_content(struct rhizome_fetch_slot *slot, char *buffer, int bytes)
{
  if (bytes>(slot->file_len-slot->file_ofs))
    bytes=slot->file_len-slot->file_ofs;
  if (fwrite(buffer,bytes,1,slot->file) != 1) {
    if (debug & DEBUG_RHIZOME_RX)
      DEBUGF("Failed to write %d bytes to file @ offset %d", bytes, slot->file_ofs);
    rhizome_fetch_close(slot);
    return;
  }
  slot->file_ofs+=bytes;
  if (slot->file_ofs>=slot->file_len) {
    /* got all of file */
    if (debug & DEBUG_RHIZOME_RX)
      DEBUGF("Received all of file via rhizome -- now to import it");
    fclose(slot->file);
    slot->file = NULL;
    if (slot->manifest) {
      // Were fetching payload, now we have it.
      rhizome_import_received_bundle(slot->manifest);
    } else {
      /* This was to fetch the manifest, so now fetch the file if needed */
      DEBUGF("Received a manifest in response to supplying a manifest prefix.");
      /* Read the manifest and add it to suggestion queue, then immediately
	 call schedule queued items. */
      rhizome_manifest *m = rhizome_new_manifest();
      if (m) {
	if (rhizome_read_manifest_file(m, slot->filename, 0) == -1) {
	  DEBUGF("Couldn't read manifest from %s",slot->filename);
	  rhizome_manifest_free(m);
	} else {
	  DEBUGF("All looks good for importing manifest id=%s", alloca_tohex_bid(m->cryptoSignPublic));
	  dump("slot->peer",&slot->peer,sizeof(slot->peer));
	  rhizome_suggest_queue_manifest_import(m, &slot->peer);
	}
      }
    }
    rhizome_fetch_close(slot);
    return;
  }
  // reset inactivity timeout
  unschedule(&slot->alarm);
  slot->alarm.alarm=gettime_ms() + RHIZOME_IDLE_TIMEOUT;
  slot->alarm.deadline = slot->alarm.alarm+RHIZOME_IDLE_TIMEOUT;
  schedule(&slot->alarm);
}

void rhizome_fetch_poll(struct sched_ent *alarm)
{
  struct rhizome_fetch_slot *slot = (struct rhizome_fetch_slot *) alarm;

  if (alarm->poll.revents & (POLLIN | POLLOUT)) {
    switch (slot->state) {
      case RHIZOME_FETCH_CONNECTING:
      case RHIZOME_FETCH_SENDINGHTTPREQUEST:
	rhizome_fetch_write(slot);
	return;
      case RHIZOME_FETCH_RXFILE: {
	  /* Keep reading until we have the promised amount of data */
	  char buffer[8192];
	  sigPipeFlag = 0;
	  int bytes = read_nonblock(slot->alarm.poll.fd, buffer, sizeof buffer);
	  /* If we got some data, see if we have found the end of the HTTP request */
	  if (bytes > 0) {
	    rhizome_write_content(slot, buffer, bytes);
	    return;
	  } else {
	    if (debug & DEBUG_RHIZOME_RX)
	      DEBUG("Empty read, closing connection");
	    rhizome_fetch_close(slot);
	    return;
	  }
	  if (sigPipeFlag) {
	    if (debug & DEBUG_RHIZOME_RX)
	      DEBUG("Received SIGPIPE, closing connection");
	    rhizome_fetch_close(slot);
	    return;
	  }
	}
	break;
      case RHIZOME_FETCH_RXHTTPHEADERS: {
	  /* Keep reading until we have two CR/LFs in a row */
	  sigPipeFlag = 0;
	  int bytes = read_nonblock(slot->alarm.poll.fd, &slot->request[slot->request_len], 1024 - slot->request_len - 1);
	  /* If we got some data, see if we have found the end of the HTTP reply */
	  if (bytes > 0) {
	    // reset timeout
	    unschedule(&slot->alarm);
	    slot->alarm.alarm = gettime_ms() + RHIZOME_IDLE_TIMEOUT;
	    slot->alarm.deadline = slot->alarm.alarm + RHIZOME_IDLE_TIMEOUT;
	    schedule(&slot->alarm);
	    slot->request_len += bytes;
	    if (http_header_complete(slot->request, slot->request_len, bytes)) {
	      if (debug & DEBUG_RHIZOME_RX)
		DEBUGF("Got HTTP reply: %s", alloca_toprint(160, slot->request, slot->request_len));
	      /* We have all the reply headers, so parse them, taking care of any following bytes of
		content. */
	      struct http_response_parts parts;
	      if (unpack_http_response(slot->request, &parts) == -1) {
		if (debug & DEBUG_RHIZOME_RX)
		  DEBUGF("Failed HTTP request: failed to unpack http response");
		rhizome_fetch_close(slot);
		return;
	      }
	      if (parts.code != 200) {
		if (debug & DEBUG_RHIZOME_RX)
		  DEBUGF("Failed HTTP request: rhizome server returned %d != 200 OK", parts.code);
		rhizome_fetch_close(slot);
		return;
	      }
	      if (parts.content_length == -1) {
		if (debug & DEBUG_RHIZOME_RX)
		  DEBUGF("Invalid HTTP reply: missing Content-Length header");
		rhizome_fetch_close(slot);
		return;
	      }
	      slot->file_len = parts.content_length;
	      /* We have all we need.  The file is already open, so just write out any initial bytes of
		the body we read.
	      */
	      slot->state = RHIZOME_FETCH_RXFILE;
	      int content_bytes = slot->request + slot->request_len - parts.content_start;
	      if (content_bytes > 0){
		rhizome_write_content(slot, parts.content_start, content_bytes);
		return;
	      }
	    }
	  }
	  break;
	default:
	  if (debug & DEBUG_RHIZOME_RX)
	    DEBUG("Closing rhizome fetch connection due to illegal/unimplemented state.");
	  rhizome_fetch_close(slot);
	  return;
	}
    }
  }
  
  if (alarm->poll.revents==0 || alarm->poll.revents & (POLLHUP | POLLERR)){
    // timeout or socket error, close the socket
    if (debug & DEBUG_RHIZOME_RX)
      DEBUGF("Closing due to timeout or error %x (%x %x)", alarm->poll.revents, POLLHUP, POLLERR);
    rhizome_fetch_close(slot);
  }
  
}

/*
   This function takes a pointer to a buffer into which the entire HTTP response header has been
   read.  The caller must have ensured that the buffer contains at least one consecutive pair of
   newlines '\n', optionally with carriage returns '\r' preceding and optionally interspersed with
   nul characters '\0' (which can originate from telnet).  The http_header_complete() function
   is useful for this.
   This returns pointers to within the supplied buffer, and may overwrite some characters in the
   buffer, for example to nul-terminate a string that was terminated by space ' ' or newline '\r'
   '\n' in the buffer.  For that reason, it takes char* not const char* arguments and returns the
   same.  It is up to the caller to manage the lifetime of the returned pointers, which of course
   will only be valid for as long as the buffer persists and is not overwritten.
   @author Andrew Bettison <andrew@servalproject.com>
 */
int unpack_http_response(char *response, struct http_response_parts *parts)
{
  parts->code = -1;
  parts->reason = NULL;
  parts->content_length = -1;
  parts->content_start = NULL;
  char *p = NULL;
  if (!str_startswith(response, "HTTP/1.0 ", &p)) {
    if (debug&DEBUG_RHIZOME_RX)
      DEBUGF("Malformed HTTP reply: missing HTTP/1.0 preamble");
    return -1;
  }
  if (!(isdigit(p[0]) && isdigit(p[1]) && isdigit(p[2]) && p[3] == ' ')) {
    if (debug&DEBUG_RHIZOME_RX)
      DEBUGF("Malformed HTTP reply: missing three-digit status code");
    return -1;
  }
  parts->code = (p[0]-'0') * 100 + (p[1]-'0') * 10 + p[2]-'0';
  p += 4;
  parts->reason = p;
  while (*p != '\n')
    ++p;
  if (p[-1] == '\r')
    p[-1] = '\0';
  *p++ = '\0';
  // Iterate over header lines until the last blank line.
  while (!(p[0] == '\n' || (p[0] == '\r' && p[1] == '\n'))) {
    if (strcase_startswith(p, "Content-Length:", &p)) {
      while (*p == ' ')
	++p;
      parts->content_length = 0;
      char *nump = p;
      while (isdigit(*p))
	parts->content_length = parts->content_length * 10 + *p++ - '0';
      if (p == nump || (*p != '\r' && *p != '\n')) {
	if (debug & DEBUG_RHIZOME_RX)
	  DEBUGF("Invalid HTTP reply: malformed Content-Length header");
	return -1;
      }
    }
    while (*p++ != '\n')
      ;
  }
  if (*p == '\r')
    ++p;
  ++p; // skip '\n' at end of blank line
  parts->content_start = p;
  return 0;
}
