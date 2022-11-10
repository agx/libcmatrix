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

#define G_LOG_DOMAIN "cm-db"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <glib.h>
#include <fcntl.h>
#include <sqlite3.h>

#include "events/cm-event-private.h"
#include "events/cm-room-event-private.h"
#include "events/cm-room-message-event-private.h"
#include "users/cm-user-private.h"
#include "cm-device-private.h"
#include "cm-enc-private.h"
#include "cm-olm-private.h"
#include "cm-client-private.h"
#include "cm-room-private.h"
#include "cm-utils-private.h"
#include "cm-db-private.h"

#define STRING(arg) STRING_VALUE(arg)
#define STRING_VALUE(arg) #arg

/* increment when DB changes */
#define DB_VERSION 2

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
#define VERIFICATION_IS_SELF     5

#define EVENT_NOT_ENCRYPTED       0
#define EVENT_NOT_DECRYPTED       1
#define EVENT_DECRYPTED           2
/* We got m.room.encrypted, but with empty content */
#define EVENT_MAY_BE_DECRYPTED    3

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
                 gint64        bind_value,
                 const char   *message)
{
  guint status;

  status = sqlite3_bind_int64 (statement, position, bind_value);
  warn_if_sql_error (status, message);
}

static int
db_event_state_to_int (CmEventState state)
{
  switch (state)
    {
    case CM_EVENT_STATE_DRAFT:
      return 1;

    case CM_EVENT_STATE_WAITING:
      return 2;

    case CM_EVENT_STATE_SENDING:
      return 3;

    case CM_EVENT_STATE_SENDING_FAILED:
      return 4;

    case CM_EVENT_STATE_SENT:
      return 5;

    case CM_EVENT_STATE_RECEIVED:
      return 6;

    case CM_EVENT_STATE_UNKNOWN:
    default:
      return 0;
    }

  return 0;
}

static CmEventState
db_event_state_from_int (int state)
{
  if (state == 0)
    return CM_EVENT_STATE_UNKNOWN;

  if (state == 1)
    return CM_EVENT_STATE_DRAFT;

  if (state == 2 || state == 3 || state == 4)
    return CM_EVENT_STATE_SENDING_FAILED;

  if (state == 5)
    return CM_EVENT_STATE_SENT;

  if (state == 6)
    return CM_EVENT_STATE_RECEIVED;

  g_return_val_if_reached (CM_EVENT_STATE_UNKNOWN);
}

static int
db_event_get_decryption_value (CmEvent *event)
{
  gboolean encrypted = 0, decrypted = 0, has_content = 0;

  if (!event)
    return EVENT_NOT_ENCRYPTED;

  g_assert (CM_IS_EVENT (event));

  encrypted = cm_event_is_encrypted (event);
  decrypted = cm_event_is_decrypted (event);
  has_content = cm_event_has_encrypted_content (event);

  if (encrypted)
    {
      if (has_content && decrypted)
        return EVENT_DECRYPTED;

      if (!has_content)
        return EVENT_MAY_BE_DECRYPTED;

      return EVENT_NOT_DECRYPTED;
    }

  return EVENT_NOT_ENCRYPTED;
}

static GPtrArray *
db_get_past_room_events (CmDb   *self,
                         CmRoom *cm_room,
                         int     room_id,
                         int     from_event_id,
                         int     from_sorted_event_id,
                         int     max_count)
{
  GPtrArray *events = NULL;
  sqlite3_stmt *stmt;
  gboolean skip = FALSE;

  if (from_event_id)
    skip = TRUE;

  sqlite3_prepare_v2 (self->db,
                      "SELECT id,event_state,room_events.json_data FROM room_events "
                      "WHERE room_id=? AND sorted_id <= ? "
                      /* Limit to messages until chatty has better events support */
                      "AND (event_type=? OR event_type=?)"
                      "ORDER BY sorted_id DESC, id DESC LIMIT ?",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, room_id, "binding when loading past events");
  matrix_bind_int (stmt, 2, from_sorted_event_id, "binding when loading past events");
  matrix_bind_int (stmt, 3, CM_M_ROOM_MESSAGE, "binding when loading past events");
  matrix_bind_int (stmt, 4, CM_M_ROOM_ENCRYPTED, "binding when loading past events");
  matrix_bind_int (stmt, 5, max_count, "binding when loading past events");

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      JsonObject *encrypted, *root;
      g_autoptr(JsonObject) json = NULL;
      CmRoomEvent *cm_event;
      CmEventState state;

      if (skip)
        {
          skip = FALSE;
          continue;
        }

      json = cm_utils_string_to_json_object ((char *)sqlite3_column_text (stmt, 2));

      if (!json)
        continue;

      root = cm_utils_json_object_get_object (json, "json");
      encrypted = cm_utils_json_object_get_object (json, "encrypted");
      cm_event = cm_room_event_new_from_json (cm_room, root, encrypted);
      state = db_event_state_from_int (sqlite3_column_int (stmt, 1));
      cm_event_set_state (CM_EVENT (cm_event), state);

      if (!cm_event)
        continue;

      if (!events)
        events = g_ptr_array_new_full (32, g_object_unref);

      g_ptr_array_add (events, cm_event);
    }

  sqlite3_finalize (stmt);

  return events;
}

