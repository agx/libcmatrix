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

INSERT INTO users VALUES(1,'@alice:example.com', 1, NULL);
INSERT INTO users VALUES(2,'@alice:example.net', 1, NULL);
INSERT INTO users VALUES(3,'@bob:example.com', 1, NULL);

INSERT INTO user_devices VALUES(3, 1, 'ALICE EXAMPLE COM', NULL, NULL, 0, NULL);
INSERT INTO user_devices VALUES(2, 2, 'ALICE EXAMPLE NET 3', NULL, NULL, 0, NULL);
INSERT INTO user_devices VALUES(4, 3, 'BOB EXAMPLE COM', NULL, NULL, 0, NULL);
INSERT INTO user_devices VALUES(6, 2, 'ALICE EXAMPLE NET', NULL, NULL, 0, NULL);
INSERT INTO user_devices VALUES(5, 2, 'ALICE EXAMPLE NET 2', NULL, NULL, 0, NULL);

INSERT INTO accounts VALUES(3, 2, 'alice example net batch', 'alice example net pickle', 1, NULL);
INSERT INTO accounts VALUES(1, 3, 'alice example com batch', 'alice example com pickle', 1, NULL);
INSERT INTO accounts VALUES(4, 4, 'bob example com batch', 'bob example com pickle', 0, NULL);

INSERT INTO rooms VALUES(8, 3, 'alice example net room A', 'prev batch 1', NULL, NULL);
INSERT INTO rooms VALUES(6, 3, 'alice example net room B', 'prev batch 2', NULL, NULL);
INSERT INTO rooms VALUES(4, 4, 'bob example com room C', 'bob com batch 3', NULL, NULL);
INSERT INTO rooms VALUES(3, 4, 'bob example com room A', 'bob com batch 1', NULL, NULL);
INSERT INTO rooms VALUES(5, 3, 'alice example net room C', 'prev batch 3', NULL, NULL);
INSERT INTO rooms VALUES(9, 4, 'bob example com room B', 'bob com batch 2', NULL, NULL);
INSERT INTO rooms VALUES(2, 3, 'alice example net room D', 'prev batch 4', NULL, NULL);

INSERT INTO session VALUES(1, 1, 'alice com key 1', 'alice com id 1', 1, 'alice com id 1', 11111111, NULL);
INSERT INTO session VALUES(2, 4, 'bob key 1', 'bob id 1', 1, 'bob id 1', 22222222, NULL);
INSERT INTO session VALUES(3, 4, 'bob key 2', 'bob id 2', 1, 'bob id 2', 33333333, NULL);
INSERT INTO session VALUES(4, 4, 'bob key 3', 'bob id 3', 2, 'bob id 3', 44444444, NULL);
INSERT INTO session VALUES(5, 3, 'net key 1', 'net id 1', 1, 'netid 1', 555555, NULL);

COMMIT;
