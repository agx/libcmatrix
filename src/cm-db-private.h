/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-db.h
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

#include "cm-enc-private.h"
#include "cm-client.h"
#include "cm-device.h"
#include "cm-room.h"

G_BEGIN_DECLS

/* These values shouldn’t be changed. They are used in DB */
typedef enum {
  SESSION_OLM_V1_IN      = 1,
  SESSION_OLM_V1_OUT     = 2,
  SESSION_MEGOLM_V1_IN   = 3,
  SESSION_MEGOLM_V1_OUT  = 4,
} CmSessionType;

#define CMATRIX_ALGORITHM_A256CTR 1

#define CMATRIX_KEY_TYPE_OCT      1

#define CM_TYPE_DB (cm_db_get_type ())

G_DECLARE_FINAL_TYPE (CmDb, cm_db, CM, DB, GObject)

CmDb          *cm_db_new                           (void);
void           cm_db_open_async                    (CmDb                *self,
                                                    char                *dir,
                                                    const char          *file_name,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean       cm_db_open_finish                   (CmDb                *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
gboolean       cm_db_is_open                       (CmDb                *self);
void           cm_db_close_async                   (CmDb                *self,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean       cm_db_close_finish                  (CmDb                *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void           cm_db_save_client_async             (CmDb                *db,
                                                    CmClient            *client,
                                                    char                *pickle,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean       cm_db_save_client_finish            (CmDb                *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void           cm_db_load_client_async             (CmDb                *db,
                                                    CmClient            *client,
                                                    const char          *device_id,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean       cm_db_load_client_finish            (CmDb                *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void           cm_db_save_room_async               (CmDb                *self,
                                                    CmClient            *client,
                                                    CmRoom              *room,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean       cm_db_save_room_finish              (CmDb                *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void           cm_db_delete_client_async           (CmDb                *self,
                                                    CmClient            *client,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean       cm_db_delete_client_finish          (CmDb                *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void           cm_db_save_file_enc_async           (CmDb                *self,
                                                    CmEncFileInfo       *file,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
gboolean       cm_db_save_file_enc_finish          (CmDb                *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
void           cm_db_find_file_enc_async           (CmDb                *self,
                                                    const char          *url,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
CmEncFileInfo *cm_db_find_file_enc_finish          (CmDb                *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
gboolean       cm_db_add_session                   (CmDb                *self,
                                                    gpointer             session,
                                                    char                *pickle);
gpointer       cm_db_lookup_session                (CmDb                *self,
                                                    const char          *account_id,
                                                    const char          *account_device,
                                                    const char          *session_id,
                                                    const char          *sender_key,
                                                    const char          *pickle_key,
                                                    const char          *room_id,
                                                    CmSessionType        type);
void           cm_db_add_room_members              (CmDb                *self,
                                                    CmRoom              *cm_room,
                                                    GPtrArray           *members);
void           cm_db_add_room_events               (CmDb                *self,
                                                    CmRoom              *room,
                                                    GPtrArray           *events,
                                                    gboolean             prepend);
void           cm_db_get_past_events_async         (CmDb                *self,
                                                    CmRoom              *room,
                                                    CmEvent             *from,
                                                    GAsyncReadyCallback  callback,
                                                    gpointer             user_data);
GPtrArray     *cm_db_get_past_events_finish        (CmDb                *self,
                                                    GAsyncResult        *result,
                                                    GError             **error);
gpointer       cm_db_lookup_olm_session            (CmDb                *self,
                                                    const char          *account_id,
                                                    const char          *account_device,
                                                    const char          *sender_curve25519_key,
                                                    const char          *body,
                                                    const char          *pickle_key,
                                                    CmSessionType        type,
                                                    size_t               message_type,
                                                    char               **out_plain_text);
void           cm_db_mark_user_device_change       (CmDb                *self,
                                                    CmClient            *client,
                                                    GPtrArray           *users,
                                                    gboolean             outdated,
                                                    gboolean             is_tracking);
void           cm_db_update_user_devices           (CmDb                *self,
                                                    CmClient            *client,
                                                    CmUser              *user,
                                                    GPtrArray           *added,
                                                    GPtrArray           *removed,
                                                    gboolean             force_add);
void          cm_db_update_device                  (CmDb                *self,
                                                    CmClient            *client,
                                                    CmUser              *user,
                                                    CmDevice            *device);

G_END_DECLS
