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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <glib.h>
#include <fcntl.h>
#include <sqlite3.h>

#include "cm-enc-private.h"
#include "cm-client-private.h"
#include "cm-room-private.h"
#include "cm-utils-private.h"
#include "cm-db-private.h"

#define STRING(arg) STRING_VALUE(arg)
#define STRING_VALUE(arg) #arg

/* increment when DB changes */
#define DB_VERSION 1

struct _CmDb
{
  GObject      parent_instance;

  GAsyncQueue *queue;
  GThread     *worker_thread;
  sqlite3     *db;
  char        *db_path;
};

#define VERIFICATION_UNSET       0
#define VERIFICATION_KNOWN       1
#define VERIFICATION_VERIFIED    2
#define VERIFICATION_BLACKLISTED 3
#define VERIFICATION_IGNORED     4

#define ENCRYPTION_NONE                       0
#define ENCRYPTION_MEGOLM_V1_AES_SHA2         1
#define ENCRYPTION_OLM_v1_curve25519_AES_SHA2 2

/*
 * CmDb->db should never be accessed nor modified in main thread
 * except for checking if it’s %NULL.  Any operation should be done only
 * in @worker_thread.  Don't reuse the same #CmDb once closed.
 *
 * Always copy data with g_object_set_data_full() or similar if the data can change
 * (regardless of whether the data has changed or not), so as to avoid surprises
 * with multi-thread stuff.
 */

typedef void (*CmDbCallback) (CmDb  *self,
                              GTask *task);

G_DEFINE_TYPE (CmDb, cm_db, G_TYPE_OBJECT)

static void
warn_if_sql_error (int         status,
                   const char *message)
{
  if (status == SQLITE_OK || status == SQLITE_ROW || status == SQLITE_DONE)
    return;

  g_warning ("Error %s. errno: %d, message: %s", message, status, sqlite3_errstr (status));
}

static void
matrix_bind_text (sqlite3_stmt *statement,
                  guint         position,
                  const char   *bind_value,
                  const char   *message)
{
  guint status;

  status = sqlite3_bind_text (statement, position, bind_value, -1, SQLITE_TRANSIENT);
  warn_if_sql_error (status, message);
}

static void
matrix_bind_int (sqlite3_stmt *statement,
                 guint         position,
                 int           bind_value,
                 const char   *message)
{
  guint status;

  status = sqlite3_bind_int (statement, position, bind_value);
  warn_if_sql_error (status, message);
}

/**
   user_devices.json_data
   - local
   -  device_display_name
   users.json_data
   - local
   -  status = unknown, known, blocked, verified,
   accounts.json_data
   - local
   -  filter
   -    id = text
   -    version = int
   rooms.json_data
   - local
   -  direct = bool
   -  state = int (joined, invited, left, unknown)
   -  name_loaded = bool
   -  alias = string
   -  encryption = int (0 = "none", 1 = "m.megolm.v1.aes-sha2")
   -  rotation_period_ms = time_t (or may be use μs as provided by server?)
   -  rotation_count_msgs = int (message count max for key change)
   session.json_data
   - local
   -  key_added_date = time_t (or may be use μs as provided by server?)
*/
static gboolean
cm_db_create_schema (CmDb  *self,
                     GTask *task)
{
  const char *sql;
  char *error = NULL;
  int status;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  sql = "BEGIN TRANSACTION;"

    "PRAGMA user_version = " STRING (DB_VERSION) ";"

    "CREATE TABLE IF NOT EXISTS users ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    /* Version 2: Unique */
    "username TEXT NOT NULL UNIQUE, "
    /* Version 2 */
    "outdated INTEGER DEFAULT 1, "
    /* Version 2 */
    "json_data TEXT);"

    /* Version 2 */
    "CREATE TABLE IF NOT EXISTS user_devices ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "user_id INTEGER NOT NULL REFERENCES users(id), "
    "device TEXT NOT NULL, "
    "curve25519_key TEXT, "
    "ed25519_key TEXT, "
    "verification INTEGER DEFAULT 0, "
    "json_data TEXT, "
    "UNIQUE (user_id, device));"

    "CREATE TABLE IF NOT EXISTS accounts ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    /* Version 2 */
    "user_device_id INTEGER NOT NULL REFERENCES user_devices(id), "
    "next_batch TEXT, "
    "pickle TEXT, "
    "enabled INTEGER DEFAULT 0, "
    /* Version 2 */
    "json_data TEXT, "
    "UNIQUE (user_device_id));"

    "CREATE TABLE IF NOT EXISTS rooms ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "account_id INTEGER NOT NULL REFERENCES accounts(id), "
    "room_name TEXT NOT NULL, "
    "prev_batch TEXT, "
    /* Version 2 */
    "json_data TEXT, "
    "UNIQUE (account_id, room_name));"

    "CREATE TABLE IF NOT EXISTS encryption_keys ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "file_url TEXT NOT NULL, "
    "file_sha256 TEXT, "
    /* Initialization vector: iv in JSON */
    "iv TEXT NOT NULL, "
    /* v in JSON */
    "version INT DEFAULT 2 NOT NULL, "
    /* alg in JSON */
    "algorithm INT NOT NULL, "
    /* k in JSON */
    "key TEXT NOT NULL, "
    /* kty in JSON */
    "type INT NOT NULL, "
    /* ext in JSON */
    "extractable INT DEFAULT 1 NOT NULL, "
    /* Version 2 */
    "json_data TEXT, "
    "UNIQUE (file_url));"

    "CREATE TABLE IF NOT EXISTS session ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "account_id INTEGER NOT NULL REFERENCES accounts(id), "
    "sender_key TEXT NOT NULL, "
    "session_id TEXT NOT NULL, "
    "type INTEGER NOT NULL, "
    "pickle TEXT NOT NULL, "
    "time INT, "
    /* Version 2 */
    "room_id INTEGER REFERENCES rooms(id), "
    /* Version 2 */
    "json_data TEXT, "
    "UNIQUE (account_id, sender_key, session_id));"

    "COMMIT;";

  status = sqlite3_exec (self->db, sql, NULL, NULL, &error);

  if (status != SQLITE_OK)
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               "Error creating table. errno: %d, desc: %s. %s",
                               status, sqlite3_errmsg (self->db), error);
      return FALSE;
    }

  return TRUE;
}

static int
cm_db_get_db_version (CmDb  *self,
                      GTask *task)
{
  sqlite3_stmt *stmt;
  int status, version = -1;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  sqlite3_prepare_v2 (self->db, "PRAGMA user_version;", -1, &stmt, NULL);
  status = sqlite3_step (stmt);

  if (status == SQLITE_ROW)
    version = sqlite3_column_int (stmt, 0);
  else
    g_task_return_new_error (task,
                             G_IO_ERROR,
                             G_IO_ERROR_FAILED,
                             "Couldn't get database version. error: %s",
                             sqlite3_errmsg (self->db));
  sqlite3_finalize (stmt);

  return version;
}

