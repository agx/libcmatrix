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

static JsonObject *
get_json_object_for_file (const char *dir,
                          const char *file_name)
{
  g_autoptr(JsonParser) parser = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  path = g_build_filename (dir, file_name, NULL);
  parser = json_parser_new ();
  json_parser_load_from_file (parser, path, &error);
  g_assert_no_error (error);

  return json_node_dup_object (json_parser_get_root (parser));
}

static void
test_utils_canonical (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GDir) dir = NULL;
  g_autofree char *path = NULL;
  const char *name;

  path = g_test_build_filename (G_TEST_DIST, "cm-utils", NULL);
  dir = g_dir_open (path, 0, &error);
  g_assert_no_error (error);

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree char *expected_file = NULL;
    g_autofree char *expected_name = NULL;
    g_autofree char *expected_json = NULL;
    g_autoptr(JsonObject) object = NULL;
    g_autoptr(GString) json_str = NULL;

    if (!g_str_has_suffix (name, ".json"))
      continue;

    expected_name = g_strconcat (name, ".expected", NULL);
    expected_file = g_build_filename (path, expected_name, NULL);
    g_file_get_contents (expected_file, &expected_json, NULL, &error);
    g_assert_no_error (error);

    object = get_json_object_for_file (path, name);
    json_str = cm_utils_json_get_canonical (object, NULL);
    g_assert_cmpstr (json_str->str, ==, expected_json);
  }
}

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

static void
test_utils_valid_home_server (void)
{
  struct Data
  {
    const char *uri;
    gboolean valid;
  } data[] = {
    {"", FALSE},
    {"http://", FALSE},
    {"ftp://example.com", FALSE},
    {"http://example.com", TRUE},
    {"https://example.com", TRUE},
    {"http://example.com/", TRUE},
    {"http://example.com.", FALSE},
    {"http://localhost:8008", TRUE},
    {"http://localhost:8008/path", FALSE},
  };

  for (guint i = 0; i < G_N_ELEMENTS (data); i++)
    {
      const char *uri = data[i].uri;
      gboolean valid = data[i].valid;

      if (valid)
        g_assert_true (cm_utils_home_server_valid (uri));
      else
        g_assert_false (cm_utils_home_server_valid (uri));
    }
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cm-utils/canonical", test_utils_canonical);
  g_test_add_func ("/cm-utils/valid-user-name", test_utils_valid_user_name);
  g_test_add_func ("/cm-utils/valid-email", test_utils_valid_email);
  g_test_add_func ("/cm-utils/valid-phone", test_utils_valid_phone);
  g_test_add_func ("/cm-utils/valid-home-server", test_utils_valid_home_server);

  return g_test_run ();
}
