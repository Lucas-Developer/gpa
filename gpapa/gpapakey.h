/* gpapakey.h - The GNU Privacy Assistant Pipe Access - abstract key object header
 * Copyright (C) 2000, 2001 G-N-U GmbH, http://www.g-n-u.de
 *
 * This file is part of GPAPA.
 *
 * GPAPA is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GPAPA is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GPAPA; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef __GPAPAKEY_H__
#define __GPAPAKEY_H__

#include <glib.h>
#include "gpapatypedefs.h"

#define GPAPA_KEY(obj) ((obj)->key)

typedef struct
{
  char KeyTrust, OwnerTrust;  /* OwnerTrust might get a different type in future versions. */
  gint bits, algorithm;
  GList *uids, *subs;
  char *KeyID, *LocalID, *UserID;
  GDate *CreationDate, *ExpirationDate;
}
GpapaKey;

extern GpapaKey *gpapa_key_new (const gchar *keyID, GpapaCallbackFunc callback,
				gpointer calldata);

extern char *gpapa_key_get_identifier (GpapaKey *key,
                                       GpapaCallbackFunc callback,
                                       gpointer calldata);

extern char *gpapa_key_get_name (GpapaKey *key, GpapaCallbackFunc callback,
				 gpointer calldata);

extern GDate *gpapa_key_get_expiry_date (GpapaKey *key,
					 GpapaCallbackFunc callback,
					 gpointer calldata);

extern GDate *gpapa_key_get_creation_date (GpapaKey *key,
					   GpapaCallbackFunc callback,
					   gpointer calldata);

extern void gpapa_key_set_expiry_date (GpapaKey *key, GDate *date,
				       const gchar *password,
				       GpapaCallbackFunc callback,
				       gpointer calldata);

extern void gpapa_key_set_expiry_time (GpapaKey *key, gint number,
				       char unit, GpapaCallbackFunc callback,
				       gpointer calldata);

extern void gpapa_key_release (GpapaKey *key);

#endif /* __GPAPAKEY_H__ */
