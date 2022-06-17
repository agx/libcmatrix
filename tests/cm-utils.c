/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-utils.c
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

#include "cm-utils-private.h"

static void
test_utils_valid_user_name (void)
{
  struct Data
  {
    const char *user_name;
    gboolean valid;
  } data[] = {
     {NULL, FALSE},
     {"", FALSE},
     {"@:.", FALSE},
     {"@bob:", FALSE},
     {"@:example.org", FALSE},
     {"abc", FALSE},
     {"good@bad:com", FALSE},
     {"@a:example.org", TRUE},
     {"@alice:example.org", TRUE},
     {"@alice:example.org@alice:example.org", FALSE},
     {"@alice:sub.example.org", TRUE},
     {"@bob:localhost", TRUE},
  };

  for (guint i = 0; i < G_N_ELEMENTS (data); i++)
    {
      const char *user_name = data[i].user_name;
      gboolean valid = data[i].valid;

      if (valid)
        g_assert_true (cm_utils_user_name_valid (user_name));
      else
        g_assert_false (cm_utils_user_name_valid (user_name));
    }
}

static void
test_utils_valid_email (void)
{
  struct Data
  {
    const char *email;
    gboolean valid;
  } data[] = {
    {"", FALSE},
    {"@:.", FALSE},
    {"@bob:", FALSE},
    {"@:example.org", FALSE},
    {"abc", FALSE},
    {"good@bad:com", FALSE},
    {"@a:example.org", FALSE},
    {"@alice:example.org", FALSE},
    {"test@user.com", TRUE},
    {"test@user.comtest@user.com", FALSE},
    {"തറ@home.com", TRUE},
  };

  for (guint i = 0; i < G_N_ELEMENTS (data); i++)
    {
      const char *email = data[i].email;
      gboolean valid = data[i].valid;

      if (valid)
        g_assert_true (cm_utils_user_name_is_email (email));
      else
        g_assert_false (cm_utils_user_name_is_email (email));
    }
}

static void
test_utils_valid_phone (void)
{
  struct Data
  {
    const char *phone;
    gboolean valid;
  } data[] = {
     {"", FALSE},
     {"123", FALSE},
     {"+9123", FALSE},
     {"+91223344", FALSE},
     {"+91123456789", TRUE},
     {"+13123456789", TRUE},
     {"+13123456789002211443", FALSE},
  };

  for (guint i = 0; i < G_N_ELEMENTS (data); i++)
    {
      const char *phone = data[i].phone;
      gboolean valid = data[i].valid;

      if (valid)
        g_assert_true (cm_utils_mobile_is_valid (phone));
      else
        g_assert_false (cm_utils_mobile_is_valid (phone));
    }
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cm-utils/valid-user-name", test_utils_valid_user_name);
  g_test_add_func ("/cm-utils/valid-email", test_utils_valid_email);
  g_test_add_func ("/cm-utils/valid-phone", test_utils_valid_phone);

  return g_test_run ();
}