/**
   user_devices.json_data
   - local
   -  device_display_name
   users.json_data
   - local
   -  name
   -  avatar_url
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
   -  draft = json object
   -    m.text = string
   -  encryption = string
   -  rotation_period_ms = time_t (or may be use μs as provided by server?)
   -  rotation_count_msgs = int (message count max for key change)
   sessions.json_data
   - local
   -  key_added_date = time_t (or may be use μs as provided by server?)
   room_members.json_data
   - local
   -  display_name TEXT (set if different from default name)
   -  avatar_url TEXT (set id different from default avatar)
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

  sql = "PRAGMA user_version = " STRING (DB_VERSION) ";"

    "CREATE TABLE IF NOT EXISTS users ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    /* v2 */
    "account_id INTEGER REFERENCES accounts(id) ON DELETE CASCADE, "
    /* Version 1: Unique */
    /* v2: Remove Unique */
    "username TEXT NOT NULL, "
    /* v2 */
    "tracking INTEGER NOT NULL DEFAULT 0, "
    /* Version 1 */
    "outdated INTEGER DEFAULT 1, "
    /* Version 1 */
    "json_data TEXT,"
    /* v2 */
    "UNIQUE (account_id, username));"

    /* Version 1 */
    "CREATE TABLE IF NOT EXISTS user_devices ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "user_id INTEGER NOT NULL REFERENCES users(id), "
    /* xxx: Move device to devices table along with
     * curve25519_key and ed25519_key as the same
     * can exist in several accounts
     */
    "device TEXT NOT NULL, "
    "curve25519_key TEXT, "
    "ed25519_key TEXT, "
    "verification INTEGER DEFAULT 0, "
    "json_data TEXT, "
    "UNIQUE (user_id, device));"

    "CREATE TABLE IF NOT EXISTS accounts ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    /* Version 1 */
    "user_device_id INTEGER NOT NULL REFERENCES user_devices(id), "
    "next_batch TEXT, "
    "pickle TEXT, "
    "enabled INTEGER DEFAULT 0, "
    /* Version 1 */
    "json_data TEXT, "
    "UNIQUE (user_device_id));"

    "CREATE TABLE IF NOT EXISTS rooms ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    /* v2: on delete cascade */
    "account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE, "
    "room_name TEXT NOT NULL, "
    "prev_batch TEXT, "
    /* Version 1 */
    /* Set if the room has tombstone and got replaced by a different room */
    "replacement_room_id INTEGER REFERENCES rooms(id),"
    /* v2 */
    "room_state INTEGER NOT NULL DEFAULT 0, "
    /* Version 1 */
    "json_data TEXT, "
    "UNIQUE (account_id, room_name));"

    /* v2 */
    "CREATE TABLE IF NOT EXISTS room_members ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE, "
    "user_id INTEGER NOT NULL REFERENCES users(id), "
    /* joined, invited, left (we set left instead of deleting as past messages may refer to user id) */
    "user_state INTEGER NOT NULL DEFAULT 0, "
    "json_data TEXT, "
    "UNIQUE (room_id, user_id));"

    /* v2 */
    "CREATE TABLE IF NOT EXISTS room_events_cache ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE, "
    "sender_id INTEGER REFERENCES room_members(id), "
    "event_uid TEXT NOT NULL, "
    "origin_server_ts INTEGER, "
    "json_data TEXT, "
    "UNIQUE (room_id, event_uid));"

    /* v2 */
    "CREATE TABLE IF NOT EXISTS room_events ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    /* 'id' above only increments, 'sorted_id' increments
     * or decrements in the order events has to be placed.
     */
    "sorted_id INTEGER NOT NULL, "
    "room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE, "
    "sender_id INTEGER NOT NULL REFERENCES room_members(id), "
    "event_type INTEGER NOT NULL, "
    "event_uid TEXT, "
    "txnid TEXT, "
    /* If set to 0, this event is a reply to some other, which is not yet in db */
    "replaces_event_id INTEGER REFERENCES room_events(id), "
    "replaces_event_cache_id INTEGER REFERENCES room_events_cache(id), "
    /* This event has been replaced with the given event id */
    /* This will likely point to the latest replacement id available */
    "replaced_with_id INTEGER REFERENCES room_events(id), "
    /* sending, sent, sending failed, */
    "event_state INTEGER, "
    "state_key TEXT, "
    "origin_server_ts INTEGER NOT NULL, "
    /* 0: not encrypted, 1: not decrypted 2: decrypted */
    /* 3: may be decrypted, we got m.room.encrypted but without content */
    "decryption INTEGER NOT NULL DEFAULT 0, "
    /* direction int, encrypted int, verified int, txnid */
    "json_data TEXT, "
    "UNIQUE (room_id, event_uid));"

    "CREATE TABLE IF NOT EXISTS encryption_keys ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    /* v2 */
    "account_id INTEGER REFERENCES accounts(id) ON DELETE CASCADE, "
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
    /* Version 1 */
    "json_data TEXT, "
    "UNIQUE (account_id, file_url));"

    /* v2: Renamed to sessions from session */
    "CREATE TABLE IF NOT EXISTS sessions ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE, "
    "sender_key TEXT NOT NULL, "
    "session_id TEXT NOT NULL, "
    "type INTEGER NOT NULL, "
    "pickle TEXT NOT NULL, "
    "time INT, "
    /* v2 */
    "origin_server_ts INTEGER, "
    /* Version 1 */
    "room_id INTEGER REFERENCES rooms(id), "
    /* v2 */
    "chain_index INTEGER, "
    /* v2 */
    /* 0: usable, 1: rotated, 2: invalidated (on user/device removals) */
    "session_state INTEGER NOT NULL DEFAULT 0, "
    /* v1 */
    "json_data TEXT, "
    "UNIQUE (account_id, sender_key, session_id));"

    /* v2 */
    "CREATE UNIQUE INDEX IF NOT EXISTS room_event_idx ON room_events (room_id, event_uid);"
    "CREATE UNIQUE INDEX IF NOT EXISTS room_event_txn_idx ON room_events (room_id, txnid);"
    "CREATE UNIQUE INDEX IF NOT EXISTS user_device_idx ON user_devices (user_id, device);"
    "CREATE INDEX IF NOT EXISTS room_event_state_idx ON room_events (state_key);"
    "CREATE UNIQUE INDEX IF NOT EXISTS room_event_cache_idx ON room_events_cache (room_id, event_uid);"
    "CREATE UNIQUE INDEX IF NOT EXISTS encryption_key_idx ON encryption_keys (account_id, file_url);"
    "CREATE INDEX IF NOT EXISTS session_sender_idx ON sessions (account_id, sender_key);"
    "CREATE INDEX IF NOT EXISTS user_idx ON users (username);"

    /* v2 */
    "CREATE TRIGGER IF NOT EXISTS insert_replaced_with_id AFTER INSERT "
    "ON room_events FOR EACH ROW WHEN NEW.replaces_event_id IS NOT NULL "
    "BEGIN "
    "UPDATE room_events SET replaced_with_id=NEW.id "
    "WHERE id=NEW.replaces_event_id AND (replaced_with_id IS NULL or replaced_with_id < NEW.id); "
    "END;"

    /* v2 */
    "CREATE TRIGGER IF NOT EXISTS update_replaced_with_id AFTER UPDATE OF replaces_event_id "
    "ON room_events FOR EACH ROW WHEN NEW.replaces_event_id IS NOT NULL "
    "BEGIN "
    "UPDATE room_events SET replaced_with_id=NEW.id "
    "WHERE id=NEW.replaces_event_id AND (replaced_with_id IS NULL or replaced_with_id < NEW.id); "
    "END;";

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
cm_db_migrate_db_v1 (CmDb  *self,
                     GTask *task)
{
  char *error = NULL;
  int status;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);

  cm_db_backup (self);

  status = sqlite3_exec (self->db,
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

                         "INSERT OR IGNORE INTO tmp_users(username) "
                         "SELECT DISTINCT username FROM users;"

                         "INSERT OR IGNORE INTO user_devices(user_id,device) "
                         "SELECT tmp_users.id,devices.device FROM tmp_users "
                         "JOIN users ON users.username=tmp_users.username "
                         "JOIN devices ON users.device_id=devices.id;"

                         "INSERT OR IGNORE INTO tmp_accounts(user_device_id,next_batch,pickle,enabled) "
                         "SELECT user_devices.id,next_batch,pickle,enabled FROM accounts "
                         "JOIN users ON users.id=accounts.user_id "
                         "JOIN devices ON users.device_id=devices.id "
                         "JOIN user_devices ON user_devices.device=devices.device "
                         "JOIN tmp_users ON user_devices.user_id=tmp_users.id "
                         "AND tmp_users.username=users.username;"

                         "UPDATE OR IGNORE session SET account_id=(SELECT tmp_accounts.id "
                         "FROM tmp_accounts "
                         "INNER JOIN accounts ON accounts.pickle=tmp_accounts.pickle "
                         "AND session.account_id=accounts.id"
                         ");"

                         "UPDATE OR IGNORE rooms SET account_id=(SELECT tmp_accounts.id "
                         "FROM tmp_accounts "
                         "INNER JOIN accounts ON accounts.pickle=tmp_accounts.pickle "
                         "AND rooms.account_id=accounts.id"
                         ");"

                         "DROP TABLE IF EXISTS users;"
                         "DROP TABLE IF EXISTS accounts;"
                         "DROP TABLE IF EXISTS devices;"

                         "ALTER TABLE tmp_users RENAME TO users;"
                         "ALTER TABLE tmp_accounts RENAME TO accounts;"

                         "ALTER TABLE rooms ADD COLUMN replacement_room_id "
                         "INTEGER REFERENCES rooms(id);"
                         "ALTER TABLE rooms ADD COLUMN json_data TEXT;"

                         "ALTER TABLE encryption_keys ADD COLUMN json_data TEXT;"

                         "ALTER TABLE session ADD COLUMN room_id "
                         "INTEGER REFERENCES rooms(id);"
                         "ALTER TABLE session ADD COLUMN json_data TEXT;"

                         "PRAGMA user_version = 1;",
                         NULL, NULL, &error);

  g_debug ("Migrating db to version 1, success: %d", !error);

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
cm_db_migrate_to_v2 (CmDb  *self,
                     GTask *task)
{
  char *error = NULL;
  int status;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);

  cm_db_backup (self);

  status = sqlite3_exec (self->db,
                         "CREATE TABLE IF NOT EXISTS room_members ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         "room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE, "
                         "user_id INTEGER NOT NULL REFERENCES users(id), "
                         /* joined, invited, left (we set left instead of deleting as past messages may refer to user id) */
                         "user_state INTEGER NOT NULL DEFAULT 0, "
                         "json_data TEXT, "
                         "UNIQUE (room_id, user_id));"

                         /* v2 */
                         "CREATE TABLE IF NOT EXISTS room_events_cache ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         "room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE, "
                         "sender_id INTEGER REFERENCES room_members(id), "
                         "event_uid TEXT NOT NULL, "
                         "origin_server_ts INTEGER, "
                         "json_data TEXT, "
                         "UNIQUE (room_id, event_uid));"

                         "CREATE TABLE IF NOT EXISTS room_events ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         /* 'id' above only increments, 'sorted_id' increments
                          * or decrements in the order events has to be placed.
                          */
                         "sorted_id INTEGER NOT NULL, "
                         "room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE, "
                         "sender_id INTEGER NOT NULL REFERENCES room_members(id), "
                         "event_type INTEGER NOT NULL, "
                         "event_uid TEXT, "
                         "txnid TEXT, "
                         /* If set to 0, this event is a reply to some other, which is not yet in db */
                         "replaces_event_id INTEGER REFERENCES room_events(id), "
                         "replaces_event_cache_id INTEGER REFERENCES room_events_cache(id), "
                         /* This event has been replaced with the given event id */
                         /* This will likely point to the latest replacement id available */
                         "replaced_with_id INTEGER REFERENCES room_events(id), "
                         /* sending, sent, sending failed, */
                         "event_state INTEGER, "
                         "state_key TEXT, "
                         "origin_server_ts INTEGER NOT NULL, "
                         /* 0: not encrypted, 1: not decrypted 2: decrypted */
                         /* 3: may be decrypted, we got m.room.encrypted but without content */
                         "decryption INTEGER NOT NULL DEFAULT 0, "
                         /* direction int, encrypted int, verified int, txnid */
                         "json_data TEXT, "
                         "UNIQUE (room_id, event_uid));"

                         "CREATE TABLE IF NOT EXISTS tmp_encryption_keys ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         /* v2 */
                         "account_id INTEGER REFERENCES accounts(id) ON DELETE CASCADE, "
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
                         /* Version 1 */
                         "json_data TEXT, "
                         "UNIQUE (account_id, file_url));"

                         "INSERT INTO tmp_encryption_keys(file_url,file_sha256,iv,version,algorithm,key,type,extractable) "
                         "SELECT DISTINCT file_url,file_sha256,iv,version,algorithm,key,type,extractable FROM encryption_keys;"

                         "CREATE TABLE IF NOT EXISTS tmp_users ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         "account_id INTEGER REFERENCES accounts(id) ON DELETE CASCADE, "
                         "username TEXT NOT NULL, "
                         "tracking INTEGER NOT NULL DEFAULT 0, "
                         "outdated INTEGER DEFAULT 1, "
                         "json_data TEXT, "
                         "UNIQUE (account_id, username));"

                         "INSERT INTO tmp_users(id,username) "
                         "SELECT DISTINCT id,username FROM users;"

                         "CREATE TABLE IF NOT EXISTS tmp_rooms ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         /* v2: on delete cascade */
                         "account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE, "
                         "room_name TEXT NOT NULL, "
                         "prev_batch TEXT, "
                         /* v1 */
                         /* Set if the room has tombstone and got replaced by a different room */
                         "replacement_room_id INTEGER REFERENCES rooms(id),"
                         /* v2 */
                         "room_state INTEGER NOT NULL DEFAULT 0, "
                         /* v1 */
                         "json_data TEXT, "
                         "UNIQUE (account_id, room_name));"

                         "INSERT OR IGNORE INTO tmp_rooms(id,account_id,room_name,prev_batch) "
                         "SELECT DISTINCT id,account_id,room_name,prev_batch FROM rooms;"

                         "CREATE TABLE IF NOT EXISTS tmp_sessions ("
                         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
                         "account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE, "
                         "sender_key TEXT NOT NULL, "
                         "session_id TEXT NOT NULL, "
                         "type INTEGER NOT NULL, "
                         "pickle TEXT NOT NULL, "
                         "time INT, "
                         /* v2 */
                         "origin_server_ts INTEGER, "
                         /* Version 1 */
                         "room_id INTEGER REFERENCES rooms(id), "
                         /* v2 */
                         "chain_index INTEGER, "
                         /* v2 */
                         /* 0: usable, 1: rotated, 2: invalidated (on user/device removals) */
                         "session_state INTEGER NOT NULL DEFAULT 0, "
                         /* v1 */
                         "json_data TEXT, "
                         "UNIQUE (account_id, sender_key, session_id));"

                         "INSERT OR IGNORE INTO tmp_sessions(id,account_id,sender_key,session_id,type,pickle,time) "
                         "SELECT DISTINCT id,account_id,sender_key,session_id,type,pickle,time FROM session;"

                         "DROP TABLE IF EXISTS rooms;"
                         "DROP TABLE IF EXISTS users;"
                         "DROP TABLE IF EXISTS session;"
                         "DROP TABLE IF EXISTS encryption_keys;"

                         "ALTER TABLE tmp_rooms RENAME TO rooms;"
                         "ALTER TABLE tmp_users RENAME TO users;"
                         "ALTER TABLE tmp_sessions RENAME TO sessions;"
                         "ALTER TABLE tmp_encryption_keys RENAME TO encryption_keys;"

                         "CREATE UNIQUE INDEX IF NOT EXISTS room_event_idx ON room_events (room_id, event_uid);"
                         "CREATE UNIQUE INDEX IF NOT EXISTS room_event_txn_idx ON room_events (room_id, txnid);"
                         "CREATE UNIQUE INDEX IF NOT EXISTS user_device_idx ON user_devices (user_id, device);"
                         "CREATE INDEX IF NOT EXISTS room_event_state_idx ON room_events (state_key);"
                         "CREATE UNIQUE INDEX IF NOT EXISTS room_event_cache_idx ON room_events_cache (room_id, event_uid);"
                         "CREATE UNIQUE INDEX IF NOT EXISTS encryption_key_idx ON encryption_keys (account_id, file_url);"
                         "CREATE INDEX IF NOT EXISTS session_sender_idx ON sessions (account_id, sender_key);"
                         "CREATE INDEX IF NOT EXISTS user_idx ON users (username);"

                         /* v2 */
                         "CREATE TRIGGER IF NOT EXISTS insert_replaced_with_id AFTER INSERT "
                         "ON room_events FOR EACH ROW WHEN NEW.replaces_event_id IS NOT NULL "
                         "BEGIN "
                         "UPDATE room_events SET replaced_with_id=NEW.id "
                         "WHERE id=NEW.replaces_event_id AND (replaced_with_id IS NULL or replaced_with_id < NEW.id); "
                         "END;"

                         /* v2 */
                         "CREATE TRIGGER IF NOT EXISTS update_replaced_with_id AFTER UPDATE OF replaces_event_id "
                         "ON room_events FOR EACH ROW WHEN NEW.replaces_event_id IS NOT NULL "
                         "BEGIN "
                         "UPDATE room_events SET replaced_with_id=NEW.id "
                         "WHERE id=NEW.replaces_event_id AND (replaced_with_id IS NULL or replaced_with_id < NEW.id); "
                         "END;"

                         "PRAGMA user_version = 2;",
                         NULL, NULL, &error);

  g_debug ("Migrating db to version 2, success: %d", !error);

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
    if (!cm_db_migrate_db_v1 (self, task))
      return FALSE;
    /* fallthrough */

  case 1:
    if (!cm_db_migrate_to_v2 (self, task))
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
db_get_room_event_id (CmDb       *self,
                      int         room_id,
                      int        *out_sorted_id,
                      const char *event)
{
  sqlite3_stmt *stmt;
  int event_id = 0;

  if (!room_id || !event || !*event)
    return 0;

  g_assert (CM_IS_DB (self));

  sqlite3_prepare_v2 (self->db,
                      "SELECT id,sorted_id FROM room_events WHERE room_id=? "
                      "AND event_uid=?",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, room_id, "binding when selecting event");
  matrix_bind_text (stmt, 2, event, "binding when selecting event");
  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      event_id = sqlite3_column_int (stmt, 0);

      if (out_sorted_id)
        *out_sorted_id = sqlite3_column_int (stmt, 1);
    }

  sqlite3_finalize (stmt);

  return event_id;
}

