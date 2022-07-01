BEGIN TRANSACTION;

PRAGMA user_version = 1;
PRAGMA foreign_keys = ON;

CREATE TABLE users(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  username TEXT NOT NULL UNIQUE,
  outdated INTEGER DEFAULT 1,
  json_data TEXT
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
  account_id INTEGER NOT NULL REFERENCES accounts(id),
  room_name TEXT NOT NULL,
  prev_batch TEXT,
  replacement_room_id INTEGER REFERENCES rooms(id),
  json_data TEXT,
  UNIQUE (account_id, room_name)
);

CREATE TABLE encryption_keys(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  file_url TEXT NOT NULL,
  file_sha256 TEXT,
  iv TEXT NOT NULL,
  version INT DEFAULT 2 NOT NULL,
  algorithm INT NOT NULL,
  key TEXT NOT NULL,
  type INT NOT NULL,
  extractable INT DEFAULT 1 NOT NULL,
  json_data TEXT,
  UNIQUE (file_url)
);

CREATE TABLE session(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  account_id INTEGER NOT NULL REFERENCES accounts(id),
  sender_key TEXT NOT NULL,
  session_id TEXT NOT NULL,
  type INTEGER NOT NULL,
  pickle TEXT NOT NULL,
  time INT,
  json_data TEXT,
  UNIQUE (account_id, sender_key, session_id)
);

COMMIT;
