/* -*- mode: c; c-basic-offset: 2; indent-tabs-mode: nil; -*- */
/* cm-db.c
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

#include <glib/gstdio.h>
#include <sqlite3.h>

#include "cm-matrix.h"
#include "cm-db-private.h"
#include "cm-client.h"

typedef struct _Data
{
  const char *username;
  const char *device_id;
} Data;

static void
finish_bool_cb (GObject      *object,
                GAsyncResult *result,
                gpointer      user_data)
{
  g_autoptr(GError) error = NULL;
  GTask *task = user_data;
  GObject *obj;
  gboolean status;

  g_assert_true (G_IS_TASK (task));

  status = g_task_propagate_boolean (G_TASK (result), &error);
  g_assert_no_error (error);

  obj = G_OBJECT (result);
  g_object_set_data (user_data, "enabled", g_object_get_data (obj, "enabled"));
  g_object_set_data_full (user_data, "pickle", g_object_steal_data (obj, "pickle"), g_free);
  g_object_set_data_full (user_data, "device", g_object_steal_data (obj, "device"), g_free);
  g_object_set_data_full (user_data, "username", g_object_steal_data (obj, "username"), g_free);
  g_task_return_boolean (task, status);
}

static gboolean
client_matches_user_details (gconstpointer client,
                             gconstpointer data)
{
  Data *details = (gpointer)data;

  g_assert_true (CM_IS_CLIENT ((gpointer)client));
  g_assert_true (details);
  g_assert_nonnull (details->username);
  g_assert_nonnull (details->device_id);

  return g_strcmp0 (details->username, cm_client_get_user_id ((gpointer)client)) == 0 &&
    g_strcmp0 (details->device_id, cm_client_get_device_id ((gpointer)client)) == 0;
}

static void
add_matrix_account (CmDb       *db,
                    GPtrArray  *client_array,
                    const char *username,
                    const char *pickle,
                    const char *device_id,
                    gboolean    enabled)
{
  CmClient *client;
  GObject *object;
  GTask *task;
  GError *error = NULL;
  gboolean success;
  g_autofree Data *data = NULL;
  guint i;

  g_assert_true (CM_IS_DB (db));
  g_assert_nonnull (client_array);
  g_assert_nonnull (username);

  data = g_new0 (Data, 1);
  data->device_id = device_id;
  data->username = username;

  if (g_ptr_array_find_with_equal_func (client_array, data,
                                        client_matches_user_details, &i)) {
    client = client_array->pdata[i];
  } else {
    client = cm_client_new ();
    /* Mark client to not save changes to db */
    g_object_set_data (G_OBJECT (client), "no-save", GINT_TO_POINTER (TRUE));
    cm_client_set_user_id (client, username);
    cm_client_set_device_id (client, device_id);
    g_ptr_array_add (client_array, client);
  }

  g_assert_true (CM_IS_CLIENT (client));
  object = G_OBJECT (client);

  g_object_set_data (object, "enabled", GINT_TO_POINTER (enabled));
  g_object_set_data_full (object, "pickle", g_strdup (pickle), g_free);
  g_object_set_data_full (object, "device", g_strdup (device_id), g_free);

  task = g_task_new (NULL, NULL, NULL, NULL);
  cm_db_save_client_async (db, client, g_strdup (pickle),
                           finish_bool_cb, task);

  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  success = g_task_propagate_boolean (task, &error);
  g_assert_no_error (error);
  g_assert_true (success);
  g_clear_object (&task);

  g_assert_true (g_ptr_array_find (client_array, client, &i));
  client = client_array->pdata[i];
  task = g_task_new (NULL, NULL, NULL, NULL);
  cm_db_load_client_async (db, client, device_id, finish_bool_cb, task);

  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  success = g_task_propagate_boolean (task, &error);
  g_assert_no_error (error);
  g_assert_true (success);
  g_assert_cmpstr (g_object_get_data (G_OBJECT (task), "username"), ==,
                   cm_client_get_user_id (client));
  g_assert_cmpstr (g_object_get_data (G_OBJECT (task), "pickle"), ==,
                   g_object_get_data (object, "pickle"));
  g_assert_cmpstr (g_object_get_data (G_OBJECT (task), "device"), ==,
                   g_object_get_data (object, "device"));
  g_clear_object (&task);
}