static int
db_get_room_cache_event_id (CmDb       *self,
                            int         room_id,
                            const char *event,
                            gboolean    insert_if_missing)
{
  sqlite3_stmt *stmt;
  int event_cache_id = 0;

  if (!room_id || !event || !*event)
    return 0;

  g_assert (CM_IS_DB (self));

  sqlite3_prepare_v2 (self->db,
                      "SELECT id FROM room_events_cache "
                      "WHERE room_id=? AND event_uid=?",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, room_id, "binding when selecting cache event");
  matrix_bind_text (stmt, 2, event, "binding when selecting cache event");

  if (sqlite3_step (stmt) == SQLITE_ROW)
    event_cache_id = sqlite3_column_int (stmt, 0);
  sqlite3_finalize (stmt);

  if (event_cache_id || !insert_if_missing)
    return event_cache_id;

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO room_events_cache (room_id,event_uid) VALUES(?1,?2)",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, room_id, "binding when adding cache event");
  matrix_bind_text (stmt, 2, event, "binding when adding cache event");
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  event_cache_id = sqlite3_last_insert_rowid (self->db);

  return event_cache_id;
}

static int
db_get_first_room_event_id (CmDb *self,
                            int   room_id,
                            int  *out_sorted_id)
{
  sqlite3_stmt *stmt;
  int event_id = 0;

  g_assert (CM_IS_DB (self));

  if (!room_id)
    return 0;

  sqlite3_prepare_v2 (self->db,
                      "SELECT id,sorted_id FROM room_events WHERE room_id=? "
                      "ORDER BY sorted_id ASC LIMIT 1",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, room_id, "binding when selecting event");

  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      event_id = sqlite3_column_int (stmt, 0);
      if (out_sorted_id)
        *out_sorted_id = sqlite3_column_int (stmt, 1);
    }

  sqlite3_finalize (stmt);

  return event_id;
}

/* xxx: Merge with above method with a proper name */
static int
db_get_last_room_event_id (CmDb *self,
                           int   room_id,
                           int  *out_sorted_id)
{
  sqlite3_stmt *stmt;
  int event_id = 0;

  g_assert (CM_IS_DB (self));

  if (!room_id)
    return 0;

  sqlite3_prepare_v2 (self->db,
                      "SELECT id,sorted_id FROM room_events WHERE room_id=? "
                      "ORDER BY sorted_id DESC LIMIT 1",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, room_id, "binding when selecting event");

  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      event_id = sqlite3_column_int (stmt, 0);
      if (out_sorted_id)
        *out_sorted_id = sqlite3_column_int (stmt, 1);
    }

  sqlite3_finalize (stmt);

  return event_id;
}

static int
matrix_db_get_user_id (CmDb       *self,
                       int         account_id,
                       const char *username,
                       gboolean    insert_if_missing)
{
  const char *query;
  sqlite3_stmt *stmt;
  int user_id = 0;

  if (!username || !*username)
    return 0;

  g_assert (CM_IS_DB (self));

  if (account_id)
    query = "SELECT id FROM users WHERE username=? AND account_id=?";
  else
    query = "SELECT id FROM users WHERE username=? AND account_id IS NULL";

  sqlite3_prepare_v2 (self->db, query, -1, &stmt, NULL);
  matrix_bind_text (stmt, 1, username, "binding when selecting user");
  if (account_id)
    matrix_bind_int (stmt, 2, account_id, "binding when selecting user");

  if (sqlite3_step (stmt) == SQLITE_ROW)
    user_id = sqlite3_column_int (stmt, 0);
  sqlite3_finalize (stmt);

  if (user_id || !insert_if_missing)
    return user_id;

  if (account_id)
    query = "INSERT INTO users(username,account_id) VALUES(?1,?2)";
  else
    query = "INSERT INTO users(username) VALUES(?1)";

  sqlite3_prepare_v2 (self->db, query, -1, &stmt, NULL);
  matrix_bind_text (stmt, 1, username, "binding when adding user");
  if (account_id)
    matrix_bind_int (stmt, 2, account_id, "binding when adding user");

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
                              gboolean    insert_if_missing,
                              gboolean    is_self)
{
  sqlite3_stmt *stmt;
  int user_id = 0, user_device_id = 0;

  if (!username || !*username || !device || !*device)
    return 0;

  g_assert (CM_IS_DB (self));

  user_id = matrix_db_get_user_id (self, 0, username, insert_if_missing);

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
                      "INSERT INTO user_devices(user_id, device, verification) "
                      "VALUES(?1, ?2, ?3)",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, user_id, "binding when adding user device");
  matrix_bind_text (stmt, 2, device, "binding when adding user device");
  if (is_self)
    matrix_bind_int (stmt, 3, VERIFICATION_IS_SELF, "binding when adding user device");

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

  user_device_id = matrix_db_get_user_device_id (self, username, device, NULL, insert_if_missing, TRUE);

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

