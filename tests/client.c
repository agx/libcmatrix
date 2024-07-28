/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* utils.c
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

#include "cm-client.h"

static void
test_cm_client_new (void)
{
  CmClient *client;
  CmAccount *account;

  client = cm_client_new ();
  /* Mark client to not save changes to db */
  g_object_set_data (G_OBJECT (client), "no-save", GINT_TO_POINTER (TRUE));
  g_assert (CM_IS_CLIENT (client));

  g_assert_null (cm_client_get_user_id (client));
  cm_client_set_user_id (client, "@invalid:bad:");
  g_assert_null (cm_client_get_user_id (client));
  cm_client_set_user_id (client, "@user:example.com");
  g_assert_cmpstr (cm_client_get_user_id (client), ==, "@user:example.com");

  g_assert_false (cm_client_get_enabled (client));

  account = cm_client_get_account (client);
  g_assert_null (cm_account_get_login_id (account));
  cm_account_set_login_id (account, "user@@invalid");
  g_assert_null (cm_account_get_login_id (account));
  cm_account_set_login_id (account, "user@example.com");
  g_assert_cmpstr (cm_account_get_login_id (account), ==, "user@example.com");

  g_assert_null (cm_client_get_homeserver (client));
  cm_client_set_homeserver (client, "http://localhost:8008/");
  g_assert_cmpstr (cm_client_get_homeserver (client), ==, "http://localhost:8008");
  cm_client_set_homeserver (client, "http://sub.domain.example.com/");
  g_assert_cmpstr (cm_client_get_homeserver (client), ==, "http://sub.domain.example.com");

  g_assert_null (cm_client_get_password (client));
  cm_client_set_password (client, "hunter2");
  g_assert_cmpstr (cm_client_get_password (client), ==, "hunter2");

  g_assert_null (cm_client_get_access_token (client));
  cm_client_set_access_token (client, "ec-8b67-37f0683");
  g_assert_cmpstr (cm_client_get_access_token (client), ==, "ec-8b67-37f0683");

  g_assert_null (cm_client_get_device_id (client));
  cm_client_set_device_id (client, "DEADBEAF");
  g_assert_cmpstr (cm_client_get_device_id (client), ==, "DEADBEAF");

  g_assert_null (cm_client_get_device_name (client));
  cm_client_set_device_name (client, "Chatty");
  g_assert_cmpstr (cm_client_get_device_name (client), ==, "Chatty");

  g_assert_null (cm_client_get_pickle_key (client));
  cm_client_set_pickle_key (client, "passw@rd");
  /* We don't have set encryption, so password shall be NULL */
  g_assert_null (cm_client_get_pickle_key (client));

  g_assert_false (cm_client_is_sync (client));
  g_assert_false (cm_client_get_logging_in (client));
  g_assert_false (cm_client_get_logged_in (client));

  g_assert_finalize_object (client);
}

int
main (int   argc,
      char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cm-client/new", test_cm_client_new);

  return g_test_run ();
}
