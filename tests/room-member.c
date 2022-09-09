/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* room-member.c
 *
 * Copyright 2022 Purism SPC
 *
 * Author(s):
 *   Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#undef NDEBUG
#undef G_DISABLE_ASSERT
#undef G_DISABLE_CHECKS
#undef G_DISABLE_CAST_CHECKS
#undef G_LOG_DOMAIN

#include "cm-room-private.h"

#undef G_LOG_DOMAIN
#include "users/cm-room-member.c"

static void
test_room_member_new (void)
{
  CmRoomMember *member;
  GRefString *user_id;
  CmClient *client;
  CmRoom *room;
  CmUser *user;

  room = cm_room_new ("random room");
  client = cm_client_new ();
  cm_room_set_client (room, client);
  user_id = g_ref_string_new_intern ("@alice:example.co");
  member = cm_room_member_new (user_id);
  g_ref_string_release (user_id);
  user = CM_USER (member);
  cm_user_set_client (user, client);
  g_object_unref (client);
  g_assert (CM_IS_ROOM_MEMBER (member));

  g_assert_cmpstr (cm_user_get_id (user), ==, "@alice:example.co");

  g_assert_finalize_object (room);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/room-member/new", test_room_member_new);

  return g_test_run ();
}