static int
matrix_db_get_room_id (CmDb       *self,
                       int         account_id,
                       const char *room,
                       gboolean    insert_if_missing)
{
  sqlite3_stmt *stmt;
  int room_id = 0;

  if (!room || !*room || !account_id)
    return 0;

  sqlite3_prepare_v2 (self->db,
                      "SELECT rooms.id FROM rooms "
                      "WHERE account_id=? and room_name=?",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, account_id, "binding when getting room id");
  matrix_bind_text (stmt, 2, room, "binding when getting room id");

  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      room_id = sqlite3_column_int (stmt, 0);
      sqlite3_finalize (stmt);

      return room_id;
    }

  if (!insert_if_missing)
    return 0;

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO rooms(account_id,room_name) "
                      "VALUES(?1,?2)",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, account_id, "binding when getting room id");
  matrix_bind_text (stmt, 2, room, "binding when getting room id");

  sqlite3_step (stmt);
  room_id = sqlite3_last_insert_rowid (self->db);

  sqlite3_finalize (stmt);

  return room_id;
}

static int
db_get_room_member_id (CmDb       *self,
                       int         account_id,
                       int         room_id,
                       const char *member,
                       int        *out_user_id,
                       gboolean    insert_if_missing)
{
  sqlite3_stmt *stmt;
  int member_id = 0, user_id = 0;

  if (!member || !*member || !room_id)
    return 0;

  sqlite3_prepare_v2 (self->db,
                      "SELECT user_id,room_members.id FROM room_members "
                      "INNER JOIN users ON users.username=? AND users.account_id=? "
                      "WHERE room_id=?",
                      -1, &stmt, NULL);
  matrix_bind_text (stmt, 1, member, "binding when getting room member id");
  matrix_bind_int (stmt, 2, account_id, "binding when getting room member id");
  matrix_bind_int (stmt, 3, room_id, "binding when getting room member id");

  if (sqlite3_step (stmt) == SQLITE_ROW)
    {
      if (out_user_id)
        *out_user_id = sqlite3_column_int (stmt, 0);

      member_id = sqlite3_column_int (stmt, 1);
      sqlite3_finalize (stmt);

      return member_id;
    }

  if (!insert_if_missing)
    return 0;

  user_id = matrix_db_get_user_id (self, account_id, member, insert_if_missing);
  if (!user_id)
    return 0;

  if (out_user_id)
    *out_user_id = user_id;

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO room_members(room_id,user_id) "
                      "VALUES(?1,?2)",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, room_id, "binding when getting room member id");
  matrix_bind_int (stmt, 2, user_id, "binding when getting room member id");

  sqlite3_step (stmt);
  member_id = sqlite3_last_insert_rowid (self->db);

  sqlite3_finalize (stmt);

  return member_id;
}

static void
cm_db_update_user (CmDb   *self,
                   int     user_id,
                   CmUser *user)
{
  g_autofree char *json_str = NULL;
  sqlite3_stmt *stmt;
  JsonObject *json;

  g_assert (CM_IS_DB (self));
  g_assert (CM_IS_USER (user));

  if (!user_id)
    return;

  json = cm_user_generate_json (user);
  if (!json)
    return;

  json_str = cm_utils_json_object_to_string (json, FALSE);

  sqlite3_prepare_v2 (self->db,
                      "UPDATE users SET json_data=?1 "
                      "WHERE id=?2",
                      -1, &stmt, NULL);

  matrix_bind_text (stmt, 1, json_str, "binding when updating user");
  matrix_bind_int (stmt, 2, user_id, "binding when updating user");

  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
}

