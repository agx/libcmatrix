/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
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

#include <glib.h>

#include "cm-client.h"

#define CM_USERNAME_ATTRIBUTE  "username"
#define CM_SERVER_ATTRIBUTE    "server"
#define CM_PROTOCOL_ATTRIBUTE  "protocol"

G_BEGIN_DECLS

void        cm_secret_store_save_async     (CmClient            *client,
                                            char                *access_token,
                                            char                *pickle_key,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data);
gboolean    cm_secret_store_save_finish    (GAsyncResult        *result,
                                            GError             **error);

void        cm_secret_store_load_async     (GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data);
GPtrArray  *cm_secret_store_load_finish    (GAsyncResult        *result,
                                            GError             **error);

void        cm_secret_store_delete_async   (CmClient            *client,
                                            GCancellable        *cancellable,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data);
gboolean    cm_secret_store_delete_finish  (GAsyncResult        *result,
                                            GError             **error);

G_END_DECLS
