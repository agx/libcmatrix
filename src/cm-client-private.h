/*
 * Copyright (C) 2022 Purism SPC
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#if !defined(_CMATRIX_TAKEN) && !defined(CMATRIX_COMPILATION)
# error "Only <cmatrix.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "cm-db-private.h"
#include "cm-enc-private.h"
#include "cm-net-private.h"
#include "cm-types.h"
#include "cm-client.h"

G_BEGIN_DECLS

void        cm_client_enable_as_in_store          (CmClient           *self);
CmClient   *cm_client_new_from_secret             (gpointer            secret_retrievable,
                                                   CmDb                *db);
void        cm_client_save_secrets_async          (CmClient            *self,
                                                   GCancellable        *cancellable,
                                                   GAsyncReadyCallback  callback,
                                                   gpointer             user_data);
gboolean    cm_client_save_secrets_finish         (CmClient            *self,
                                                   GAsyncResult        *result,
                                                   GError             **error);
void        cm_client_delete_secrets_async        (CmClient            *self,
                                                   GCancellable        *cancellable,
                                                   GAsyncReadyCallback  callback,
                                                   gpointer             user_data);
gboolean    cm_client_delete_secrets_finish       (CmClient            *self,
                                                   GAsyncResult        *result,
                                                   GError             **error);
int         cm_client_pop_event_id                (CmClient            *self);
CmDb       *cm_client_get_db                      (CmClient            *self);
CmNet      *cm_client_get_net                     (CmClient            *self);
CmEnc      *cm_client_get_enc                     (CmClient            *self);
void        cm_client_set_db                      (CmClient            *self,
                                                   CmDb                *db);
const char *cm_client_get_filter_id               (CmClient            *self);
void        cm_client_save                        (CmClient            *self);
const char *cm_client_get_next_batch              (CmClient            *self);
CmUserList *cm_client_get_user_list               (CmClient            *self);
void        cm_client_get_file_async              (CmClient              *self,
                                                   const char            *uri,
                                                   GCancellable          *cancellable,
                                                   GFileProgressCallback  progress_callback,
                                                   gpointer               progress_user_data,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
GInputStream *cm_client_get_file_finish           (CmClient              *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
G_END_DECLS
