/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/*
 * Copyright 2024 The Phosh Developers
 *
 * Author(s):
 *   Guido Günther <agx@sigxcpu.org>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#undef NDEBUG
#undef G_DISABLE_ASSERT
#undef G_DISABLE_CHECKS
#undef G_DISABLE_CAST_CHECKS
#undef G_LOG_DOMAIN

#include "cm-pusher.c"

static void
test_pusher_new (void)
{
  CmPusher *pusher;

  pusher = cm_pusher_new ();
  g_assert (CM_IS_PUSHER (pusher));

  cm_pusher_set_kind_from_string (pusher, "http");
  g_assert_cmpint (cm_pusher_get_kind (pusher), ==, CM_PUSHER_KIND_HTTP);

  g_assert_finalize_object (pusher);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/pusher/new", test_pusher_new);

  return g_test_run ();
}
