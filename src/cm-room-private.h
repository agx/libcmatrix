/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
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
const char   *cm_room_get_replacement_room         (CmRoom              *self);
void          cm_room_set_client                   (CmRoom              *self,
                                                    CmClient            *client);
gboolean      cm_room_has_state_sync               (CmRoom              *self);
void          cm_room_set_data                     (CmRoom              *self,
                                                    JsonObject          *object);
void          cm_room_user_changed                 (CmRoom              *self,
                                                    const char          *user_id);
const char   *cm_room_get_prev_batch               (CmRoom              *self);
void          cm_room_set_prev_batch               (CmRoom              *self,
                                                    const char          *prev_batch);
void          cm_room_set_name                     (CmRoom              *self,
                                                    const char          *name);
void          cm_room_set_generated_name           (CmRoom              *self,
                                                    const char          *name);
gint64        cm_room_get_encryption_rotation_time (CmRoom              *self);
CmRoomType    cm_room_get_room_type                (CmRoom              *self);
guint         cm_room_get_encryption_msg_count     (CmRoom              *self);
gboolean      cm_room_is_direct                    (CmRoom              *self);
void          cm_room_set_is_direct                (CmRoom              *self,
                                                    gboolean             is_direct);
void          cm_room_set_is_encrypted             (CmRoom              *self,
                                                    gboolean             encrypted);
void          cm_room_query_keys_async             (CmRoom              *self,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean      cm_room_query_keys_finish            (CmRoom              *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
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
void          cm_room_get_joined_members_async     (CmRoom              *self,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
GListModel   *cm_room_get_joined_members_finish    (CmRoom              *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void          cm_room_is_encrypted_async           (CmRoom              *self,
                                                    GCancellable        *cancellable,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean      cm_room_is_encrypted_finish          (CmRoom              *self,
                                                    GAsyncResult        *result,
                                                   GError             **error);
G_END_DECLS