static void
test_cm_db_account (void)
{
  GTask *task;
  CmDb *db;
  gboolean status;
  GPtrArray *account_array;

  g_remove (g_test_get_filename (G_TEST_BUILT, "test-matrix.db", NULL));

  db = cm_db_new ();
  task = g_task_new (NULL, NULL, NULL, NULL);
  cm_db_open_async (db, g_strdup (g_test_get_dir (G_TEST_BUILT)),
                        "test-matrix.db", finish_bool_cb, task);

  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  status = g_task_propagate_boolean (task, NULL);
  g_assert_finalize_object (task);
  g_assert_true (status);

  account_array = g_ptr_array_new ();
  g_ptr_array_set_free_func (account_array, (GDestroyNotify)g_object_unref);

  add_matrix_account (db, account_array, "@alice:example.org",
                      NULL, "AABBCCDD", TRUE);
  add_matrix_account (db, account_array, "@alice:example.org",
                      NULL, "CCDDEE", FALSE);
  add_matrix_account (db, account_array, "@alice:example.com",
                      NULL, "XXAABBDD", FALSE);
  add_matrix_account (db, account_array, "@alice:example.com",
                      "Some Pickle", "XXAABBDD", TRUE);

  add_matrix_account (db, account_array, "@bob:example.org",
                      NULL, "XXAABBDD", FALSE);

  g_ptr_array_unref (account_array);
}

static void
test_cm_db_new (void)
{
  const char *file_name;
  CmDb *db;
  GTask *task;
  gboolean status;

  file_name = g_test_get_filename (G_TEST_BUILT, "test-matrix.db", NULL);
  g_remove (file_name);
  g_assert_false (g_file_test (file_name, G_FILE_TEST_EXISTS));

  db = cm_db_new ();
  task = g_task_new (NULL, NULL, NULL, NULL);
  cm_db_open_async (db, g_strdup (g_test_get_dir (G_TEST_BUILT)),
                    "test-matrix.db", finish_bool_cb, task);

  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  status = g_task_propagate_boolean (task, NULL);
  g_assert_true (g_file_test (file_name, G_FILE_TEST_IS_REGULAR));
  g_assert_true (cm_db_is_open (db));
  g_assert_true (status);
  g_clear_object (&task);

  task = g_task_new (NULL, NULL, NULL, NULL);
  cm_db_close_async (db, finish_bool_cb, task);

  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  status = g_task_propagate_boolean (task, NULL);
  g_assert_true (status);
  g_assert_false (cm_db_is_open (db));
  g_clear_object (&db);
  g_clear_object (&task);

  g_remove (file_name);
  g_assert_false (g_file_test (file_name, G_FILE_TEST_EXISTS));
}

static void
matrix_export_sql_file (const char  *sql_path,
                        const char  *file_name,
                        sqlite3    **db)
{
  g_autofree char *export_file = NULL;
  g_autofree char *input_file = NULL;
  g_autofree char *content = NULL;
  g_autoptr(GError) err = NULL;
  char *error = NULL;
  int status;

  g_assert (sql_path && *sql_path);
  g_assert (file_name && *file_name);
  g_assert (db);

  input_file = g_build_filename (sql_path, file_name, NULL);
  export_file = g_test_build_filename (G_TEST_BUILT, file_name, NULL);
  strcpy (export_file + strlen (export_file) - strlen ("sql"), "db");
  g_remove (export_file);
  status = sqlite3_open (export_file, db);
  g_assert_cmpint (status, ==, SQLITE_OK);
  g_file_get_contents (input_file, &content, NULL, &err);
  g_assert_no_error (err);

  status = sqlite3_exec (*db, content, NULL, NULL, &error);
  if (error)
    g_warning ("%s error: %s", G_STRLOC, error);
  g_assert_cmpint (status, ==, SQLITE_OK);
}

static int
db_get_int (sqlite3    *db,
            const char *statement)
{
  sqlite3_stmt *stmt;
  int value, status;

  g_assert (db);

  status = sqlite3_prepare_v2 (db, statement, -1, &stmt, NULL);
  g_assert_cmpint (status, ==, SQLITE_OK);

  status = sqlite3_step (stmt);
  g_assert_cmpint (status, ==, SQLITE_ROW);

  value = sqlite3_column_int (stmt, 0);

  sqlite3_finalize (stmt);

  return value;
}