static void
cm_db_backup (CmDb *self)
{
  g_autoptr(GFile) backup_db = NULL;
  g_autoptr(GFile) old_db = NULL;
  g_autofree char *backup_name = NULL;
  g_autofree char *time = NULL;
  g_autoptr(GDateTime) date = NULL;
  g_autoptr(GError) error = NULL;

  date = g_date_time_new_now_local ();
  time = g_date_time_format (date, "%Y-%m-%d-%H%M%S");
  backup_name = g_strdup_printf ("%s.%s", self->db_path, time);
  g_info ("Copying database for backup");

  old_db = g_file_new_for_path (self->db_path);
  backup_db = g_file_new_for_path (backup_name);
  g_file_copy (old_db, backup_db, G_FILE_COPY_NONE, NULL, NULL, NULL, &error);
  g_info ("Copying database success: %d", !error);

  if (error &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
    g_error ("Error creating DB backup: %s", error->message);
}

static gboolean
cm_db_migrate_db_v2 (CmDb  *self,
                     GTask *task)
{
  char *error = NULL;
  int status;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);

  cm_db_backup (self);

  status = sqlite3_exec (self->db,
                         "BEGIN TRANSACTION;"
                         "PRAGMA foreign_keys=OFF;"

                         "CREATE TABLE IF NOT EXISTS tmp_users ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         "username TEXT NOT NULL UNIQUE, "
                         "outdated INTEGER DEFAULT 1, "
                         "json_data TEXT "
                         ");"

                         "CREATE TABLE IF NOT EXISTS user_devices ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         "user_id INTEGER NOT NULL REFERENCES users(id), "
                         "device TEXT NOT NULL, "
                         "curve25519_key TEXT, "
                         "ed25519_key TEXT, "
                         "verification INTEGER DEFAULT 0, "
                         "json_data TEXT, "
                         "UNIQUE (user_id, device));"

                         "CREATE TABLE IF NOT EXISTS tmp_accounts ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         "user_device_id INTEGER NOT NULL REFERENCES user_devices(id), "
                         "next_batch TEXT, "
                         "pickle TEXT, "
                         "enabled INTEGER DEFAULT 0, "
                         "json_data TEXT, "
                         "UNIQUE (user_device_id));"

                         "CREATE TABLE IF NOT EXISTS tmp_users ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         "username TEXT NOT NULL UNIQUE, "
                         "outdated INTEGER DEFAULT 1, "
                         "json_data TEXT "
                         ");"

                         "INSERT INTO tmp_users(username) "
                         "SELECT DISTINCT username FROM users;"

                         "INSERT INTO user_devices(user_id,device) "
                         "SELECT tmp_users.id,devices.device FROM tmp_users "
                         "JOIN users ON users.username=tmp_users.username "
                         "JOIN devices ON users.device_id=devices.id;"

                         "INSERT INTO tmp_accounts(user_device_id,next_batch,pickle,enabled) "
                         "SELECT user_devices.id,next_batch,pickle,enabled FROM accounts "
                         "JOIN users ON users.id=accounts.user_id "
                         "JOIN devices ON users.device_id=devices.id "
                         "JOIN user_devices ON user_devices.device=devices.device "
                         "JOIN tmp_users ON user_devices.user_id=tmp_users.id "
                         "AND tmp_users.username=users.username;"

                         "UPDATE session SET account_id=(SELECT tmp_accounts.id "
                         "FROM tmp_accounts "
                         "INNER JOIN accounts ON accounts.pickle=tmp_accounts.pickle "
                         "AND session.account_id=accounts.id"
                         ");"

                         "UPDATE rooms SET account_id=(SELECT tmp_accounts.id "
                         "FROM tmp_accounts "
                         "INNER JOIN accounts ON accounts.pickle=tmp_accounts.pickle "
                         "AND rooms.account_id=accounts.id"
                         ");"

                         "DROP TABLE IF EXISTS users;"
                         "DROP TABLE IF EXISTS accounts;"
                         "DROP TABLE IF EXISTS devices;"

                         "ALTER TABLE tmp_users RENAME TO users;"
                         "ALTER TABLE tmp_accounts RENAME TO accounts;"

                         "ALTER TABLE rooms ADD COLUMN json_data TEXT;"

                         "ALTER TABLE encryption_keys ADD COLUMN json_data TEXT;"

                         "ALTER TABLE session ADD COLUMN room_id "
                         "INTEGER REFERENCES rooms(id);"
                         "ALTER TABLE session ADD COLUMN json_data TEXT;"

                         "PRAGMA user_version = 1;"

                         "COMMIT;"

                         "PRAGMA foreign_keys=ON;",
                         NULL, NULL, &error);

  g_debug ("Migrating db to version %d, success: %d", DB_VERSION, !error);

  if (status == SQLITE_OK || status == SQLITE_DONE)
    return TRUE;

  g_task_return_new_error (task,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Couldn't migrate to new db. errno: %d. %s",
                           status, error);
  sqlite3_free (error);

  return FALSE;
}

static gboolean
cm_db_migrate (CmDb  *self,
               GTask *task)
{
  int version;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  version = cm_db_get_db_version (self, task);

  if (version == DB_VERSION)
    return TRUE;

  switch (version) {
  case -1:  /* Error */
    return FALSE;

  case 0:
    if (!cm_db_migrate_db_v2 (self, task))
      return FALSE;
    break;

  default:
    g_task_return_new_error (task,
                             G_IO_ERROR,
                             G_IO_ERROR_FAILED,
                             "Failed to migrate from unknown version %d",
                             version);
    return FALSE;
  }

  return TRUE;
}

static int
matrix_db_get_user_id (CmDb       *self,
                       const char *username,
                       gboolean    insert_if_missing)
{
  sqlite3_stmt *stmt;
  int user_id = 0;

  if (!username || !*username)
    return 0;

  g_assert (CM_IS_DB (self));

  sqlite3_prepare_v2 (self->db,
                      "SELECT id FROM users WHERE username=?",
                      -1, &stmt, NULL);
  matrix_bind_text (stmt, 1, username, "binding when selecting user");
  if (sqlite3_step (stmt) == SQLITE_ROW)
    user_id = sqlite3_column_int (stmt, 0);
  sqlite3_finalize (stmt);

  if (user_id || !insert_if_missing)
    return user_id;

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO users(username) VALUES(?1)",
                      -1, &stmt, NULL);
  matrix_bind_text (stmt, 1, username, "binding when adding user");
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  user_id = sqlite3_last_insert_rowid (self->db);

  return user_id;
}

