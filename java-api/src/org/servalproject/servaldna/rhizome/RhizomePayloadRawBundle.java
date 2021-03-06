/**
 * Copyright (C) 2012-2014 Serval Project Inc.
 *
 * This file is part of Serval Software (http://www.servalproject.org)
 *
 * Serval Software is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This source code is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this source code; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

package org.servalproject.servaldna.rhizome;

import org.servalproject.servaldna.BundleSecret;
import org.servalproject.servaldna.SubscriberId;

import java.io.InputStream;

public class RhizomePayloadRawBundle {

	public final RhizomeManifest manifest;
	public final InputStream rawPayloadInputStream;
	public final Long rowId;
	public final Long insertTime;
	public final SubscriberId author;
	public final BundleSecret secret;

	protected RhizomePayloadRawBundle(RhizomeManifest manifest,
									  InputStream rawPayloadInputStream,
									  Long rowId,
									  Long insertTime,
									  SubscriberId author,
									  BundleSecret secret)
	{
		this.rawPayloadInputStream = rawPayloadInputStream;
		this.manifest = manifest;
		this.rowId = rowId;
		this.insertTime = insertTime;
		this.author = author;
		this.secret = secret;
	}

}
