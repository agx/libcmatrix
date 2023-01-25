/*
 * Copyright (C) 2022 Purism SPC
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

#include "cm-room.h"

G_BEGIN_DECLS

CmRoom       *cm_room_new                          (const char          *room_id);
CmRoom       *cm_room_new_from_json                (const char          *room_id,
                                                    JsonObject          *root,
                                                    CmEvent             *last_event);
char         *cm_room_get_json                     (CmRoom              *self);
const char   *cm_room_get_replacement_room         (CmRoom              *self);
CmClient     *cm_room_get_client                   (CmRoom              *self);
void          cm_room_set_client                   (CmRoom              *self,
                                                    CmClient            *client);
gboolean      cm_room_has_state_sync               (CmRoom              *self);
GPtrArray    *cm_room_set_data                     (CmRoom              *self,
                                                    JsonObject          *object);
JsonObject   *cm_room_decrypt                      (CmRoom              *self,
                                                    JsonObject          *root);
void          cm_room_add_events                   (CmRoom              *self,
                                                    GPtrArray           *events,
                                                    gboolean             append);
void          cm_room_user_changed                 (CmRoom              *self,
                                                    GPtrArray           *changed_users);
const char   *cm_room_get_prev_batch               (CmRoom              *self);
void          cm_room_set_prev_batch               (CmRoom              *self,
                                                    const char          *prev_batch);
void          cm_room_set_name                     (CmRoom              *self,
                                                    const char          *name);
void          cm_room_set_generated_name           (CmRoom              *self,
                                                    const char          *name);
gint64        cm_room_get_encryption_rotation_time (CmRoom              *self);
CmStatus      cm_room_get_status                   (CmRoom              *self);
void          cm_room_set_status                   (CmRoom              *self,
                                                    CmStatus             status);
guint         cm_room_get_encryption_msg_count     (CmRoom              *self);
gboolean      cm_room_is_direct                    (CmRoom              *self);
void          cm_room_set_is_direct                (CmRoom              *self,
                                                    gboolean             is_direct);
void          cm_room_get_name_async               (CmRoom              *self,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
char         *cm_room_get_name_finish              (CmRoom              *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void          cm_room_load_async                   (CmRoom              *self,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean      cm_room_load_finish                  (CmRoom              *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void          cm_room_save                         (CmRoom              *self);
void          cm_room_load_joined_members_async    (CmRoom              *self,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean      cm_room_load_joined_members_finish   (CmRoom              *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void          cm_room_is_encrypted_async           (CmRoom              *self,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean      cm_room_is_encrypted_finish          (CmRoom              *self,
                                                    GAsyncResult        *result,
                                                   GError             **error);
void          cm_room_load_prev_batch_async        (CmRoom              *self,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
GPtrArray    *cm_room_load_prev_batch_finish       (CmRoom              *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
CmUser       *cm_room_find_user                    (CmRoom              *self,
                                                    GRefString          *matrix_id,
                                                    gboolean             add_if_missing);
void          cm_room_update_user                  (CmRoom              *self,
                                                    CmEvent             *event);

G_END_DECLS