static int
matrix_db_get_user_device_id (CmDb       *self,
                              const char *username,
                              const char *device,
                              int        *out_user_id,
                              gboolean    insert_if_missing)
{
  sqlite3_stmt *stmt;
  int user_id = 0, user_device_id = 0;

  if (!username || !*username || !device || !*device)
    return 0;

  g_assert (CM_IS_DB (self));

  user_id = matrix_db_get_user_id (self, username, insert_if_missing);

  if (out_user_id)
    *out_user_id = user_id;

  if (!user_id)
    return 0;

  if (device && *device)
    {
      sqlite3_prepare_v2 (self->db,
                          "SELECT user_devices.id FROM user_devices "
                          "WHERE user_id=?1 AND user_devices.device=?2",
                          -1, &stmt, NULL);
      matrix_bind_int (stmt, 1, user_id, "binding when getting user device");
      matrix_bind_text (stmt, 2, device, "binding when getting user device");
    }
  else
    {
      sqlite3_prepare_v2 (self->db,
                          "SELECT user_devices.id FROM user_devices "
                          "WHERE user_id=?1 AND user_devices.device IS NULL LIMIT 1",
                          -1, &stmt, NULL);
      matrix_bind_int (stmt, 1, user_id, "binding when getting user device");
    }

  if (sqlite3_step (stmt) == SQLITE_ROW)
    user_device_id = sqlite3_column_int (stmt, 0);
  sqlite3_finalize (stmt);

  if (user_device_id || !insert_if_missing || !device || !*device)
    return user_device_id;

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO user_devices(user_id, device) VALUES(?1, ?2)",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, user_id, "binding when adding user device");
  matrix_bind_text (stmt, 2, device, "binding when adding user device");
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  user_device_id = sqlite3_last_insert_rowid (self->db);

  return user_device_id;
}

static int
matrix_db_get_account_id (CmDb       *self,
                          const char *username,
                          const char *device,
                          int        *out_user_device_id,
                          gboolean    insert_if_missing)
{
  sqlite3_stmt *stmt;
  int user_device_id = 0;

  if (!username || !*username || !device || !*device)
    return 0;

  user_device_id = matrix_db_get_user_device_id (self, username, device, NULL, insert_if_missing);

  if (out_user_device_id)
    *out_user_device_id = user_device_id;

  if (!user_device_id)
    return 0;

  sqlite3_prepare_v2 (self->db,
                      "SELECT accounts.id FROM accounts "
                      "WHERE user_device_id=?1;",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, user_device_id, "binding when getting account id");
  if (sqlite3_step (stmt) == SQLITE_ROW)
    return sqlite3_column_int (stmt, 0);

  if (!insert_if_missing)
    return 0;

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO accounts(user_device_id) "
                      "VALUES(?1)",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, user_device_id, "binding when updating account");
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  return sqlite3_last_insert_rowid (self->db);
}

static void
matrix_open_db (CmDb  *self,
                GTask *task)
{
  const char *dir, *file_name;
  sqlite3 *db;
  int status;
  gboolean db_exists;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (!self->db);

  dir = g_object_get_data (G_OBJECT (task), "dir");
  file_name = g_object_get_data (G_OBJECT (task), "file-name");
  g_assert (dir && *dir);
  g_assert (file_name && *file_name);

  g_mkdir_with_parents (dir, S_IRWXU);
  self->db_path = g_build_filename (dir, file_name, NULL);

  db_exists = g_file_test (self->db_path, G_FILE_TEST_EXISTS);
  status = sqlite3_open (self->db_path, &db);

  if (status == SQLITE_OK) {
    self->db = db;

    if (db_exists) {
      if (!cm_db_migrate (self, task))
        return;
    } else {
      if (!cm_db_create_schema (self, task))
        return;
    }

    sqlite3_exec (self->db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    g_task_return_boolean (task, TRUE);
  } else {
    g_task_return_boolean (task, FALSE);
    sqlite3_close (db);
  }
}

static void
matrix_close_db (CmDb  *self,
                 GTask *task)
{
  sqlite3 *db;
  int status;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  db = self->db;
  self->db = NULL;
  status = sqlite3_close (db);

  if (status == SQLITE_OK)
    {
      /*
       * We can’t know when will @self associated with the task will
       * be unref.  So cm_db_get_default() called immediately
       * after this may return the @self that is yet to be free.  But
       * as the worker_thread is exited after closing the database, any
       * actions with the same @self will not execute, and so the tasks
       * will take ∞ time to complete.
       *
       * So Instead of relying on GObject to free the object, Let’s
       * explicitly run dispose
       */
      g_object_run_dispose (G_OBJECT (self));
      g_debug ("Database closed successfully");
      g_task_return_boolean (task, TRUE);
    }
  else
    {
      g_task_return_new_error (task,
                               G_IO_ERROR,
                               G_IO_ERROR_FAILED,
                               "Database could not be closed. errno: %d, desc: %s",
                               status, sqlite3_errmsg (db));
    }
}

static void
cm_db_save_client (CmDb  *self,
                   GTask *task)
{
  const char *device, *pickle, *username, *batch, *filter;
  g_autofree char *json_str = NULL;
  JsonObject *root, *obj;
  sqlite3_stmt *stmt;
  int status, user_device_id = 0, account_id = 0;
  gboolean enabled;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  batch = g_object_get_data (G_OBJECT (task), "batch");
  pickle = g_object_get_data (G_OBJECT (task), "pickle");
  device = g_object_get_data (G_OBJECT (task), "device");
  enabled = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "enabled"));
  username = g_object_get_data (G_OBJECT (task), "username");
  filter = g_object_get_data (G_OBJECT (task), "filter-id");

  account_id = matrix_db_get_account_id (self, username, device, &user_device_id, TRUE);

  if (!account_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Failed to add account to db");
      return;
    }

  root = json_object_new ();
  obj = json_object_new ();
  json_object_set_object_member (root, "local", obj);

  if (filter && *filter)
    json_object_set_string_member (obj, "filter-id", filter);

  json_str = cm_utils_json_object_to_string (root, FALSE);

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO accounts(user_device_id,pickle,"
                      "next_batch,enabled,json_data) "
                      "VALUES(?1,?2,?3,?4,?5) "
                      "ON CONFLICT(user_device_id) "
                      "DO UPDATE SET pickle=?2, next_batch=?3, enabled=?4, json_data=?5",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, user_device_id, "binding when updating account");
  if (pickle && *pickle)
    matrix_bind_text (stmt, 2, pickle, "binding when updating account");
  matrix_bind_text (stmt, 3, batch, "binding when updating account");
  matrix_bind_int (stmt, 4, enabled, "binding when updating account");
  matrix_bind_text (stmt, 5, json_str, "binding when updating account");

  status = sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  if (status == SQLITE_DONE)
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_new_error (task,
                             G_IO_ERROR,
                             G_IO_ERROR_FAILED,
                             "Error saving account. errno: %d, desc: %s",
                             status, sqlite3_errmsg (self->db));
}

