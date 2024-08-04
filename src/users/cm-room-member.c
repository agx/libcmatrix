/* cm-room-member.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#define G_LOG_DOMAIN "cm-room-member"

#include "cm-config.h"

#include "cm-client.h"
#include "cm-client-private.h"
#include "cm-utils-private.h"
#include "cm-device.h"
#include "cm-device-private.h"
#include "cm-room.h"
#include "cm-room-private.h"
#include "cm-enc-private.h"
#include "cm-user-private.h"
#include "cm-room-member-private.h"
#include "cm-room-member.h"

struct _CmRoomMember
{
  CmUser      parent_instance;
};

G_DEFINE_TYPE (CmRoomMember, cm_room_member, CM_TYPE_USER)

static void
cm_room_member_class_init (CmRoomMemberClass *klass)
{
}

static void
cm_room_member_init (CmRoomMember *self)
{
}

CmRoomMember *
cm_room_member_new (GRefString *user_id)
{
  CmRoomMember *self;

  g_return_val_if_fail (user_id && *user_id == '@', NULL);

  self = g_object_new (CM_TYPE_ROOM_MEMBER, NULL);
  cm_user_set_user_id (CM_USER (self), user_id);

  return self;
}
