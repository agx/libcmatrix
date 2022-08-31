/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* session.c
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

#include "src/cm-enc.c"
#include "cm-matrix.h"
#include "cm-matrix-private.h"

static void
finish_bool_cb (GObject      *object,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  GTask *task = user_data;
  gboolean status;

  g_assert_true (G_IS_TASK (task));

  status = g_task_propagate_boolean (G_TASK (result), &error);
  g_assert_no_error (error);
  g_task_return_boolean (task, status);
}

/* 'n' starts from 1 */
static JsonObject *
get_nth_member (JsonObject *root,
                guint       n)
{
  g_autoptr(GList) members = NULL;

  members = json_object_get_members (root);

  if (g_list_length (members) < n)
    return NULL;

  return json_object_get_object_member (root, g_list_nth_data (members, n));
}

static void
test_enc_chat_new (void)
{
  char *alice_one_time, *bob_one_time;
  CmEnc *alice_enc, *bob_enc;
  JsonObject *obj, *root;
  CmMatrix *matrix;
  size_t len;

  matrix = g_object_new (CM_TYPE_MATRIX, NULL);

  {
    GTask *task;
    GError *error = NULL;

    task = g_task_new (NULL, NULL, NULL, NULL);
    cm_matrix_open_async (matrix,
                          g_test_get_dir (G_TEST_BUILT),
                          "test-chat.c",
                          NULL,
                          finish_bool_cb, task);

    while (!g_task_get_completed (task))
      g_main_context_iteration (NULL, TRUE);

    g_assert_true (g_task_propagate_boolean (task, &error));
    g_assert_no_error (error);
    g_assert_finalize_object (task);
  }

  /* Generate new keys for each */
  {
    GRefString *matrix_id;

    matrix_id = g_ref_string_new_intern ("@alice:example.org");
    alice_enc = cm_enc_new (cm_matrix_get_db (matrix), NULL, NULL);
    cm_enc_set_details (alice_enc, matrix_id, "SYNAPSE");
    g_ref_string_release (matrix_id);

    matrix_id = g_ref_string_new_intern ("@bob:example.org");
    bob_enc = cm_enc_new (cm_matrix_get_db (matrix), NULL, NULL);
    cm_enc_set_details (bob_enc, matrix_id, "DENDRITE");
    g_ref_string_release (matrix_id);
  }

  /* Generate one time keys */
  len = cm_enc_create_one_time_keys (alice_enc, 3);
  g_assert_cmpint (len, ==, 3);
  len = cm_enc_create_one_time_keys (bob_enc, 3);
  g_assert_cmpint (len, ==, 3);

  alice_one_time = cm_enc_get_one_time_keys_json (alice_enc);
  bob_one_time = cm_enc_get_one_time_keys_json (bob_enc);
  g_assert_nonnull (alice_one_time);
  g_assert_nonnull (bob_one_time);

  cm_enc_publish_one_time_keys (alice_enc);
  cm_enc_publish_one_time_keys (bob_enc);
  g_assert_null (cm_enc_get_one_time_keys_json (alice_enc));
  g_assert_null (cm_enc_get_one_time_keys_json (bob_enc));

  root = cm_utils_string_to_json_object (alice_one_time);
  g_assert_nonnull (root);
  obj = cm_utils_json_object_get_object (root, "one_time_keys");
  obj = get_nth_member (obj, 1);
  g_assert_nonnull (obj);
  /* Verify Alice's one time key with Bob's device */
  g_assert_true (cm_enc_verify (bob_enc, obj,
                                cm_enc_get_user_id (alice_enc),
                                cm_enc_get_device_id (alice_enc),
                                cm_enc_get_ed25519_key (alice_enc)));
  json_object_unref (root);

  root = cm_utils_string_to_json_object (bob_one_time);
  g_assert_nonnull (root);
  obj = cm_utils_json_object_get_object (root, "one_time_keys");
  obj = get_nth_member (obj, 1);
  g_assert_nonnull (obj);
  /* Verify Bob's one time key with Alice's device */
  g_assert_true (cm_enc_verify (alice_enc, obj,
                                cm_enc_get_user_id (bob_enc),
                                cm_enc_get_device_id (bob_enc),
                                cm_enc_get_ed25519_key (bob_enc)));
  /* Verify Bob's one time key with Bob's device */
  g_assert_true (cm_enc_verify (bob_enc, obj,
                                cm_enc_get_user_id (bob_enc),
                                cm_enc_get_device_id (bob_enc),
                                cm_enc_get_ed25519_key (bob_enc)));
  g_assert_false (cm_enc_verify (bob_enc, obj,
                                 cm_enc_get_user_id (alice_enc),
                                 cm_enc_get_device_id (alice_enc),
                                 cm_enc_get_ed25519_key (alice_enc)));
  json_object_unref (root);

  g_free (alice_one_time);
  g_free (bob_one_time);

  g_assert_finalize_object (alice_enc);
  g_assert_finalize_object (bob_enc);
  g_assert_finalize_object (matrix);
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr(CmMatrix) matrix = NULL;

  g_test_init (&argc, &argv, NULL);

  cm_init (TRUE);
  matrix = cm_matrix_new (g_test_get_dir (G_TEST_BUILT),
                          g_test_get_dir (G_TEST_BUILT),
                          "org.example.CMatrix",
                          FALSE);
  g_test_add_func ("/enc-chat/new", test_enc_chat_new);

  return g_test_run ();
}