static int
cm_db_get_room_id (CmDb       *self,
                   GTask      *task,
                   const char *room,
                   int         account_id)
{
  sqlite3_stmt *stmt;
  const char *error;
  int status;

  g_assert (room && *room);
  g_assert (account_id);

  sqlite3_prepare_v2 (self->db,
                      "SELECT rooms.id FROM rooms "
                      "WHERE room_name=? and account_id=?",
                      -1, &stmt, NULL);
  matrix_bind_text (stmt, 1, room, "binding when getting room id");
  matrix_bind_int (stmt, 2, account_id, "binding when getting room id");

  status = sqlite3_step (stmt);
  if (status == SQLITE_ROW)
    return sqlite3_column_int (stmt, 0);

  if (status == SQLITE_DONE)
    error = "Room not found in db";
  else
    error = sqlite3_errmsg (self->db);

  g_task_return_new_error (task,
                           G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Couldn't find room %s. error: %s",
                           room, error);
  return 0;
}

static GPtrArray *
cm_db_get_rooms (CmDb *self,
                 int   account_id)
{
  g_autoptr(GPtrArray) rooms = NULL;
  sqlite3_stmt *stmt;

  g_assert (CM_IS_DB (self));
  g_assert (account_id);

  rooms = g_ptr_array_new_full (32, g_object_unref);

  sqlite3_prepare_v2 (self->db,
                      "SELECT room_name,prev_batch,json_data FROM rooms "
                      "WHERE account_id=?",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, account_id, "binding when getting rooms");

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      char *room_id, *prev_batch, *json_str;
      g_autoptr(JsonObject) json = NULL;
      JsonObject *child;
      CmRoom *room;

      room_id = (char *)sqlite3_column_text (stmt, 0);
      prev_batch = (char *)sqlite3_column_text (stmt, 1);
      json_str = (char *)sqlite3_column_text (stmt, 2);

      room = cm_room_new (room_id);
      cm_room_set_prev_batch (room, prev_batch);

      if (json_str && *json_str)
        json = cm_utils_string_to_json_object (json_str);

      child = cm_utils_json_object_get_object (json, "local");
      cm_room_set_name (room, cm_utils_json_object_get_string (child, "alias"));
      cm_room_set_is_direct (room, cm_utils_json_object_get_bool (child, "direct"));
      cm_room_set_is_encrypted (room, cm_utils_json_object_get_int (child, "encryption") > 0);

      g_ptr_array_add (rooms, room);
    }

  sqlite3_finalize (stmt);

  return g_steal_pointer (&rooms);
}

static void
cm_db_load_client (CmDb  *self,
                   GTask *task)
{
  sqlite3_stmt *stmt;
  char *username, *device_id;
  int status, account_id = 0;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  username = g_object_get_data (G_OBJECT (task), "username");
  device_id = g_object_get_data (G_OBJECT (task), "device");

  account_id = matrix_db_get_account_id (self, username, device_id, NULL, FALSE);

  if (!account_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "Account not in db");
      return;
    }

  status = sqlite3_prepare_v2 (self->db,
                               "SELECT pickle,next_batch,json_data "
                               "FROM accounts WHERE accounts.id=?",
                               -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, account_id, "binding when loading account");
  status = sqlite3_step (stmt);

  if (status == SQLITE_ROW)
    {
      const char *filter;
      GObject *object = G_OBJECT (task);
      g_autoptr(JsonObject) json = NULL;
      JsonObject *child;
      GPtrArray *rooms;

      g_object_set_data_full (object, "pickle", g_strdup ((char *)sqlite3_column_text (stmt, 0)), g_free);
      g_object_set_data_full (object, "batch", g_strdup ((char *)sqlite3_column_text (stmt, 1)), g_free);

      json = cm_utils_string_to_json_object ((char *)sqlite3_column_text (stmt, 2));
      child = cm_utils_json_object_get_object (json, "local");
      filter = cm_utils_json_object_get_string (child, "filter-id");

      if (filter && *filter)
        g_object_set_data_full (object, "filter-id", g_strdup (filter), g_free);

      rooms = cm_db_get_rooms (self, account_id);
      g_object_set_data_full (object, "rooms", rooms, (GDestroyNotify)g_ptr_array_unref);
    }

  sqlite3_finalize (stmt);
  g_task_return_boolean (task, status == SQLITE_ROW);
}

static void
cm_db_save_room (CmDb  *self,
                 GTask *task)
{
  g_autofree char *json_str = NULL;
  CmRoom *room;
  JsonObject *root, *obj;
  const char *username, *client_device, *room_alias, *prev_batch;
  sqlite3_stmt *stmt;
  int account_id;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  username = g_object_get_data (G_OBJECT (task), "username");
  client_device = g_object_get_data (G_OBJECT (task), "client-device");
  room = g_object_get_data (G_OBJECT (task), "room");
  room_alias = g_object_get_data (G_OBJECT (task), "room-alias");
  prev_batch = g_object_get_data (G_OBJECT (task), "prev-batch");

  account_id = matrix_db_get_account_id (self, username, client_device, NULL, FALSE);

  if (!account_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  root = json_object_new ();
  obj = json_object_new ();
  json_object_set_object_member (root, "local", obj);

  json_object_set_boolean_member (obj, "direct", cm_room_is_direct (room));
  json_object_set_string_member (obj, "alias", room_alias);
  json_object_set_int_member (obj, "encryption", cm_room_is_encrypted (room));

  json_str = cm_utils_json_object_to_string (root, FALSE);

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO rooms(account_id,room_name,prev_batch,json_data) "
                      "VALUES(?1,?2,?3,?4) "
                      "ON CONFLICT(account_id, room_name) "
                      "DO UPDATE SET prev_batch=?3,json_data=?4",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, account_id, "binding when saving room");
  matrix_bind_text (stmt, 2, cm_room_get_id (room), "binding when saving room");
  matrix_bind_text (stmt, 3, prev_batch, "binding when saving room");
  matrix_bind_text (stmt, 4, json_str, "binding when saving room");

  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  g_task_return_boolean (task, TRUE);
}

static void
cm_db_load_room (CmDb  *self,
                 GTask *task)
{
  const char *username, *room_name, *device_name;
  sqlite3_stmt *stmt;
  int account_id;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  username = g_object_get_data (G_OBJECT (task), "account-id");
  room_name = g_object_get_data (G_OBJECT (task), "room-id");
  device_name = g_object_get_data (G_OBJECT (task), "device-id");

  account_id = matrix_db_get_account_id (self, username, device_name, NULL, FALSE);

  if (!account_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  sqlite3_prepare_v2 (self->db,
                      "SELECT prev_batch,json_data FROM rooms "
                      "WHERE account_id=? AND room_name=? ",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, account_id, "binding when loading room");
  matrix_bind_text (stmt, 2, room_name, "binding when loading room");

  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      g_task_return_pointer (task, g_strdup ((char *)sqlite3_column_text (stmt, 1)), g_free);
      g_object_set_data_full (G_OBJECT (task), "prev-batch",
                              g_strdup ((char *)sqlite3_column_text (stmt, 0)), g_free);
    }
  else
    {
      g_task_return_pointer (task, NULL, NULL);
    }

  sqlite3_finalize (stmt);
}

