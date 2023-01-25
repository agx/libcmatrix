/* cm-user-private.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

#include <json-glib/json-glib.h>
#include <glib-object.h>
#include <gio/gio.h>

#include "cm-client.h"
#include "cm-device.h"
#include "cm-user.h"

G_BEGIN_DECLS

JsonObject    *cm_user_generate_json         (CmUser        *self);
void           cm_user_set_json_data         (CmUser        *self,
                                              JsonObject    *root);
void           cm_user_set_client            (CmUser        *self,
                                              CmClient      *client);
CmClient      *cm_user_get_client            (CmUser        *self);
void           cm_user_set_user_id           (CmUser        *self,
                                              GRefString    *user_id);
void           cm_user_set_details          (CmUser         *self,
                                             const char     *display_name,
                                             const char     *avatar_url);
GListModel    *cm_user_get_devices          (CmUser         *self);
CmDevice      *cm_user_find_device          (CmUser         *self,
                                             const char     *device_id);
void           cm_user_set_devices          (CmUser         *self,
                                             JsonObject     *root,
                                             gboolean        update_state,
                                             GPtrArray      *added,
                                             GPtrArray      *removed);
void           cm_user_add_one_time_keys    (CmUser         *self,
                                             const char     *room_id,
                                             JsonObject     *root,
                                             GPtrArray      *out_keys);

G_END_DECLS
