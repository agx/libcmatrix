/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-room-member.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

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
cm_room_member_new (const char *user_id)
{
  CmRoomMember *self;

  g_return_val_if_fail (user_id && *user_id == '@', NULL);

  self = g_object_new (CM_TYPE_ROOM_MEMBER, NULL);
  cm_user_set_user_id (CM_USER (self), user_id);

  return self;
}

void
cm_room_member_set_json_data (CmRoomMember *self,
                              JsonObject   *object)
{
  JsonObject *child;
  const char *name, *avatar_url;

  g_return_if_fail (CM_IS_ROOM_MEMBER (self));
  g_return_if_fail (object);

  child = cm_utils_json_object_get_object (object, "content");

  if (!child)
    child = object;

  name = cm_utils_json_object_get_string (child, "display_name");
  if (!name)
      name = cm_utils_json_object_get_string (child, "displayname");

  avatar_url = cm_utils_json_object_get_string (child, "avatar_url");
  cm_user_set_details (CM_USER (self), name, avatar_url);
}
