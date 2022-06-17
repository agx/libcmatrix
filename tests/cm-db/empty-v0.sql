BEGIN TRANSACTION;

CREATE TABLE devices(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  device TEXT NOT NULL UNIQUE
);

CREATE TABLE users(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  username TEXT NOT NULL,
  device_id INTEGER REFERENCES devices(id),
  UNIQUE (username, device_id)
);

CREATE TABLE accounts(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  user_id INTEGER NOT NULL REFERENCES users(id),
  next_batch TEXT,
  pickle TEXT,
  enabled INTEGER DEFAULT 0,
  UNIQUE (user_id)
);

CREATE TABLE rooms(
  id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
  account_id INTEGER NOT NULL REFERENCES accounts(id),
  room_name TEXT NOT NULL,
  prev_batch TEXT,
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
  UNIQUE (account_id, sender_key, session_id)
);

COMMIT;
