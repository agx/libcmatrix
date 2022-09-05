BEGIN TRANSACTION;

PRAGMA user_version = 2;
PRAGMA foreign_keys = ON;

CREATE TABLE users(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  account_id INTEGER REFERENCES accounts(id) ON DELETE CASCADE,
  username TEXT NOT NULL,
  tracking INTEGER NOT NULL DEFAULT 0,
  outdated INTEGER DEFAULT 1,
  json_data TEXT,
  UNIQUE (account_id, username)
);

CREATE TABLE user_devices(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES users(id),
  device TEXT NOT NULL,
  curve25519_key TEXT,
  ed25519_key TEXT,
  verification INTEGER DEFAULT 0,
  json_data TEXT,
  UNIQUE (user_id, device)
);

CREATE TABLE accounts(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  user_device_id INTEGER NOT NULL REFERENCES user_devices(id),
  next_batch TEXT,
  pickle TEXT,
  enabled INTEGER DEFAULT 0,
  json_data TEXT,
  UNIQUE (user_device_id)
);

CREATE TABLE rooms(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
  room_name TEXT NOT NULL,
  prev_batch TEXT,
  replacement_room_id INTEGER REFERENCES rooms(id),
  room_state INTEGER NOT NULL DEFAULT 0,
  json_data TEXT,
  UNIQUE (account_id, room_name)
);

CREATE TABLE IF NOT EXISTS room_members (
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  user_id INTEGER NOT NULL REFERENCES users(id),
  user_state INTEGER NOT NULL DEFAULT 0,
  json_data TEXT,
  UNIQUE (room_id, user_id)
);

CREATE TABLE IF NOT EXISTS room_events_cache (
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  sender_id INTEGER REFERENCES room_members(id),
  event_uid TEXT NOT NULL,
  origin_server_ts INTEGER,
  json_data TEXT,
  UNIQUE (room_id, event_uid)
);

CREATE TABLE IF NOT EXISTS room_events (
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  sorted_id INTEGER NOT NULL,
  room_id INTEGER NOT NULL REFERENCES rooms(id) ON DELETE CASCADE,
  sender_id INTEGER NOT NULL REFERENCES room_members(id),
  event_type INTEGER NOT NULL,
  event_uid TEXT,
  txnid TEXT,
  replaces_event_id INTEGER REFERENCES room_events(id),
  replaces_event_cache_id INTEGER REFERENCES room_events_cache(id),
  replaced_with_id INTEGER REFERENCES room_events(id),
  reply_to_id INTEGER REFERENCES room_events(id),
  event_state INTEGER,
  state_key TEXT,
  origin_server_ts INTEGER NOT NULL,
  decryption INTEGER NOT NULL DEFAULT 0,
  json_data TEXT,
  UNIQUE (room_id, event_uid)
);

CREATE TABLE encryption_keys(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  account_id INTEGER REFERENCES accounts(id) ON DELETE CASCADE,
  file_url TEXT NOT NULL,
  file_sha256 TEXT,
  iv TEXT NOT NULL,
  version INT DEFAULT 2 NOT NULL,
  algorithm INT NOT NULL,
  key TEXT NOT NULL,
  type INT NOT NULL,
  extractable INT DEFAULT 1 NOT NULL,
  json_data TEXT,
  UNIQUE (account_id, file_url)
);

CREATE TABLE sessions (
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
  sender_key TEXT NOT NULL,
  session_id TEXT NOT NULL,
  type INTEGER NOT NULL,
  pickle TEXT NOT NULL,
  time INT,
  origin_server_ts INTEGER,
  chain_index INTEGER,
  session_state INTEGER NOT NULL DEFAULT 0,
  json_data TEXT,
  UNIQUE (account_id, sender_key, session_id)
);

CREATE UNIQUE INDEX IF NOT EXISTS room_event_idx ON room_events (room_id, event_uid);
CREATE UNIQUE INDEX IF NOT EXISTS room_event_txn_idx ON room_events (room_id, txnid);
CREATE UNIQUE INDEX IF NOT EXISTS user_device_idx ON user_devices (user_id, device);
CREATE INDEX IF NOT EXISTS room_event_state_idx ON room_events (state_key);
CREATE UNIQUE INDEX IF NOT EXISTS room_event_cache_idx ON room_events_cache (room_id, event_uid);
CREATE UNIQUE INDEX IF NOT EXISTS encryption_key_idx ON encryption_keys (account_id, file_url);
CREATE INDEX IF NOT EXISTS session_sender_idx ON sessions (account_id, sender_key);
CREATE INDEX IF NOT EXISTS user_idx ON users (username);

CREATE TRIGGER IF NOT EXISTS insert_replaced_with_id AFTER INSERT
ON room_events FOR EACH ROW WHEN NEW.replaces_event_id IS NOT NULL
BEGIN
  UPDATE room_events SET replaced_with_id=NEW.id
  WHERE id=NEW.replaces_event_id AND (replaced_with_id IS NULL or replaced_with_id < NEW.id);
END;

CREATE TRIGGER IF NOT EXISTS update_replaced_with_id AFTER UPDATE OF replaces_event_id
ON room_events FOR EACH ROW WHEN NEW.replaces_event_id IS NOT NULL
BEGIN
  UPDATE room_events SET replaced_with_id=NEW.id
  WHERE id=NEW.replaces_event_id AND (replaced_with_id IS NULL or replaced_with_id < NEW.id);
END;

COMMIT;
