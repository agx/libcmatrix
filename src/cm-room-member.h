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

#define CM_TYPE_ROOM_MEMBER (cm_room_member_get_type ())

G_DECLARE_FINAL_TYPE (CmRoomMember, cm_room_member, CM, ROOM_MEMBER, GObject)

const char    *cm_room_member_get_user_id          (CmRoomMember          *self);
const char    *cm_room_member_get_display_name     (CmRoomMember          *self);
const char    *cm_room_member_get_name             (CmRoomMember          *self);
const char    *cm_room_member_get_avatar_url       (CmRoomMember          *self);
void           cm_room_member_get_avatar_async     (CmRoomMember          *self,
                                                    GCancellable          *cancellable,
                                                    GAsyncReadyCallback    callback,
                                                    gpointer               user_data);
GInputStream  *cm_room_member_get_avatar_finish    (CmRoomMember          *self,
                                                    GAsyncResult          *result,
                                                    GError               **error);

G_END_DECLS

