/* cm-secret-store.h
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <gio/gio.h>

#include "cm-client.h"

G_BEGIN_DECLS

#define CM_USERNAME_ATTRIBUTE  "username"
#define CM_SERVER_ATTRIBUTE    "server"
#define CM_PROTOCOL_ATTRIBUTE  "protocol"

#define CM_TYPE_SECRET_STORE (cm_secret_store_get_type ())
G_DECLARE_FINAL_TYPE (CmSecretStore, cm_secret_store, CM, SECRET_STORE, GObject)

CmSecretStore *cm_secret_store_new            (void);
void           cm_secret_store_load_async     (CmSecretStore       *self,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data);
GPtrArray     *cm_secret_store_load_finish    (CmSecretStore       *self,
                                               GAsyncResult        *result,
                                               GError             **error);
void           cm_secret_store_save_async     (CmSecretStore       *self,
                                               CmClient            *client,
                                               char                *access_token,
                                               char                *pickle_key,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data);
gboolean       cm_secret_store_save_finish    (CmSecretStore       *self,
                                               GAsyncResult        *result,
                                               GError             **error);
void           cm_secret_store_delete_async   (CmSecretStore       *self,
                                               CmClient            *client,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data);
gboolean       cm_secret_store_delete_finish  (CmSecretStore       *self,
                                               GAsyncResult        *result,
                                               GError             **error);

G_END_DECLS
