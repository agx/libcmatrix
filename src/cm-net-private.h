/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-net-private.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "cm-enc-private.h"

G_BEGIN_DECLS

#define CM_TYPE_NET (cm_net_get_type ())

G_DECLARE_FINAL_TYPE (CmNet, cm_net, CM, NET, GObject)

CmNet         *cm_net_new                 (void);
void           cm_net_set_homeserver      (CmNet                 *self,
                                           const char            *homeserver);
void           cm_net_set_access_token    (CmNet                 *self,
                                           const char            *access_token);
const char    *cm_net_get_access_token    (CmNet                 *self);
void           cm_net_send_data_async     (CmNet                 *self,
                                           int                    priority,
                                           char                  *data,
                                           gsize                  size,
                                           const char            *uri_path,
                                           const char            *method, /* interned */
                                           GHashTable            *query,
                                           GCancellable          *cancellable,
                                           GAsyncReadyCallback    callback,
                                           gpointer               user_data);
void           cm_net_send_json_async     (CmNet                 *self,
                                           int                    priority,
                                           JsonObject            *object,
                                           const char            *uri_path,
                                           const char            *method, /* interned */
                                           GHashTable            *query,
                                           GCancellable          *cancellable,
                                           GAsyncReadyCallback    callback,
                                           gpointer               user_data);
void           cm_net_get_file_async      (CmNet                 *self,
                                           const char            *uri,
                                           CmEncFileInfo         *file_info,
                                           GCancellable          *cancellable,
                                           GAsyncReadyCallback    callback,
                                           gpointer               user_data);
GInputStream  *cm_net_get_file_finish     (CmNet                 *self,
                                           GAsyncResult          *result,
                                           GError               **error);
void          cm_net_put_file_async       (CmNet                 *self,
                                           GFile                 *file,
                                           gboolean               encrypt,
                                           GFileProgressCallback  progress_callback,
                                           gpointer               progress_user_data,
                                           GCancellable          *cancellable,
                                           GAsyncReadyCallback    callback,
                                           gpointer               user_data);
char         *cm_net_put_file_finish      (CmNet                 *self,
                                           GAsyncResult          *result,
                                           GError               **error);

G_END_DECLS
