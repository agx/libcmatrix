/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* room.c
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

#include "cm-room.c"

static void
test_room_new (void)
{
  CmRoom *room;

  room = cm_room_new ("some-room-id");
  g_assert (CM_IS_ROOM (room));

  g_assert_cmpstr (cm_room_get_id (room), ==, "some-room-id");

  g_assert_false (cm_room_is_encrypted (room));

  g_assert_finalize_object (room);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/room/new", test_room_new);

  return g_test_run ();
}