static void
cm_db_delete_client (CmDb  *self,
                     GTask *task)
{
  sqlite3_stmt *stmt;
  char *username, *device_id;
  int account_id;
  int status;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  username = g_object_get_data (G_OBJECT (task), "username");
  device_id = g_object_get_data (G_OBJECT (task), "device-id");

  account_id = matrix_db_get_account_id (self, username, device_id, NULL, FALSE);

  if (!account_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  status = sqlite3_prepare_v2 (self->db,
                               "DELETE FROM session "
                               "WHERE session.account_id=?1; ",
                               -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, account_id, "binding when deleting account");
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  status = sqlite3_prepare_v2 (self->db,
                               "DELETE FROM rooms "
                               "WHERE rooms.account_id=?1; ",
                               -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, account_id, "binding when deleting account");
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  status = sqlite3_prepare_v2 (self->db,
                               "DELETE FROM accounts "
                               "WHERE accounts.id=?1; ",
                               -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, account_id, "binding when deleting account");

  status = sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  g_task_return_boolean (task, status == SQLITE_ROW);
}

static void
cm_db_add_session (CmDb  *self,
                   GTask *task)
{
  sqlite3_stmt *stmt;
  const char *username, *account_device, *session_id, *sender_key, *pickle, *room;
  CmSessionType type;
  int status, account_id, room_id = 0;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "type"));

  room = g_object_get_data (G_OBJECT (task), "room-id");
  pickle = g_object_get_data (G_OBJECT (task), "pickle");
  username = g_object_get_data (G_OBJECT (task), "account-id");
  session_id = g_object_get_data (G_OBJECT (task), "session-id");
  sender_key = g_object_get_data (G_OBJECT (task), "sender-key");
  account_device = g_object_get_data (G_OBJECT (task), "device-id");

  account_id = matrix_db_get_account_id (self, username, account_device, NULL, FALSE);

  if (!account_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  if (room)
    room_id = cm_db_get_room_id (self, task, room, account_id);

  status = sqlite3_prepare_v2 (self->db,
                               "INSERT INTO session(account_id,sender_key,session_id,type,pickle,room_id) "
                               "VALUES(?1,?2,?3,?4,?5,?6)",
                               -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, account_id, "binding when adding session");
  matrix_bind_text (stmt, 2, sender_key, "binding when adding session");
  matrix_bind_text (stmt, 3, session_id, "binding when adding session");
  matrix_bind_int (stmt, 4, type, "binding when adding session");
  matrix_bind_text (stmt, 5, pickle, "binding when adding session");
  if (room_id)
    matrix_bind_int (stmt, 6, room_id, "binding when adding session");

  status = sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  g_task_return_boolean (task, status == SQLITE_DONE);
}

static void
cm_db_save_file_enc (CmDb  *self,
                     GTask *task)
{
  CmEncFileInfo *file;
  sqlite3_stmt *stmt;
  int status, algorithm = 0, version = 0, type = 0;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  file = g_object_get_data (G_OBJECT (task), "file");
  g_assert (file && file->mxc_uri);

  if (g_strcmp0 (file->algorithm, "A256CTR") == 0)
    algorithm = CMATRIX_ALGORITHM_A256CTR;

  if (g_strcmp0 (file->kty, "oct") == 0)
    type = CMATRIX_KEY_TYPE_OCT;

  if (g_strcmp0 (file->version, "v2") == 0)
    version = 2;

  status = sqlite3_prepare_v2 (self->db,
                               "INSERT INTO encryption_keys(file_url,file_sha256,"
                               "iv,version,algorithm,key,type,extractable) "
                               "VALUES(?1,?2,?3,?4,?5,?6,?7,?8)",
                               -1, &stmt, NULL);

  matrix_bind_text (stmt, 1, file->mxc_uri, "binding when adding file url");
  matrix_bind_text (stmt, 2, file->sha256_base64, "binding when adding file url");
  matrix_bind_text (stmt, 3, file->aes_iv_base64, "binding when adding file url");
  matrix_bind_int (stmt, 4, version, "binding when adding file url");
  matrix_bind_int (stmt, 5, algorithm, "binding when adding file url");
  matrix_bind_text (stmt, 6, file->aes_key_base64, "binding when adding file url");
  matrix_bind_int (stmt, 7, type, "binding when adding file url");
  matrix_bind_int (stmt, 8, file->extractable, "binding when adding file url");

  status = sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  g_task_return_boolean (task, status == SQLITE_DONE);
}

static void
cm_db_find_file_enc (CmDb  *self,
                     GTask *task)
{
  sqlite3_stmt *stmt;
  CmEncFileInfo *file = NULL;
  char *uri;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  uri = g_object_get_data (G_OBJECT (task), "uri");
  g_assert (uri && *uri);

  sqlite3_prepare_v2 (self->db,
                      "SELECT file_sha256,iv,key "
                      "FROM encryption_keys WHERE file_url=?1",
                      -1, &stmt, NULL);
  matrix_bind_text (stmt, 1, uri, "binding when looking up file encryption");

  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      file = g_new0 (CmEncFileInfo, 1);
      file->mxc_uri = g_strdup (uri);
      file->sha256_base64 = g_strdup ((char *)sqlite3_column_text (stmt, 0));
      file->aes_iv_base64 = g_strdup ((char *)sqlite3_column_text (stmt, 1));
      file->aes_key_base64 = g_strdup ((char *)sqlite3_column_text (stmt, 2));

      if (!g_str_has_prefix (uri, "mxc://"))
        g_clear_pointer (&file->mxc_uri, g_free);
    }

  g_task_return_pointer (task, file, cm_enc_file_info_free);
}

static void
db_lookup_session (CmDb  *self,
                   GTask *task)
{
  sqlite3_stmt *stmt;
  const char *username, *account_device, *session_id, *sender_key;
  char *pickle = NULL;
  CmSessionType type;
  int status, account_id;

  g_assert (CM_IS_DB (self));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (G_IS_TASK (task));

  type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "type"));

  username = g_object_get_data (G_OBJECT (task), "account-id");
  session_id = g_object_get_data (G_OBJECT (task), "session-id");
  sender_key = g_object_get_data (G_OBJECT (task), "sender-key");
  account_device = g_object_get_data (G_OBJECT (task), "account-device");

  account_id = matrix_db_get_account_id (self, username, account_device, NULL, FALSE);

  if (!account_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  sqlite3_prepare_v2 (self->db,
                      "SELECT pickle FROM session "
                      "WHERE account_id=? AND sender_key=? AND session_id=? AND type=?",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, account_id, "binding when adding session");
  matrix_bind_text (stmt, 2, sender_key, "binding when looking up session");
  matrix_bind_text (stmt, 3, session_id, "binding when looking up session");
  matrix_bind_int (stmt, 4, type, "binding when looking up session");

  status = sqlite3_step (stmt);

  if (status == SQLITE_ROW)
    pickle = g_strdup ((char *)sqlite3_column_text (stmt, 0));

  sqlite3_finalize (stmt);
  g_task_return_pointer (task, pickle, g_free);
}