static void
cm_db_update_user_device (CmDb     *self,
                          int       user_id,
                          CmDevice *device)
{
  const char *curve25519_key, *ed25519_key;
  g_autofree char *json_str = NULL;
  sqlite3_stmt *stmt;
  JsonObject *json;

  g_assert (CM_IS_DB (self));
  g_assert (CM_IS_DEVICE (device));

  if (!user_id)
    return;

  curve25519_key = cm_device_get_curve_key (device);
  ed25519_key = cm_device_get_ed_key (device);
  json = cm_device_get_json (device);
  if (json)
    json_str = cm_utils_json_object_to_string (json, FALSE);

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO user_devices(user_id,device,"
                      "curve25519_key,ed25519_key,json_data)"
                      "VALUES(?1,?2,?3,?4,?5) "
                      "ON CONFLICT(user_id,device) DO UPDATE SET "
                      "curve25519_key=?3, ed25519_key=?4, json_data=?5",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, user_id, "binding when updating user device");
  matrix_bind_text (stmt, 2, cm_device_get_id (device), "binding when updating user device");
  if (curve25519_key && *curve25519_key)
    matrix_bind_text (stmt, 3, curve25519_key, "binding when updating user device");
  if (ed25519_key && *ed25519_key)
    matrix_bind_text (stmt, 4, ed25519_key, "binding when updating user device");
  matrix_bind_text (stmt, 5, json_str, "binding when updating user device");

  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
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

    sqlite3_exec (self->db, "PRAGMA foreign_keys = OFF;", NULL, NULL, NULL);
    sqlite3_exec (self->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (db_exists) {
      if (!cm_db_migrate (self, task))
        {
          sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
          return;
        }
    } else {
      if (!cm_db_create_schema (self, task))
        {
          sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
          return;
        }
    }

    sqlite3_exec (self->db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
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

  sqlite3_exec (self->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  account_id = matrix_db_get_account_id (self, username, device, &user_device_id, TRUE);

  if (!account_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Failed to add account to db");
      return;
    }

  if (filter && *filter)
    {
      root = json_object_new ();
      obj = json_object_new ();
      json_object_set_object_member (root, "local", obj);

      if (filter && *filter)
        json_object_set_string_member (obj, "filter-id", filter);

      json_str = cm_utils_json_object_to_string (root, FALSE);
    }

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
  sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);

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
cm_db_get_rooms (CmDb       *self,
                 int         account_id,
                 const char *account_next_batch)
{
  g_autoptr(GPtrArray) rooms = NULL;
  sqlite3_stmt *stmt;

  g_assert (CM_IS_DB (self));
  g_assert (account_id);

  sqlite3_prepare_v2 (self->db,
                      "SELECT id,room_name,prev_batch,json_data,room_state FROM rooms "
                      "WHERE account_id=? AND replacement_room_id IS NULL "
                      "AND room_state != ?",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, account_id, "binding when getting rooms");
  matrix_bind_int (stmt, 2, CM_STATUS_LEAVE, "binding when getting rooms");

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      g_autoptr(GPtrArray) events = NULL;
      char *room_name, *prev_batch, *json_str;
      JsonObject *json = NULL;
      CmRoom *room;
      CmStatus room_status;
      int room_id, event_id, sorted_event_id = 0;

      if (!rooms)
        rooms = g_ptr_array_new_full (32, g_object_unref);

      room_id = sqlite3_column_int (stmt, 0);
      room_name = (char *)sqlite3_column_text (stmt, 1);
      prev_batch = (char *)sqlite3_column_text (stmt, 2);
      json_str = (char *)sqlite3_column_text (stmt, 3);
      json = cm_utils_string_to_json_object (json_str);
      room_status = sqlite3_column_int (stmt, 4);

      if (room_status == CM_STATUS_UNKNOWN)
        continue;

      room = cm_room_new_from_json (room_name, json, NULL);
      g_object_set_data (G_OBJECT (room), "-cm-room-id", GINT_TO_POINTER (room_id));
      cm_room_set_prev_batch (room, prev_batch);
      cm_room_set_status (room, room_status);

      event_id = db_get_last_room_event_id (self, room_id, &sorted_event_id);
      if (event_id)
        events = db_get_past_room_events (self, room, room_id, 0, sorted_event_id, 1);
      else
        cm_room_set_prev_batch (room, account_next_batch);

      if (events)
        cm_room_add_events (room, events, TRUE);

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

      /* If we don't have json_data the db was just migrated from older version */
      if (sqlite3_column_text (stmt, 2) == NULL)
        g_object_set_data (object, "db-migrated", GINT_TO_POINTER (TRUE));

      if (filter && *filter)
        g_object_set_data_full (object, "filter-id", g_strdup (filter), g_free);

      rooms = cm_db_get_rooms (self, account_id, (char *)sqlite3_column_text (stmt, 1));
      g_object_set_data_full (object, "rooms", rooms, (GDestroyNotify)g_ptr_array_unref);
    }

  sqlite3_finalize (stmt);
  g_task_return_boolean (task, status == SQLITE_ROW);
}

static void
cm_db_save_room (CmDb  *self,
                 GTask *task)
{
  CmRoom *room;
  const char *username, *client_device, *prev_batch;
  const char *replacement, *json = NULL;
  sqlite3_stmt *stmt;
  int account_id, room_id = 0, replacement_id = 0;
  int room_status;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  username = g_object_get_data (G_OBJECT (task), "username");
  client_device = g_object_get_data (G_OBJECT (task), "client-device");
  room = g_object_get_data (G_OBJECT (task), "room");
  json = g_object_get_data (G_OBJECT (task), "json");
  prev_batch = g_object_get_data (G_OBJECT (task), "prev-batch");
  replacement = g_object_get_data (G_OBJECT (task), "replacement");
  room_status = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "status"));

  sqlite3_exec (self->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  account_id = matrix_db_get_account_id (self, username, client_device, NULL, FALSE);

  if (!account_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  if (replacement)
    replacement_id = matrix_db_get_room_id (self, account_id,
                                            replacement, TRUE);
  room_id = matrix_db_get_room_id (self, account_id, cm_room_get_id (room), TRUE);

  if (!room_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting room id");
      return;
    }

  if (!cm_room_has_state_sync (room))
    json = NULL;

  sqlite3_prepare_v2 (self->db,
                      "UPDATE rooms SET prev_batch=?1,json_data=?2, "
                      "replacement_room_id=iif(?3 = 0, null, ?3),room_state=?4 "
                      "WHERE id=?5",
                      -1, &stmt, NULL);

  matrix_bind_text (stmt, 1, prev_batch, "binding when saving room");
  matrix_bind_text (stmt, 2, json, "binding when saving room");
  matrix_bind_int (stmt, 3, replacement_id, "binding when saving room");
  matrix_bind_int (stmt, 4, room_status, "binding when saving room");
  matrix_bind_int (stmt, 5, room_id, "binding when saving room");

  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);

  g_task_return_boolean (task, TRUE);
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

  sqlite3_exec (self->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  account_id = matrix_db_get_account_id (self, username, device_id, NULL, FALSE);

  if (!account_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  status = sqlite3_prepare_v2 (self->db,
                               "DELETE FROM sessions "
                               "WHERE sessions.account_id=?1; ",
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
  sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);

  g_task_return_boolean (task, status == SQLITE_ROW);
}

static void
db_add_session (CmDb  *self,
                GTask *task)
{
  CmOlm *session;
  sqlite3_stmt *stmt;
  const char *username, *account_device, *session_id, *sender_key, *pickle, *room;
  CmSessionType type;
  CmOlmState state;
  int status, account_id, room_id = 0;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  session = g_object_get_data (G_OBJECT (task), "session");
  g_assert (CM_IS_OLM (session));

  room = cm_olm_get_room_id (session);
  type = cm_olm_get_session_type (session);
  username = cm_olm_get_account_id (session);
  session_id = cm_olm_get_session_id (session);
  sender_key = cm_olm_get_sender_key (session);
  account_device = cm_olm_get_account_device (session);
  pickle = g_object_get_data (G_OBJECT (task), "pickle");
  state = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "state"));

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
                               /*                        1           2         3 */
                               "INSERT INTO sessions(account_id,sender_key,session_id,"
                               /* 4    5      6       7        8              9            10 */
                               "type,pickle,room_id,time,session_state,origin_server_ts,json_data) "
                               "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
                               "ON CONFLICT(account_id, sender_key, session_id) DO UPDATE SET "
                               "pickle=?5, session_state=?8",
                               -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, account_id, "binding when adding session");
  matrix_bind_text (stmt, 2, sender_key, "binding when adding session");
  matrix_bind_text (stmt, 3, session_id, "binding when adding session");
  matrix_bind_int (stmt, 4, type, "binding when adding session");
  matrix_bind_text (stmt, 5, pickle, "binding when adding session");
  if (room_id)
    matrix_bind_int (stmt, 6, room_id, "binding when adding session");
  /* Save time in milliseconds */
  matrix_bind_int (stmt, 7, time (NULL) * 1000, "binding when adding session");
  matrix_bind_int (stmt, 8, state, "binding when adding session");

  if (!g_object_get_data (G_OBJECT (session), "-cm-db-id"))
  {
    g_autofree char *json_str = NULL;
    JsonObject *json, *child;

    g_object_set_data (G_OBJECT (session), "-cm-db-id",
                       GINT_TO_POINTER (sqlite3_last_insert_rowid (self->db)));
    json = json_object_new ();
    json_object_set_object_member (json, "local", json_object_new ());
    child = cm_utils_json_object_get_object (json, "local");
    json_object_set_string_member (child, "first_pickle", pickle);
    json_str = cm_utils_json_object_to_string (json, FALSE);

    matrix_bind_text (stmt, 10, json_str, "binding when adding session");
  }

  status = sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  if (status != SQLITE_DONE)
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "%s", sqlite3_errmsg (self->db));
  else
    g_task_return_boolean (task, TRUE);
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
  char *pickle_key, *room;
  CmOlm *session = NULL;
  CmSessionType type;
  int status, account_id, room_id = 0;

  g_assert (CM_IS_DB (self));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (G_IS_TASK (task));

  type = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "type"));

  room = g_object_get_data (G_OBJECT (task), "room-id");
  username = g_object_get_data (G_OBJECT (task), "account-id");
  session_id = g_object_get_data (G_OBJECT (task), "session-id");
  sender_key = g_object_get_data (G_OBJECT (task), "sender-key");
  pickle_key = g_object_get_data (G_OBJECT (task), "pickle-key");
  account_device = g_object_get_data (G_OBJECT (task), "account-device");

  account_id = matrix_db_get_account_id (self, username, account_device, NULL, FALSE);

  if (!account_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  /* If no session-id is given, only MegOlm out session can be requested */
  if (!session_id && type != SESSION_MEGOLM_V1_OUT)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Requested session without session id");
      return;
    }

  if (room)
    room_id =  matrix_db_get_room_id (self, account_id, room, FALSE);

  if (session_id)
    sqlite3_prepare_v2 (self->db,
                        "SELECT id,pickle FROM sessions "
                        "WHERE account_id=? AND sender_key=? AND type=? "
                        "AND session_id=? AND session_state=0",
                        -1, &stmt, NULL);
  else
    sqlite3_prepare_v2 (self->db,
                        "SELECT id,pickle FROM sessions "
                        "WHERE account_id=? AND sender_key=? AND type=?"
                        "AND room_id=? AND session_state=0 "
                        "ORDER BY id DESC LIMIT 1",
                        -1, &stmt, NULL);


  matrix_bind_int (stmt, 1, account_id, "binding when looking up session");
  matrix_bind_text (stmt, 2, sender_key, "binding when looking up session");
  matrix_bind_int (stmt, 3, type, "binding when looking up session");
  if (session_id)
    matrix_bind_text (stmt, 4, session_id, "binding when looking up session");
  else if (room_id)
    matrix_bind_int (stmt, 4, room_id, "binding when looking up session");

  status = sqlite3_step (stmt);

  if (status == SQLITE_ROW)
    {
      g_autofree char *pickle = NULL;
      int id;

      pickle = g_strdup ((const char *)sqlite3_column_text (stmt, 1));
      session = cm_olm_new_from_pickle (pickle, pickle_key, sender_key, type);
      id = sqlite3_column_int (stmt, 0);

      if (session)
        g_object_set_data (G_OBJECT (session), "-cm-db-id", GINT_TO_POINTER (id));
    }

  sqlite3_finalize (stmt);
  g_task_return_pointer (task, session, g_object_unref);
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
                      "SELECT pickle FROM sessions "
                      "WHERE account_id=? AND sender_key=? AND type=?",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, account_id, "binding when looking up olm session");
  matrix_bind_text (stmt, 2, sender_curve_key, "binding when looking up olm session");
  matrix_bind_int (stmt, 3, type, "binding when looking up olm session");

  while (sqlite3_step (stmt) == SQLITE_ROW)
    {
      char *pickle;

      pickle = (char *)sqlite3_column_text (stmt, 0);
      session = cm_olm_match_olm_session (body, strlen (body), message_type,
                                          pickle, pickle_key, sender_curve_key,
                                          type, &plain_text);

      if (session)
        break;
    }

  cm_utils_free_buffer (pickle_key);
  sqlite3_finalize (stmt);

  g_object_set_data_full (G_OBJECT (task), "plaintext", plain_text,
                          (GDestroyNotify)cm_utils_free_buffer);
  g_task_return_pointer (task, session, g_object_unref);
}

static void
db_update_user_tracking (CmDb     *self,
                         int       user_id,
                         gboolean  outdated,
                         gboolean  is_tracking)
{
  sqlite3_stmt *stmt;

  g_assert (CM_IS_DB (self));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (user_id);

  sqlite3_prepare_v2 (self->db,
                      "UPDATE users SET tracking=?1, outdated=?2 "
                      "WHERE id=?3",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, is_tracking, "binding add user device");
  matrix_bind_int (stmt, 2, outdated, "binding add user device");
  matrix_bind_int (stmt, 3, user_id, "binding add user device");
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
}

static void
db_mark_user_device_change (CmDb  *self,
                            GTask *task)
{
  GPtrArray *users;
  const char *username, *account_device;
  gboolean outdated, is_tracking;
  int account_id;

  g_assert (CM_IS_DB (self));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (G_IS_TASK (task));

  is_tracking = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "tracking"));
  outdated = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "outdated"));
  username = g_object_get_data (G_OBJECT (task), "account-id");
  users = g_object_get_data (G_OBJECT (task), "users");
  account_device = g_object_get_data (G_OBJECT (task), "account-device");
  g_assert (users && users->len);

  sqlite3_exec (self->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  account_id = matrix_db_get_account_id (self, username, account_device, NULL, FALSE);

  if (!account_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  for (guint i = 0; i < users->len; i++)
    {
      int user_id;

      username = cm_user_get_id (users->pdata[i]);
      user_id = matrix_db_get_user_id (self, account_id, username, TRUE);
      db_update_user_tracking (self, user_id, outdated, is_tracking);
    }
  sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);

  g_task_return_boolean (task, TRUE);
}