static void
compare_table (sqlite3    *db,
               const char *sql,
               int         expected_count,
               int         count)
{
  sqlite3_stmt *stmt;
  int status;

  status = sqlite3_prepare_v2 (db, sql, -1, &stmt, NULL);
  if (status != SQLITE_OK)
    g_warning ("sql error: %s", sqlite3_errmsg (db));

  g_assert_cmpint (status, ==, SQLITE_OK);

  if (expected_count != count)
    g_warning ("%d %d sql: %s", expected_count, count, sql);
  status = sqlite3_step (stmt);
  g_assert_cmpint (status, ==, SQLITE_ROW);
  g_assert_cmpint (expected_count, ==, count);

  count = sqlite3_column_int (stmt, 0);
  if (expected_count != count)
    g_warning ("%d %d sql: %s", expected_count, count, sql);

  g_assert_cmpint (expected_count, ==, count);

  sqlite3_finalize (stmt);
}

static void
compare_matrix_db (sqlite3 *db)
{
  /* Pragma version should match */
  g_assert_cmpint (db_get_int (db, "PRAGMA main.user_version;"), ==,
                   db_get_int (db, "PRAGMA test.user_version;"));

  /* Each db should have the same count of table rows */
  g_assert_cmpint (db_get_int (db, "SELECT COUNT(*) FROM main.sqlite_master;"),
                   ==,
                   db_get_int (db, "SELECT COUNT(*) FROM test.sqlite_master;"));

  /* As duplicate rows are removed, SELECT count should match the size of one table. */
  compare_table (db,
                 "SELECT COUNT (*) FROM ("
                 "SELECT username,json_data FROM main.users "
                 "UNION "
                 "SELECT username,json_data FROM test.users "
                 ");",
                 db_get_int (db, "SELECT COUNT(*) FROM main.users;"),
                 db_get_int (db, "SELECT COUNT(*) FROM test.users;"));

  /* sqlite doesn't guarantee `id` to be always incremented by one.  It
   * may depend on the order they are updated.  And so
   * compare by values.
   */
  compare_table (db,
                 "SELECT COUNT (*) FROM ("
                 "SELECT username,device,users.json_data FROM main.user_devices "
                 "INNER JOIN main.users ON user_devices.user_id=users.id "
                 "UNION "
                 "SELECT username,device,users.json_data FROM test.user_devices "
                 "INNER JOIN main.users ON user_devices.user_id=users.id "
                 ");",
                 db_get_int (db, "SELECT COUNT(*) FROM main.user_devices;"),
                 db_get_int (db, "SELECT COUNT(*) FROM test.user_devices;"));

  compare_table (db,
                 "SELECT COUNT (*) FROM ("
                 "SELECT username,device,next_batch,pickle,enabled,accounts.json_data FROM main.accounts "
                 "INNER JOIN main.user_devices ON user_devices.id=accounts.user_device_id "
                 "INNER JOIN main.users ON users.id=user_devices.user_id "
                 "UNION "
                 "SELECT username,device,next_batch,pickle,enabled,accounts.json_data FROM test.accounts "
                 "INNER JOIN test.user_devices ON user_devices.id=accounts.user_device_id "
                 "INNER JOIN test.users ON users.id=user_devices.user_id "
                 ");",
                 db_get_int (db, "SELECT COUNT(*) FROM main.accounts;"),
                 db_get_int (db, "SELECT COUNT(*) FROM test.accounts;"));

  compare_table (db,
                 "SELECT COUNT (*) FROM ("
                 "SELECT username,device,room_name,prev_batch,rooms.json_data FROM main.rooms "
                 "INNER JOIN main.accounts ON accounts.id=rooms.account_id "
                 "INNER JOIN main.user_devices ON user_devices.id=accounts.user_device_id "
                 "INNER JOIN main.users ON user_devices.user_id=users.id "
                 "UNION "
                 "SELECT username,device,room_name,prev_batch,rooms.json_data FROM test.rooms "
                 "INNER JOIN test.accounts ON accounts.id=rooms.account_id "
                 "INNER JOIN test.user_devices ON user_devices.id=accounts.user_device_id "
                 "INNER JOIN test.users ON user_devices.user_id=users.id "
                 ");",
                 db_get_int (db, "SELECT COUNT(*) FROM main.rooms;"),
                 db_get_int (db, "SELECT COUNT(*) FROM test.rooms;"));

  compare_table (db,
                 "SELECT COUNT (*) FROM ("
                 "SELECT file_url,file_sha256,iv,version,algorithm,key,type,"
                 "extractable,json_data FROM main.encryption_keys "
                 "UNION "
                 "SELECT file_url,file_sha256,iv,version,algorithm,key,type,"
                 "extractable,json_data FROM test.encryption_keys "
                 ");",
                 db_get_int (db, "SELECT COUNT(*) FROM main.encryption_keys;"),
                 db_get_int (db, "SELECT COUNT(*) FROM test.encryption_keys;"));

  compare_table (db,
                 "SELECT COUNT (*) FROM ("
                 "SELECT username,device,sender_key,session_id,type,sessions.pickle,time,sessions.json_data FROM main.sessions "
                 "INNER JOIN main.accounts ON accounts.id=sessions.account_id "
                 "INNER JOIN main.user_devices ON user_devices.id=accounts.user_device_id "
                 "INNER JOIN main.users ON user_devices.user_id=users.id "
                 "UNION "
                 "SELECT username,device,sender_key,session_id,type,sessions.pickle,time,sessions.json_data FROM test.sessions "
                 "INNER JOIN test.accounts ON accounts.id=sessions.account_id "
                 "INNER JOIN test.user_devices ON user_devices.id=accounts.user_device_id "
                 "INNER JOIN test.users ON user_devices.user_id=users.id "
                 ");",
                 db_get_int (db, "SELECT COUNT(*) FROM main.sessions;"),
                 db_get_int (db, "SELECT COUNT(*) FROM test.sessions;"));
}