static void
db_lookup_olm_session (CmDb  *self,
                       GTask *task)
{
  sqlite3_stmt *stmt;
  const char *username, *account_device, *sender_curve_key, *body;
  char *pickle_key, *plain_text = NULL;
  gpointer session = NULL;
  CmSessionType type;
  size_t message_type;
  int account_id;

  g_assert (CM_IS_DB (self));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (G_IS_TASK (task));

  type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "type"));
  message_type = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (task), "message-type"));

  username = g_object_get_data (G_OBJECT (task), "account-id");
  sender_curve_key = g_object_get_data (G_OBJECT (task), "sender-key");
  account_device = g_object_get_data (G_OBJECT (task), "account-device");
  body = g_object_get_data (G_OBJECT (task), "body");
  pickle_key = g_strdup (g_object_get_data (G_OBJECT (task), "pickle-key"));

  account_id = matrix_db_get_account_id (self, username, account_device, NULL, FALSE);

  if (!account_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  sqlite3_prepare_v2 (self->db,
                      "SELECT pickle FROM session "
                      "WHERE account_id=? AND sender_key=? AND type=?",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, account_id, "binding when looking up olm session");
  matrix_bind_text (stmt, 2, sender_curve_key, "binding when looking up olm session");
  matrix_bind_int (stmt, 3, type, "binding when looking up olm session");

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      char *pickle;

      pickle = (char *)sqlite3_column_text (stmt, 0);
      session = cm_enc_olm_session_match (body, strlen (body), message_type,
                                          pickle, pickle_key, &plain_text);

      if (session)
        break;
    }

  cm_utils_free_buffer (pickle_key);
  sqlite3_finalize (stmt);

  g_object_set_data_full (G_OBJECT (task), "plaintext", plain_text,
                          (GDestroyNotify)cm_utils_free_buffer);
  g_task_return_pointer (task, session, g_free);
}

static void
history_get_olm_sessions (CmDb  *self,
                          GTask *task)
{
  GPtrArray *sessions = NULL;
  sqlite3_stmt *stmt;
  int status;

  g_assert (CM_IS_DB (self));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (G_IS_TASK (task));

  status = sqlite3_prepare_v2 (self->db,
                               "SELECT sender,sender_key,session_pickle FROM olm_session "
                               "ORDER BY id DESC LIMIT 100", -1, &stmt, NULL);

  warn_if_sql_error (status, "getting olm sessions");

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      CmDbData *data;

      if (!sessions)
        sessions = g_ptr_array_new_full (100, NULL);

      data = g_new (CmDbData, 1);
      data->sender = g_strdup ((char *)sqlite3_column_text (stmt, 0));
      data->sender_key = g_strdup ((char *)sqlite3_column_text (stmt, 1));
      data->session_pickle = g_strdup ((char *)sqlite3_column_text (stmt, 2));

      g_ptr_array_insert (sessions, 0, data);
    }

  status = sqlite3_finalize (stmt);
  warn_if_sql_error (status, "finalizing when getting olm sessions");

  g_task_return_pointer (task, sessions, (GDestroyNotify)g_ptr_array_unref);
}

static gpointer
cm_db_worker (gpointer user_data)
{
  CmDb *self = user_data;
  GTask *task;

  g_assert (CM_IS_DB (self));

  while ((task = g_async_queue_pop (self->queue)))
    {
      CmDbCallback callback;

      g_assert (task);
      callback = g_task_get_task_data (task);
      callback (self, task);
      g_object_unref (task);

      if (callback == matrix_close_db)
        break;
    }

  return NULL;
}

static void
ma_finish_cb (GObject      *object,
              GAsyncResult *result,
              gpointer      user_data)
{
  g_autoptr(GError) error = NULL;

  g_task_propagate_boolean (G_TASK (result), &error);

  if (error)
    g_warning ("Error: %s", error->message);

  g_task_return_boolean (G_TASK (user_data), !error);
}

static void
cm_db_close (CmDb *self)
{
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (CM_IS_DB (self));

  if (!self->db)
    return;

  task = g_task_new (NULL, NULL, NULL, NULL);
  cm_db_close_async (self, ma_finish_cb, task);

  /* Wait until the task is completed */
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);
}

static void
cm_db_dispose (GObject *object)
{
  CmDb *self = (CmDb *)object;

  cm_db_close (self);

  g_clear_pointer (&self->worker_thread, g_thread_unref);

  G_OBJECT_CLASS (cm_db_parent_class)->dispose (object);
}

static void
cm_db_finalize (GObject *object)
{
  CmDb *self = (CmDb *)object;

  if (self->db)
    g_warning ("Database not closed");

  g_clear_pointer (&self->queue, g_async_queue_unref);
  g_free (self->db_path);

  G_OBJECT_CLASS (cm_db_parent_class)->finalize (object);
}

static void
cm_db_class_init (CmDbClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose  = cm_db_dispose;
  object_class->finalize = cm_db_finalize;
}

static void
cm_db_init (CmDb *self)
{
  self->queue = g_async_queue_new ();
}

CmDb *
cm_db_new (void)
{
  return g_object_new (CM_TYPE_DB, NULL);
}

/**
 * cm_db_open_async:
 * @self: a #CmDb
 * @dir: (transfer full): The database directory
 * @file_name: The file name of database
 * @callback: a #GAsyncReadyCallback, or %NULL
 * @user_data: closure data for @callback
 *
 * Open the database file @file_name from path @dir.
 * Complete with cm_db_open_finish() to get
 * the result.
 */
void
cm_db_open_async (CmDb                *self,
                  char                *dir,
                  const char          *file_name,
                  GAsyncReadyCallback  callback,
                  gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (dir || !*dir);
  g_return_if_fail (file_name || !*file_name);

  if (self->db)
    {
      g_warning ("A DataBase is already open");
      return;
    }

  if (!self->worker_thread)
    self->worker_thread = g_thread_new ("matrix-db-worker",
                                        cm_db_worker,
                                        self);

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_open_async);
  g_task_set_task_data (task, matrix_open_db, NULL);
  g_object_set_data_full (G_OBJECT (task), "dir", dir, g_free);
  g_object_set_data_full (G_OBJECT (task), "file-name", g_strdup (file_name), g_free);

  g_async_queue_push (self->queue, task);
}