static void
db_update_user_devices (CmDb  *self,
                        GTask *task)
{
  const char *account_username, *account_device, *username;
  GPtrArray *added, *removed;
  int account_id, user_id, force_add;

  g_assert (CM_IS_DB (self));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (G_IS_TASK (task));

  account_username = g_object_get_data (G_OBJECT (task), "account-id");
  account_device = g_object_get_data (G_OBJECT (task), "account-device");
  force_add = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "force-add"));
  username = g_object_get_data (G_OBJECT (task), "username");
  removed = g_object_get_data (G_OBJECT (task), "removed");
  added = g_object_get_data (G_OBJECT (task), "added");
  g_assert (added || removed);

  sqlite3_exec (self->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  account_id = matrix_db_get_account_id (self, account_username, account_device, NULL, FALSE);

  if (!account_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  user_id = matrix_db_get_user_id (self, account_id, username, force_add);

  if (!user_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "User not in db");
      return;
    }

  for (guint i = 0; added &&  i < added->len; i++)
    {
      sqlite3_stmt *stmt;
      CmDevice *device;
      gboolean verified;

      device = added->pdata[i];
      verified = cm_device_is_verified (device);
      sqlite3_prepare_v2 (self->db,
                          "INSERT INTO user_devices(user_id,device,curve25519_key,ed25519_key,verification) "
                          "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(user_id,device) DO UPDATE SET "
                          "verification=?5",
                          -1, &stmt, NULL);

      matrix_bind_int (stmt, 1, user_id, "binding add user device");
      matrix_bind_text (stmt, 2, cm_device_get_id (device), "binding add user device");
      matrix_bind_text (stmt, 3, cm_device_get_curve_key (device), "binding add user device");
      matrix_bind_text (stmt, 4, cm_device_get_ed_key (device), "binding add user device");
      if (verified)
        matrix_bind_int (stmt, 5, VERIFICATION_VERIFIED, "binding add user device");

      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  for (guint i = 0; removed &&  i < removed->len; i++)
    {
      sqlite3_stmt *stmt;
      CmDevice *device;

      device = removed->pdata[i];
      sqlite3_prepare_v2 (self->db,
                          "DELETE FROM user_devices WHERE user_id=?1 AND device=?2",
                          -1, &stmt, NULL);

      matrix_bind_int (stmt, 1, user_id, "binding add user device");
      matrix_bind_text (stmt, 2, cm_device_get_id (device), "binding add user device");
      sqlite3_step (stmt);
      sqlite3_finalize (stmt);
    }

  db_update_user_tracking (self, user_id, FALSE, TRUE);
  sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);

  g_task_return_boolean (task, TRUE);
}

static void
db_update_user_device (CmDb  *self,
                       GTask *task)
{
  const char *account_username, *account_device, *username;
  sqlite3_stmt *stmt;
  CmDevice *device;
  int account_id, user_id;
  gboolean verified;

  g_assert (CM_IS_DB (self));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (G_IS_TASK (task));

  account_username = g_object_get_data (G_OBJECT (task), "account-id");
  account_device = g_object_get_data (G_OBJECT (task), "account-device");
  username = g_object_get_data (G_OBJECT (task), "username");
  device = g_object_get_data (G_OBJECT (task), "device");
  g_assert (device);

  sqlite3_exec (self->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  account_id = matrix_db_get_account_id (self, account_username, account_device, NULL, FALSE);

  if (!account_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR,
                               "Error getting account id");
      return;
    }

  user_id = matrix_db_get_user_id (self, account_id, username, FALSE);

  if (!user_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "User not in db");
      return;
    }

  verified = cm_device_is_verified (device);

  sqlite3_prepare_v2 (self->db,
                      "INSERT INTO user_devices(user_id,device,curve25519_key,ed25519_key,verification) "
                      "VALUES(?1,?2,?3,?4,?5) ON CONFLICT(user_id,device) DO UPDATE SET "
                      "verification=?5",
                      -1, &stmt, NULL);

  matrix_bind_int (stmt, 1, user_id, "binding add user device");
  matrix_bind_text (stmt, 2, cm_device_get_id (device), "binding add user device");
  matrix_bind_text (stmt, 3, cm_device_get_curve_key (device), "binding add user device");
  matrix_bind_text (stmt, 4, cm_device_get_ed_key (device), "binding add user device");
  if (verified)
    matrix_bind_int (stmt, 5, VERIFICATION_VERIFIED, "binding add user device");

  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);

  g_task_return_boolean (task, TRUE);
}

static void
cm_db_delete_event_with_txn_id (CmDb       *self,
                                int         room_id,
                                const char *txnid)
{
  sqlite3_stmt *stmt;

  g_assert (CM_IS_DB (self));

  if (!room_id || !txnid || !*txnid)
    return;

  sqlite3_prepare_v2 (self->db,
                      "DELETE FROM room_events "
                      "WHERE room_id=? AND txnid=? AND event_uid IS NULL",
                      -1, &stmt, NULL);
  matrix_bind_int (stmt, 1, room_id, "binding when deleting room event txnid");
  matrix_bind_text (stmt, 2, txnid, "binding when deleting room event txnid");
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
}

static void
db_add_room_members (CmDb  *self,
                     GTask *task)
{
  const char *username, *device, *room;
  GPtrArray *members;
  int room_id, account_id;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  username = g_object_get_data (G_OBJECT (task), "username");
  members = g_object_get_data (G_OBJECT (task), "members");
  device = g_object_get_data (G_OBJECT (task), "device");
  room = g_object_get_data (G_OBJECT (task), "room");
  g_assert (members && members->len);

  account_id = matrix_db_get_account_id (self, username, device, NULL, FALSE);
  room_id = matrix_db_get_room_id (self, account_id, room, FALSE);

  if (!room_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "Account or Room not found in db");
      return;
    }

  /* todo: Look into sqlite transactions */
  for (guint i = 0; i < members->len; i++)
    {
      CmUser *cm_user = members->pdata[i];
      GListModel *devices;
      guint n_items;
      int member_id, user_id = 0;

      username = cm_user_get_id (cm_user);
      member_id = db_get_room_member_id (self, account_id, room_id, username, &user_id, TRUE);

      if (!member_id)
        continue;

      cm_db_update_user (self, user_id, cm_user);
      devices = cm_user_get_devices (cm_user);
      n_items = g_list_model_get_n_items (devices);

      for (guint j = 0; j < n_items; j++)
        {
          g_autoptr(CmDevice) cm_device = NULL;

          cm_device = g_list_model_get_item (devices, j);
          cm_db_update_user_device (self, user_id, cm_device);
        }
    }

  g_task_return_boolean (task, TRUE);
}

