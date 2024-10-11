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

G_BEGIN_DECLS

#include "cm-enums.h"
#include "cm-pusher.h"
#include "cm-types.h"
#include "users/cm-account.h"

#define CM_TYPE_CLIENT (cm_client_get_type ())
G_DECLARE_FINAL_TYPE (CmClient, cm_client, CM, CLIENT, GObject)


/**
 * CmCallback:
 * @self: the client that received the events
 * @room:(nullable): The room (if any) the events belong to
 * @events:(element-type CmEvent): The events
 * @err:(nullable): A recoverable error
 * @user_data: The user data passed in [method@Client.set_sync_callback]
 *
 * Type definition for the function that will be called when the client
 * sees new events.
 *
 * Usually you don't do much in this callback. The `items-changed`
 * signals of the list models returned from
 * e.g. [method@Client.get_joined_rooms] or [method@Room.get_events_list]
 * are easier to use.
 *
 * The `GMainContext` for these operations isn't well defined atm, see
 * https://source.puri.sm/Librem5/libcmatrix/-/issues/24
 */
typedef void   (*CmCallback)                        (CmClient           *self,
                                                     CmRoom             *room,
                                                     GPtrArray          *events,
                                                     GError             *err,
                                                     gpointer            user_data);

CmClient     *cm_client_new                           (void);
CmAccount    *cm_client_get_account                   (CmClient            *self);
void          cm_client_set_enabled                   (CmClient            *self,
                                                       gboolean             enable);
gboolean      cm_client_get_enabled                   (CmClient            *self);
void          cm_client_set_sync_callback             (CmClient            *self,
                                                       CmCallback           callback,
                                                       gpointer             user_data,
                                                       GDestroyNotify       destroy_data);
gboolean      cm_client_set_user_id                   (CmClient            *self,
                                                       const char          *matrix_user_id);
GRefString   *cm_client_get_user_id                   (CmClient            *self);
gboolean      cm_client_set_homeserver                (CmClient            *self,
                                                       const char          *homeserver);
const char   *cm_client_get_homeserver                (CmClient            *self);
const char * const *
              cm_client_get_homeserver_versions       (CmClient            *self);
void          cm_client_set_password                  (CmClient            *self,
                                                       const char          *password);
const char   *cm_client_get_password                  (CmClient            *self);
void          cm_client_set_access_token              (CmClient            *self,
                                                       const char          *access_token);
const char   *cm_client_get_access_token              (CmClient            *self);
void          cm_client_set_device_id                 (CmClient            *self,
                                                       const char          *device_id);
const char   *cm_client_get_device_id                 (CmClient            *self);
void          cm_client_set_device_name               (CmClient            *self,
                                                       const char          *device_name);
const char   *cm_client_get_device_name               (CmClient            *self);
void          cm_client_set_pickle_key                (CmClient            *self,
                                                       const char          *pickle_key);
const char   *cm_client_get_pickle_key                (CmClient            *self);
const char   *cm_client_get_ed25519_key               (CmClient            *self);

void          cm_client_join_room_async               (CmClient            *self,
                                                       const char          *id_or_alias,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
gboolean      cm_client_join_room_finish              (CmClient            *self,
                                                       GAsyncResult        *result,
                                                       GError             **error);
gboolean      cm_client_join_room_sync                (CmClient            *self,
                                                       const char          *id_or_alias,
                                                       GError             **error);
void          cm_client_get_homeserver_async          (CmClient            *self,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
const char   *cm_client_get_homeserver_finish         (CmClient            *self,
                                                       GAsyncResult        *result,
                                                       GError             **error);
gboolean      cm_client_can_connect                   (CmClient            *self);
void          cm_client_start_sync                    (CmClient            *self);
gboolean      cm_client_is_sync                       (CmClient            *self);
void          cm_client_stop_sync                     (CmClient            *self);
gboolean      cm_client_get_logging_in                (CmClient            *self);
gboolean      cm_client_get_logged_in                 (CmClient            *self);
GListModel   *cm_client_get_joined_rooms              (CmClient            *self);
GListModel   *cm_client_get_invited_rooms             (CmClient            *self);
GListModel   *cm_client_get_key_verifications         (CmClient            *self);


GPtrArray    *cm_client_get_pushers_finish            (CmClient            *self,
                                                       GAsyncResult        *result,
                                                       GError             **error);
void          cm_client_get_pushers_async             (CmClient            *self,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
GPtrArray    *cm_client_get_pushers_sync              (CmClient            *self,
                                                       GCancellable        *cancellable,
                                                       GError             **error);
gboolean      cm_client_add_pusher_finish             (CmClient            *self,
                                                       GAsyncResult        *result,
                                                       GError             **error);
void          cm_client_add_pusher_async              (CmClient            *self,
                                                       CmPusher            *pusher,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
gboolean      cm_client_add_pusher_sync               (CmClient            *self,
                                                       CmPusher            *pusher,
                                                       GCancellable        *cancellable,
                                                       GError             **error);
gboolean      cm_client_remove_pusher_finish          (CmClient            *self,
                                                       GAsyncResult        *result,
                                                       GError             **error);
void          cm_client_remove_pusher_async           (CmClient            *self,
                                                       CmPusher            *pusher,
                                                       GCancellable        *cancellable,
                                                       GAsyncReadyCallback  callback,
                                                       gpointer             user_data);
gboolean      cm_client_remove_pusher_sync            (CmClient            *self,
                                                       CmPusher            *pusher,
                                                       GCancellable        *cancellable,
                                                       GError             **error);
G_END_DECLS