/**
 * cm_db_open_finish:
 * @self: a #CmDb
 * @result: a #GAsyncResult provided to callback
 * @error: a location for a #GError or %NULL
 *
 * Completes opening a database started with
 * cm_db_open_async().
 *
 * Returns: %TRUE if database was opened successfully.
 * %FALSE otherwise with @error set.
 */
gboolean
cm_db_open_finish (CmDb          *self,
                   GAsyncResult  *result,
                   GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

/**
 * cm_db_is_open:
 * @self: a #CmDb
 *
 * Get if the database is open or not
 *
 * Returns: %TRUE if a database is open.
 * %FALSE otherwise.
 */
gboolean
cm_db_is_open (CmDb *self)
{
  g_return_val_if_fail (CM_IS_DB (self), FALSE);

  return !!self->db;
}

/**
 * cm_db_close_async:
 * @self: a #CmDb
 * @callback: a #GAsyncReadyCallback, or %NULL
 * @user_data: closure data for @callback
 *
 * Close the database opened.
 * Complete with cm_db_close_finish() to get
 * the result.
 */
void
cm_db_close_async (CmDb                *self,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_close_async);
  g_task_set_task_data (task, matrix_close_db, NULL);

  g_async_queue_push (self->queue, task);
}

/**
 * cm_db_open_finish:
 * @self: a #CmDb
 * @result: a #GAsyncResult provided to callback
 * @error: a location for a #GError or %NULL
 *
 * Completes closing a database started with
 * cm_db_close_async().  @self is
 * g_object_unref() if closing succeeded.
 * So @self will be freed if you haven’t kept
 * your own reference on @self.
 *
 * Returns: %TRUE if database was closed successfully.
 * %FALSE otherwise with @error set.
 */
gboolean
cm_db_close_finish (CmDb          *self,
                    GAsyncResult  *result,
                    GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cm_db_save_client_async (CmDb                *self,
                         CmClient            *client,
                         char                *pickle,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GObject *object;
  GTask *task;
  const char *username;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_CLIENT (client));

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_save_client_async);
  g_task_set_task_data (task, cm_db_save_client, NULL);

  object = G_OBJECT (task);
  username = cm_client_get_user_id (client);

  if (g_application_get_default ())
    g_application_hold (g_application_get_default ());

  if (!username || !*username || !cm_client_get_device_id (client))
    {
      g_task_return_boolean (task, FALSE);
      return;
    }

  g_object_set_data (object, "enabled", GINT_TO_POINTER (cm_client_get_enabled (client)));
  g_object_set_data_full (object, "pickle", pickle, g_free);
  g_object_set_data_full (object, "device",
                          g_strdup (cm_client_get_device_id (client)), g_free);
  g_object_set_data_full (object, "batch",
                          g_strdup (cm_client_get_next_batch (client)), g_free);
  g_object_set_data_full (object, "username", g_strdup (username), g_free);
  g_object_set_data_full (object, "account", g_object_ref (client), g_object_unref);
  g_object_set_data_full (object, "filter-id",
                          g_strdup (cm_client_get_filter_id (client)), g_free);

  g_async_queue_push (self->queue, task);
}

gboolean
cm_db_save_client_finish (CmDb          *self,
                          GAsyncResult  *result,
                          GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  if (g_application_get_default ())
    g_application_release (g_application_get_default ());

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cm_db_load_client_async (CmDb                *self,
                         CmClient            *client,
                         const char          *device_id,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GTask *task;
  const char *username;

  g_return_if_fail (!device_id || *device_id);
  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_CLIENT (client));

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_load_client_async);
  g_task_set_task_data (task, cm_db_load_client, NULL);

  username = cm_client_get_user_id (client);

  if (!username || !*username || !device_id)
    {
      g_task_return_boolean (task, FALSE);
      return;
    }

  g_object_set_data_full (G_OBJECT (task), "device", g_strdup (device_id), g_free);
  g_object_set_data_full (G_OBJECT (task), "username", g_strdup (username), g_free);
  g_object_set_data_full (G_OBJECT (task), "client", g_object_ref (client), g_object_unref);

  g_async_queue_push_front (self->queue, task);
}

gboolean
cm_db_load_client_finish (CmDb          *self,
                          GAsyncResult  *result,
                          GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cm_db_save_room_async (CmDb                *self,
                       CmClient            *client,
                       CmRoom              *room,
                       GAsyncReadyCallback  callback,
                       gpointer             user_data)
{
  const char *username, *device_id, *room_alias, *prev_batch;
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_CLIENT (client));

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_save_room_async);
  g_task_set_task_data (task, cm_db_save_room, NULL);

  username = cm_client_get_user_id (client);
  device_id = cm_client_get_device_id (client);
  room_alias = cm_room_get_name (room);
  prev_batch = cm_room_get_prev_batch (room);

  g_object_set_data_full (G_OBJECT (task), "username", g_strdup (username), g_free);
  g_object_set_data_full (G_OBJECT (task), "room", g_object_ref (room), g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "room-alias", g_strdup (room_alias), g_free);
  g_object_set_data_full (G_OBJECT (task), "prev-batch", g_strdup (prev_batch), g_free);
  g_object_set_data_full (G_OBJECT (task), "client", g_object_ref (client), g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "client-device", g_strdup (device_id), g_free);

  g_async_queue_push (self->queue, task);
}

gboolean
cm_db_save_room_finish (CmDb          *self,
                        GAsyncResult  *result,
                        GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cm_db_load_room_async (CmDb                *self,
                       CmClient            *client,
                       CmRoom              *room,
                       GAsyncReadyCallback  callback,
                       gpointer             user_data)
{
  const char *username, *device_id, *room_id;
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (CM_IS_ROOM (room));

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_load_room_async);
  g_task_set_task_data (task, cm_db_load_room, NULL);

  username = cm_client_get_user_id (client);
  device_id = cm_client_get_device_id (client);
  room_id = cm_room_get_id (room);

  g_object_set_data_full (G_OBJECT (task), "room-id", g_strdup (room_id), g_free);
  g_object_set_data_full (G_OBJECT (task), "username", g_strdup (username), g_free);
  g_object_set_data_full (G_OBJECT (task), "account-id", g_strdup (username), g_free);
  g_object_set_data_full (G_OBJECT (task), "device-id", g_strdup (device_id), g_free);

  g_object_set_data_full (G_OBJECT (task), "client", g_object_ref (client), g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "room", g_object_ref (room), g_object_unref);

  g_async_queue_push (self->queue, task);
}

char *
cm_db_load_room_finish (CmDb      *self,
                            GAsyncResult  *result,
                            GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_pointer (G_TASK (result), error);
}