static void
db_add_room_events (CmDb  *self,
                    GTask *task)
{
  const char *username, *device, *room;
  sqlite3_stmt *stmt;
  GPtrArray *events;
  int room_id, account_id, sorted_event_id = 0, match_id = 0;
  gboolean prepend;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  username = g_object_get_data (G_OBJECT (task), "username");
  device = g_object_get_data (G_OBJECT (task), "device");
  room = g_object_get_data (G_OBJECT (task), "room");
  events = g_object_get_data (G_OBJECT (task), "events");
  prepend = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (task), "prepend"));
  g_assert (events && events->len);

  sqlite3_exec (self->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);
  account_id = matrix_db_get_account_id (self, username, device, NULL, FALSE);
  room_id = matrix_db_get_room_id (self, account_id, room, FALSE);

  if (prepend)
    match_id = db_get_first_room_event_id (self, room_id, &sorted_event_id);
  else
    match_id = db_get_last_room_event_id (self, room_id, &sorted_event_id);

  if (match_id)
    prepend ? (--sorted_event_id) : (++sorted_event_id);

  if (!room_id)
    {
      sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "Account or Room not found in db");
      return;
    }

  for (guint i = 0; i < events->len; i++)
    {
      g_autoptr(JsonObject) encrypted = NULL;
      g_autoptr(JsonObject) json_obj = NULL;
      g_autoptr(JsonObject) json = NULL;
      JsonObject *local = NULL;
      CmEvent *event = events->pdata[i];
      g_autofree char *json_str = NULL;
      const char *sender;
      int member_id, replaces_id, replaces_cache_id = 0;
      int event_state, status;

      json = cm_event_get_json (event);
      encrypted = cm_event_get_encrypted_json (event);
      sender = cm_event_get_sender_id (event);

      member_id = db_get_room_member_id (self, account_id, room_id, sender, NULL, TRUE);

      /* Delete existing ones as we add them below so that the sort order is right */
      if (cm_event_get_txn_id (event))
        cm_db_delete_event_with_txn_id (self, room_id, cm_event_get_txn_id (event));

      if (!member_id)
        continue;

      replaces_id = db_get_room_event_id (self, room_id, NULL,
                                          cm_event_get_replaces_id (event));
      if (cm_event_get_replaces_id (event) && !replaces_id)
        replaces_cache_id = db_get_room_cache_event_id (self, room_id,
                                                        cm_event_get_replaces_id (event), TRUE);

      json_obj = json_object_new ();
      if (json)
        json_object_set_object_member (json_obj, "json", g_steal_pointer (&json));
      if (encrypted)
        json_object_set_object_member (json_obj, "encrypted", g_steal_pointer (&encrypted));

      if (cm_event_get_txn_id (event))
        {
          local = json_object_new ();
          json_object_set_string_member (local, "txnid",
                                         cm_event_get_txn_id (event));
        }

      if (local)
        json_object_set_object_member (json_obj, "local", local);

      json_str = cm_utils_json_object_to_string (json_obj, FALSE);
      event_state = db_event_state_to_int (cm_event_get_state (event));

      sqlite3_prepare_v2 (self->db,
                          /*                          1       2         3 */
                          "INSERT INTO room_events(sorted_id,room_id,sender_id,"
                          /*   4           5      6          7                    8 */
                          "event_type,event_uid,txnid,replaces_event_id,replaces_event_cache_id,"
                          /*   9           10             11          12         13*/
                          "event_state,state_key,origin_server_ts,decryption,json_data) "
                          "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)",
                          -1, &stmt, NULL);
      matrix_bind_int (stmt, 1, sorted_event_id, "binding when adding event");
      matrix_bind_int (stmt, 2, room_id, "binding when adding event");
      matrix_bind_int (stmt, 3, member_id, "binding when adding event");
      matrix_bind_int (stmt, 4, cm_event_get_m_type (event), "binding when adding event");
      matrix_bind_text (stmt, 5, cm_event_get_id (event), "binding when adding event");
      if (cm_event_get_txn_id (event))
        matrix_bind_text (stmt, 6, cm_event_get_txn_id (event), "binding when adding event");
      if (replaces_id)
        matrix_bind_int (stmt, 7, replaces_id, "binding when adding event");
      if (replaces_cache_id)
        matrix_bind_int (stmt, 8, replaces_cache_id, "binding when adding event");
      matrix_bind_int (stmt, 9, event_state, "binding when adding event");
      matrix_bind_text (stmt, 10, cm_event_get_state_key (event), "binding when adding event");
      matrix_bind_int (stmt, 11, cm_event_get_time_stamp (event), "binding when adding event");
      matrix_bind_int (stmt, 12, db_event_get_decryption_value (event), "binding when adding event");
      matrix_bind_text (stmt, 13, json_str, "binding when adding event");
      status = sqlite3_step (stmt);
      sqlite3_finalize (stmt);

      if (status == SQLITE_DONE)
        {
          int event_id, event_cache_id;

          event_id = sqlite3_last_insert_rowid (self->db);
          event_cache_id = db_get_room_cache_event_id (self, room_id,
                                                     cm_event_get_id (event), FALSE);

          if (event_id && event_cache_id)
            {
              sqlite3_prepare_v2 (self->db,
                                  "UPDATE room_events SET replaces_event_id=?"
                                  "WHERE replaces_event_cache_id=?;",
                                  -1, &stmt, NULL);
              matrix_bind_int (stmt, 1, event_id, "binding when adding event");
              matrix_bind_int (stmt, 2, event_cache_id, "binding when adding event");
              sqlite3_step (stmt);
              sqlite3_finalize (stmt);
            }
        }
      else
        {
          g_warning ("Failed to save event: %s, error: %s",
                     cm_event_get_id (event),
                     sqlite3_errmsg (self->db));
        }

      prepend ? (--sorted_event_id) : (++sorted_event_id);
    }
  sqlite3_exec (self->db, "END TRANSACTION;", NULL, NULL, NULL);

  g_task_return_boolean (task, TRUE);
}

static void
db_get_past_events (CmDb  *self,
                    GTask *task)
{
  const char *username, *device, *room, *event;
  GPtrArray *events = NULL;
  CmRoom *cm_room;
  int room_id, account_id, event_id, sorted_event_id = 0;

  g_assert (CM_IS_DB (self));
  g_assert (G_IS_TASK (task));
  g_assert (g_thread_self () == self->worker_thread);
  g_assert (self->db);

  room = g_object_get_data (G_OBJECT (task), "room");
  event = g_object_get_data (G_OBJECT (task), "event");
  device = g_object_get_data (G_OBJECT (task), "device");
  cm_room = g_object_get_data (G_OBJECT (task), "cm-room");
  username = g_object_get_data (G_OBJECT (task), "username");

  account_id = matrix_db_get_account_id (self, username, device, NULL, FALSE);
  room_id = matrix_db_get_room_id (self, account_id, room, FALSE);

  if (event)
    event_id = db_get_room_event_id (self, room_id, &sorted_event_id, event);
  else
    event_id = db_get_last_room_event_id (self, room_id, &sorted_event_id);

  if (!event_id)
    {
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                               "Couldn't find event in db");
      return;
    }

  events = db_get_past_room_events (self, cm_room, room_id,
                                    event ? event_id : 0,
                                    sorted_event_id, 30);
  g_task_return_pointer (task, events, (GDestroyNotify)g_ptr_array_unref);
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
 * cm_db_close_finish:
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
  const char *username, *device_id, *prev_batch;
  const char *replacement;
  CmStatus room_status;
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_CLIENT (client));

  task = g_task_new (self, NULL, callback, user_data);
  g_task_set_source_tag (task, cm_db_save_room_async);
  g_task_set_task_data (task, cm_db_save_room, NULL);

  username = cm_client_get_user_id (client);
  device_id = cm_client_get_device_id (client);
  prev_batch = cm_room_get_prev_batch (room);
  replacement = cm_room_get_replacement_room (room);
  room_status = cm_room_get_status (room);

  g_object_set_data_full (G_OBJECT (task), "username", g_strdup (username), g_free);
  g_object_set_data_full (G_OBJECT (task), "room", g_object_ref (room), g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "json", cm_room_get_json (room), g_free);
  g_object_set_data_full (G_OBJECT (task), "prev-batch", g_strdup (prev_batch), g_free);
  g_object_set_data_full (G_OBJECT (task), "client", g_object_ref (client), g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "client-device", g_strdup (device_id), g_free);
  g_object_set_data_full (G_OBJECT (task), "replacement", g_strdup (replacement), g_free);
  g_object_set_data (G_OBJECT (task), "status", GINT_TO_POINTER (room_status));

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

gboolean
cm_db_add_session (CmDb     *self,
                   gpointer  session,
                   char     *pickle)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  GObject *object;
  gboolean success;

  g_return_val_if_fail (CM_IS_DB (self), FALSE);
  g_return_val_if_fail (CM_IS_OLM (session), FALSE);
  g_return_val_if_fail (pickle && *pickle, FALSE);

  task = g_task_new (self, NULL, NULL, NULL);
  g_object_ref (task);
  g_task_set_source_tag (task, cm_db_add_session);
  g_task_set_task_data (task, db_add_session, NULL);
  object = G_OBJECT (task);

  g_object_set_data_full (object, "session", g_object_ref (session), g_object_unref);
  g_object_set_data_full (object, "pickle", pickle, g_free);
  g_object_set_data (object, "state", GINT_TO_POINTER (cm_olm_get_state (session)));
  if (cm_olm_get_session_type (session) == SESSION_MEGOLM_V1_OUT)
    g_object_set_data (object, "chain-index", GINT_TO_POINTER (cm_olm_get_message_index (session)));

  g_async_queue_push_front (self->queue, task);

  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  success = g_task_propagate_boolean (task, &error);

  if (error)
    g_warning ("Failed to save olm session with id: %s, error: %s",
               cm_olm_get_session_id (session), error->message);

  return success;
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

