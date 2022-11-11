/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-room-message-event.h
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

#include <glib-object.h>
#include <gio/gio.h>

#include "cm-room-event.h"
#include "cm-enums.h"

G_BEGIN_DECLS

#define CM_TYPE_ROOM_MESSAGE_EVENT (cm_room_message_event_get_type ())

G_DECLARE_FINAL_TYPE (CmRoomMessageEvent, cm_room_message_event, CM, ROOM_MESSAGE_EVENT, CmRoomEvent)

CmContentType       cm_room_message_event_get_msg_type    (CmRoomMessageEvent   *self);
const char         *cm_room_message_event_get_body        (CmRoomMessageEvent    *self);
const char         *cm_room_message_event_get_file_path   (CmRoomMessageEvent    *self);
void                cm_room_message_event_get_file_async  (CmRoomMessageEvent    *self,
                                                           GCancellable          *cancellable,
                                                           GFileProgressCallback  progress_callback,
                                                           gpointer               progress_user_data,
                                                           GAsyncReadyCallback    callback,
                                                           gpointer               user_data);
GInputStream       *cm_room_message_event_get_file_finish (CmRoomMessageEvent    *self,
                                                           GAsyncResult          *result,
                                                           GError               **error);


G_END_DECLS