void
cm_db_delete_client_async (CmDb                *self,
                           CmClient            *client,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  const char *username, *device_id;
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_CLIENT (client));

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_delete_client_async);
  g_task_set_task_data (task, cm_db_delete_client, NULL);

  username = cm_client_get_user_id (client);
  device_id = cm_client_get_device_id (client);

  g_object_set_data_full (G_OBJECT (task), "username", g_strdup (username), g_free);
  g_object_set_data_full (G_OBJECT (task), "device-id", g_strdup (device_id), g_free);
  g_object_set_data_full (G_OBJECT (task), "client", g_object_ref (client), g_object_unref);

  g_async_queue_push (self->queue, task);
}

gboolean
cm_db_delete_client_finish (CmDb          *self,
                            GAsyncResult  *result,
                            GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cm_db_add_session_async (CmDb                *self,
                         const char          *account_id,
                         const char          *device_id,
                         const char          *room_id,
                         const char          *session_id,
                         const char          *sender_key,
                         char                *pickle,
                         CmSessionType        type,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
  GObject *object;
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (account_id && *account_id);
  g_return_if_fail (device_id && *device_id);
  g_return_if_fail (session_id && *session_id);
  g_return_if_fail (sender_key && *sender_key);
  g_return_if_fail (pickle && *pickle);

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_add_session_async);
  g_task_set_task_data (task, cm_db_add_session, NULL);
  object = G_OBJECT (task);

  g_object_set_data_full (object, "account-id", g_strdup (account_id), g_free);
  g_object_set_data_full (object, "device-id", g_strdup (device_id), g_free);
  g_object_set_data_full (object, "room-id", g_strdup (room_id), g_free);
  g_object_set_data_full (object, "session-id", g_strdup (session_id), g_free);
  g_object_set_data_full (object, "sender-key", g_strdup (sender_key), g_free);
  g_object_set_data_full (object, "pickle", pickle, g_free);
  g_object_set_data (object, "type", GINT_TO_POINTER (type));

  g_async_queue_push (self->queue, task);
}

void
cm_db_save_file_enc_async (CmDb                *self,
                           CmEncFileInfo       *file,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (file && file->mxc_uri);

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_save_file_enc_async);
  g_task_set_task_data (task, cm_db_save_file_enc, NULL);
  g_object_set_data (G_OBJECT (task), "file", file);

  g_async_queue_push (self->queue, task);
}

gboolean
cm_db_save_file_enc_finish (CmDb          *self,
                            GAsyncResult  *result,
                            GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);
  g_return_val_if_fail (!error || !*error, FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

void
cm_db_find_file_enc_async (CmDb                *self,
                           const char          *uri,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (uri && *uri);

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_find_file_enc_async);
  g_task_set_task_data (task, cm_db_find_file_enc, NULL);

  g_object_set_data_full (G_OBJECT (task), "uri", g_strdup (uri), g_free);

  g_async_queue_push (self->queue, task);
}

CmEncFileInfo *
cm_db_find_file_enc_finish (CmDb          *self,
                            GAsyncResult  *result,
                            GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

char *
cm_db_lookup_session (CmDb          *self,
                      const char    *account_id,
                      const char    *account_device,
                      const char    *session_id,
                      const char    *sender_key,
                      CmSessionType  type)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  GObject *object;
  char *pickle;

  g_return_val_if_fail (CM_IS_DB (self), NULL);
  g_return_val_if_fail (account_id && *account_id, NULL);
  g_return_val_if_fail (account_device && *account_device, NULL);
  g_return_val_if_fail (session_id && *session_id, NULL);
  g_return_val_if_fail (sender_key && *sender_key, NULL);

  task = g_task_new (self, NULL, NULL, NULL);
  g_object_ref (task);

  g_task_set_source_tag (task, cm_db_lookup_session);
  g_task_set_task_data (task, db_lookup_session, NULL);
  object = G_OBJECT (task);

  g_object_set_data_full (object, "account-id", g_strdup (account_id), g_free);
  g_object_set_data_full (object, "account-device", g_strdup (account_device), g_free);
  g_object_set_data_full (object, "session-id", g_strdup (session_id), g_free);
  g_object_set_data_full (object, "sender-key", g_strdup (sender_key), g_free);
  g_object_set_data (object, "type", GINT_TO_POINTER (type));
  g_async_queue_push_front (self->queue, task);
  g_assert (task);

  /* Wait until the task is completed */
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  pickle = g_task_propagate_pointer (task, &error);

  if (error)
    g_debug ("Error getting session: %s", error->message);

  return pickle;
}

gpointer
cm_db_lookup_olm_session (CmDb           *self,
                          const char     *account_id,
                          const char     *account_device,
                          const char     *sender_curve25519_key,
                          const char     *body,
                          const char     *pickle_key,
                          CmSessionType   type,
                          size_t          message_type,
                          char          **out_plain_text)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  GObject *object;
  char *pickle;

  g_return_val_if_fail (CM_IS_DB (self), NULL);
  g_return_val_if_fail (account_id && *account_id, NULL);
  g_return_val_if_fail (account_device && *account_device, NULL);
  g_return_val_if_fail (sender_curve25519_key && *sender_curve25519_key, NULL);
  g_return_val_if_fail (body && *body, NULL);
  g_return_val_if_fail (pickle_key && *pickle_key, NULL);
  g_return_val_if_fail (out_plain_text, NULL);

  task = g_task_new (self, NULL, NULL, NULL);
  g_object_ref (task);

  g_task_set_source_tag (task, cm_db_lookup_olm_session);
  g_task_set_task_data (task, db_lookup_olm_session, NULL);
  object = G_OBJECT (task);

  g_object_set_data_full (object, "account-id", g_strdup (account_id), g_free);
  g_object_set_data_full (object, "account-device", g_strdup (account_device), g_free);
  g_object_set_data_full (object, "sender-key", g_strdup (sender_curve25519_key), g_free);
  g_object_set_data_full (object, "body", g_strdup (body), g_free);
  g_object_set_data_full (object, "pickle-key", g_strdup (pickle_key),
                          (GDestroyNotify)cm_utils_free_buffer);
  g_object_set_data (object, "type", GINT_TO_POINTER (type));
  g_object_set_data (object, "message-type", GUINT_TO_POINTER (message_type));

  g_async_queue_push_front (self->queue, task);
  g_assert (task);

  /* Wait until the task is completed */
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  pickle = g_task_propagate_pointer (task, &error);
  *out_plain_text = g_object_steal_data (G_OBJECT (task), "plaintext");

  if (error)
    g_debug ("Error getting session: %s", error->message);

  return pickle;
}

void
cm_db_get_olm_sessions_async (CmDb                *self,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_get_olm_sessions_async);
  g_task_set_task_data (task, history_get_olm_sessions, NULL);

  g_async_queue_push (self->queue, task);
}

GPtrArray *
cm_db_get_olm_sessions_finish (CmDb          *self,
                               GAsyncResult  *result,
                               GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