gpointer
cm_db_lookup_session (CmDb          *self,
                      const char    *account_id,
                      const char    *account_device,
                      const char    *session_id,
                      const char    *sender_key,
                      const char    *pickle_key,
                      const char    *room_id,
                      CmSessionType  type)
{
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  GObject *object;
  gpointer session;

  g_return_val_if_fail (CM_IS_DB (self), NULL);
  g_return_val_if_fail (account_id && *account_id, NULL);
  g_return_val_if_fail (account_device && *account_device, NULL);
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
  g_object_set_data_full (object, "room-id", g_strdup (room_id), g_free);
  g_object_set_data_full (object, "pickle-key", g_strdup (pickle_key),
                          (GDestroyNotify)cm_utils_free_buffer);
  g_object_set_data (object, "type", GINT_TO_POINTER (type));
  g_async_queue_push (self->queue, task);
  g_assert (task);

  /* Wait until the task is completed */
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  session = g_task_propagate_pointer (task, &error);

  if (error)
    g_debug ("Error getting session: %s", error->message);

  return session;
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

  /* Push to end as we may have to match items inserted immediately before */
  g_async_queue_push (self->queue, task);
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
cm_db_mark_user_device_change (CmDb      *self,
                               CmClient  *client,
                               GPtrArray *users,
                               gboolean   outdated,
                               gboolean   is_tracking)
{
  const char *account_id, *device;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  GObject *object;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (users && users->len);

  task = g_task_new (self, NULL, NULL, NULL);
  g_object_ref (task);

  g_task_set_source_tag (task, cm_db_mark_user_device_change);
  g_task_set_task_data (task, db_mark_user_device_change, NULL);
  account_id = cm_client_get_user_id (client);
  device = cm_client_get_device_id (client);
  object = G_OBJECT (task);

  g_debug ("Saving user device change: %p", users);
  g_object_set_data_full (object, "users", g_ptr_array_ref (users),
                          (GDestroyNotify)g_ptr_array_unref);
  g_object_set_data_full (object, "account-id", g_strdup (account_id), g_free);
  g_object_set_data_full (object, "account-device", g_strdup (device), g_free);
  g_object_set_data (object, "tracking", GINT_TO_POINTER (is_tracking));
  g_object_set_data (object, "outdated", GINT_TO_POINTER (outdated));

  g_async_queue_push (self->queue, task);
  g_assert (task);

  /* Wait until the task is completed */
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  g_task_propagate_pointer (task, &error);
  g_debug ("Saving user device change %s", CM_LOG_SUCCESS (!error));

  if (error)
    g_debug ("Error marking user device changed: %s", error->message);
}

void
cm_db_update_user_devices (CmDb       *self,
                           CmClient   *client,
                           CmUser     *user,
                           GPtrArray  *added,
                           GPtrArray  *removed,
                           gboolean    force_add)
{
  const char *account_id, *device;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  GObject *object;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (CM_IS_USER (user));
  g_return_if_fail ((added && added->len) || (removed && removed->len));

  task = g_task_new (self, NULL, NULL, NULL);
  g_object_ref (task);

  g_task_set_source_tag (task, cm_db_update_user_devices);
  g_task_set_task_data (task, db_update_user_devices, NULL);
  account_id = cm_client_get_user_id (client);
  device = cm_client_get_device_id (client);
  object = G_OBJECT (task);

  if (g_application_get_default ())
    g_application_hold (g_application_get_default ());

  if (added)
    g_ptr_array_ref (added);

  if (removed)
    g_ptr_array_ref (removed);

  g_debug ("Updating user devices. user %p, added: %u, removed: %u",
           user, added ? added->len : 0, removed ? removed->len : 0);
  g_object_set_data_full (object, "client", g_object_ref (client), g_object_unref);
  g_object_set_data_full (object, "user", g_object_ref (user), g_object_unref);
  g_object_set_data_full (object, "added", added, (GDestroyNotify)g_ptr_array_unref);
  g_object_set_data_full (object, "removed", removed, (GDestroyNotify)g_ptr_array_unref);
  g_object_set_data_full (object, "account-id", g_strdup (account_id), g_free);
  g_object_set_data_full (object, "username", g_strdup (cm_user_get_id (user)), g_free);
  g_object_set_data_full (object, "account-device", g_strdup (device), g_free);
  g_object_set_data (object, "force-add", GINT_TO_POINTER (force_add));

  g_async_queue_push (self->queue, task);
  g_assert (task);

  /* Wait until the task is completed */
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  if (g_application_get_default ())
    g_application_release (g_application_get_default ());

  g_task_propagate_boolean (task, &error);

  g_debug ("Updating user devices. user %p %s", user, CM_LOG_SUCCESS (!error));

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    g_debug ("Updating user devices. user %p skipped, we are not tracking the user", user);
  else if (error)
    g_warning ("Error updating user devices, user %p: %s", user, error->message);
}

void
cm_db_update_device (CmDb     *self,
                     CmClient *client,
                     CmUser   *user,
                     CmDevice *device)
{
  const char *account_id, *device_id;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  GObject *object;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_CLIENT (client));
  g_return_if_fail (CM_IS_DEVICE (device));
  g_return_if_fail (CM_IS_USER (user));
  g_return_if_fail (cm_device_get_user (device) == user);

  task = g_task_new (self, NULL, NULL, NULL);
  g_object_ref (task);

  g_task_set_source_tag (task, cm_db_update_user_device);
  g_task_set_task_data (task, db_update_user_device, NULL);
  account_id = cm_client_get_user_id (client);
  device_id = cm_client_get_device_id (client);
  object = G_OBJECT (task);

  if (g_application_get_default ())
    g_application_hold (g_application_get_default ());

  g_debug ("Updating device. user %p, device: %p ", user, device);
  g_object_set_data_full (object, "client", g_object_ref (client), g_object_unref);
  g_object_set_data_full (object, "user", g_object_ref (user), g_object_unref);
  g_object_set_data_full (object, "device", g_object_ref (device), g_object_unref);
  g_object_set_data_full (object, "account-id", g_strdup (account_id), g_free);
  g_object_set_data_full (object, "username", g_strdup (cm_user_get_id (user)), g_free);
  g_object_set_data_full (object, "account-device", g_strdup (device_id), g_free);

  g_async_queue_push (self->queue, task);
  g_assert (task);

  /* Wait until the task is completed */
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  if (g_application_get_default ())
    g_application_release (g_application_get_default ());

  g_task_propagate_boolean (task, &error);

  g_debug ("Updating user device. user: %p, device: %p, %s", user, device, CM_LOG_SUCCESS (!error));

  if (error)
    g_warning ("Error updating user devices, user: %p, device: %p: %s", user, device, error->message);
}

void
cm_db_add_room_members (CmDb      *self,
                        CmRoom    *cm_room,
                        GPtrArray *members)
{
  const char *username, *device, *room;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  CmClient *client;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_ROOM (cm_room));

  if (!members || !members->len)
    return;

  if (g_application_get_default ())
    g_application_hold (g_application_get_default ());

  client = cm_room_get_client (cm_room);
  username = cm_client_get_user_id (client);
  device = cm_client_get_device_id (client);
  room = cm_room_get_id (cm_room);

  task = g_task_new (self, NULL, NULL, NULL);
  g_object_ref (task);
  g_object_ref (cm_room);
  g_ptr_array_ref (members);
  g_task_set_task_data (task, db_add_room_members, NULL);
  g_object_set_data_full (G_OBJECT (task), "members", members, (GDestroyNotify)g_ptr_array_unref);
  g_object_set_data_full (G_OBJECT (task), "cm-room", cm_room, g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "room", g_strdup (room), g_free);
  g_object_set_data_full (G_OBJECT (task), "username", g_strdup (username), g_free);
  g_object_set_data_full (G_OBJECT (task), "device", g_strdup (device), g_free);

  g_async_queue_push (self->queue, task);

  /* Wait until the task is completed */
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  if (g_application_get_default ())
    g_application_release (g_application_get_default ());

  g_task_propagate_boolean (task, &error);

  if (error)
    g_warning ("Error saving room members: %s", error->message);
}

/*
 * cm_db_add_room_events:
 * @prepend: Whether the events should be
 * added before or after already saved
 * events. Set %TRUE to add before. %FALSE
 * otherwise.
 */
void
cm_db_add_room_events (CmDb      *self,
                       CmRoom    *cm_room,
                       GPtrArray *events,
                       gboolean   prepend)
{
  const char *username, *device, *room;
  g_autoptr(GTask) task = NULL;
  g_autoptr(GError) error = NULL;
  CmClient *client;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_ROOM (cm_room));

  if (!events || !events->len)
    return;

  if (g_application_get_default ())
    g_application_hold (g_application_get_default ());

  client = cm_room_get_client (cm_room);
  username = cm_client_get_user_id (client);
  device = cm_client_get_device_id (client);
  room = cm_room_get_id (cm_room);

  task = g_task_new (self, NULL, NULL, NULL);
  g_object_ref (task);
  g_object_ref (cm_room);
  g_ptr_array_ref (events);
  g_task_set_task_data (task, db_add_room_events, NULL);
  g_object_set_data_full (G_OBJECT (task), "events", events, (GDestroyNotify)g_ptr_array_unref);
  g_object_set_data_full (G_OBJECT (task), "cm-room", cm_room, g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "room", g_strdup (room), g_free);
  g_object_set_data_full (G_OBJECT (task), "username", g_strdup (username), g_free);
  g_object_set_data_full (G_OBJECT (task), "device", g_strdup (device), g_free);
  g_object_set_data (G_OBJECT (task), "prepend", GINT_TO_POINTER (!!prepend));

  g_async_queue_push (self->queue, task);

  /* Wait until the task is completed */
  while (!g_task_get_completed (task))
    g_main_context_iteration (NULL, TRUE);

  if (g_application_get_default ())
    g_application_release (g_application_get_default ());

  g_task_propagate_boolean (task, &error);

  if (error)
    g_debug ("Error getting session: %s", error->message);
}

void
cm_db_get_past_events_async (CmDb                *self,
                             CmRoom              *room,
                             CmEvent             *from,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  const char *room_name, *username, *device, *event = NULL;
  CmClient *client;
  GTask *task;

  g_return_if_fail (CM_IS_DB (self));
  g_return_if_fail (CM_IS_ROOM (room));
  g_return_if_fail (!from || CM_IS_EVENT (from));

  g_object_ref (room);
  if (from)
    {
      g_object_ref (from);
      event = cm_event_get_id (from);
    }

  client = cm_room_get_client (room);
  room_name = cm_room_get_id (room);
  username = cm_client_get_user_id (client);
  device = cm_client_get_device_id (client);
  g_return_if_fail (device && *device);

  task = g_task_new (self, NULL, callback, user_data);
  g_object_set_data_full (G_OBJECT (task), "cm-room", room, g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "cm-event", from, g_object_unref);
  g_object_set_data_full (G_OBJECT (task), "room", g_strdup (room_name), g_free);
  g_object_set_data_full (G_OBJECT (task), "event", g_strdup (event), g_free);
  g_object_set_data_full (G_OBJECT (task), "username", g_strdup (username), g_free);
  g_object_set_data_full (G_OBJECT (task), "device", g_strdup (device), g_free);
  g_task_set_source_tag (task, cm_db_get_past_events_async);
  g_task_set_task_data (task, db_get_past_events, NULL);

  g_async_queue_push (self->queue, task);
}

GPtrArray *
cm_db_get_past_events_finish (CmDb          *self,
                              GAsyncResult  *result,
                              GError       **error)
{
  g_return_val_if_fail (CM_IS_DB (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);
  g_return_val_if_fail (!error || !*error, NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}