static void
test_cm_db_migration (void)
{
  g_autoptr(GDir) dir = NULL;
  g_autofree char *path = NULL;
  g_autoptr(GError) error = NULL;
  const char *name;

  path = g_test_build_filename (G_TEST_DIST, "cm-db", NULL);
  dir = g_dir_open (path, 0, &error);
  g_assert_no_error (error);

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree char *expected_file = NULL;
    g_autofree char *input_file = NULL;
    g_autofree char *input = NULL;
    CmDb *cm_db;
    sqlite3 *db = NULL;
    GTask *task;
    int status;

    if (g_str_has_suffix (name, "v2.sql"))
      continue;

    g_assert_true (g_str_has_suffix (name, "sql"));
    g_debug ("Migrating %s", name);

    /* Export old version sql file */
    matrix_export_sql_file (path, name, &db);
    sqlite3_close (db);

    /* Export migrated version sql file */
    expected_file = g_strdelimit (g_strdup (name), "01", '2');
    matrix_export_sql_file (path, expected_file, &db);

    /* Open history with old db, which will result in db migration */
    input_file = g_strdup (name);
    strcpy (input_file + strlen (input_file) - strlen ("sql"), "db");

    cm_db = cm_db_new ();
    task = g_task_new (NULL, NULL, NULL, NULL);
    cm_db_open_async (cm_db, g_strdup (g_test_get_dir (G_TEST_BUILT)),
                      input_file, finish_bool_cb, task);

    while (!g_task_get_completed (task))
      g_main_context_iteration (NULL, TRUE);

    status = g_task_propagate_boolean (task, NULL);
    g_assert_true (cm_db_is_open (cm_db));
    g_assert_true (status);
    g_assert_finalize_object (task);

    task = g_task_new (NULL, NULL, NULL, NULL);
    cm_db_close_async (cm_db, finish_bool_cb, task);

    while (!g_task_get_completed (task))
      g_main_context_iteration (NULL, TRUE);

    status = g_task_propagate_boolean (task, NULL);
    g_assert_true (status);
    g_assert_false (cm_db_is_open (cm_db));
    g_assert_finalize_object (task);
    /* xxx: g_assert_finalize_object (cm_db) fails sometimes when used as subproject */
    g_object_unref (cm_db);
    g_free (input_file);

    /* Attach old (now migrated) db with expected migrated db */
    input_file = g_test_build_filename (G_TEST_BUILT, name, NULL);
    strcpy (input_file + strlen (input_file) - strlen ("sql"), "db");
    /* The db that's verified is 'main' (shipped as testcase), and the
     * generated one by matrix-db is named 'test', which is to be tested */
    input = g_strconcat ("ATTACH '", input_file, "' as test;", NULL);
    status = sqlite3_exec (db, input, NULL, NULL, NULL);
    g_assert_cmpint (status, ==, SQLITE_OK);

    compare_matrix_db (db);
    sqlite3_close (db);
  }
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

  g_test_add_func ("/cm-db/new", test_cm_db_new);
  g_test_add_func ("/cm-db/account", test_cm_db_account);
  g_test_add_func ("/cm-db/migration", test_cm_db_migration);

  return g_test_run ();
}
