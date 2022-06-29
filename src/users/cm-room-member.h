/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-room-member.h
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

G_BEGIN_DECLS

#include "cm-user.h"
#include "cm-client.h"
#include "cm-enums.h"

#define CM_TYPE_ROOM_MEMBER (cm_room_member_get_type ())

G_DECLARE_FINAL_TYPE (CmRoomMember, cm_room_member, CM, ROOM_MEMBER, CmUser)

const char    *cm_room_member_get_name             (CmRoomMember          *self);

G_END_DECLS

