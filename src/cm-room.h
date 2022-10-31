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

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#include "cm-client.h"
#include "cm-enums.h"
#include "events/cm-event.h"

#define CM_TYPE_ROOM (cm_room_get_type ())

G_DECLARE_FINAL_TYPE (CmRoom, cm_room, CM, ROOM, GObject)

const char   *cm_room_get_id                      (CmRoom                *self);
gboolean      cm_room_self_has_power_for_event    (CmRoom                *self,
                                                   CmEventType            type);
const char   *cm_room_get_name                    (CmRoom                *self);
const char   *cm_room_get_past_name               (CmRoom                *self);
gboolean      cm_room_is_encrypted                (CmRoom                *self);
GListModel   *cm_room_get_joined_members          (CmRoom                *self);
GListModel   *cm_room_get_events_list             (CmRoom                *self);
gint64        cm_room_get_unread_notification_counts  (CmRoom                *self);
void          cm_room_get_avatar_async                (CmRoom                *self,
                                                       GCancellable          *cancellable,
                                                       GAsyncReadyCallback    callback,
                                                       gpointer               user_data);
GInputStream *cm_room_get_avatar_finish               (CmRoom                *self,
                                                       GAsyncResult          *result,
                                                       GError               **error);
void          cm_room_accept_invite_async         (CmRoom                *self,
                                                   GCancellable          *cancellable,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
gboolean      cm_room_accept_invite_finish        (CmRoom                *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
void          cm_room_reject_invite_async         (CmRoom                *self,
                                                   GCancellable          *cancellable,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
gboolean      cm_room_reject_invite_finish        (CmRoom                *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
const char   *cm_room_send_text_async             (CmRoom                *self,
                                                   const char            *text,
                                                   GCancellable          *cancellable,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
char         *cm_room_send_text_finish            (CmRoom                *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
const char   *cm_room_send_file_async             (CmRoom                *self,
                                                   GFile                 *file,
                                                   const char            *body,
                                                   GFileProgressCallback  progress_callback,
                                                   gpointer               progress_user_data,
                                                   GCancellable          *cancellable,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
char         *cm_room_send_file_finish            (CmRoom                *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
void          cm_room_set_typing_notice_async     (CmRoom                *self,
                                                   gboolean               typing,
                                                   GCancellable          *cancellable,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
gboolean      cm_room_set_typing_notice_finish    (CmRoom                *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
void          cm_room_enable_encryption_async     (CmRoom                *self,
                                                   GCancellable          *cancellable,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
gboolean      cm_room_enable_encryption_finish    (CmRoom                *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
void          cm_room_leave_async                 (CmRoom                *self,
                                                   GCancellable          *cancellable,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
gboolean      cm_room_leave_finish                (CmRoom                *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
void          cm_room_set_read_marker_async       (CmRoom                *self,
                                                   CmEvent               *fully_read_event,
                                                   CmEvent               *read_receipt_event,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
gboolean      cm_room_set_read_marker_finish      (CmRoom                *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
void          cm_room_load_past_events_async      (CmRoom                *self,
                                                   GAsyncReadyCallback    callback,
                                                   gpointer               user_data);
gboolean      cm_room_load_past_events_finish     (CmRoom                *self,
                                                   GAsyncResult          *result,
                                                   GError               **error);
G_END_DECLS

