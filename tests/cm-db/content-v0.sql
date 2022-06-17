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


INSERT INTO devices VALUES(3,'ALICE EXAMPLE COM');
INSERT INTO devices VALUES(2,'BOB EXAMPLE COM');
INSERT INTO devices VALUES(4,'ALICE EXAMPLE NET');
INSERT INTO devices VALUES(5,'ALICE EXAMPLE NET 2');
INSERT INTO devices VALUES(1,'ALICE EXAMPLE NET 3');

INSERT INTO users VALUES(1,'@alice:example.com', 3);
INSERT INTO users VALUES(3,'@bob:example.com', 2);
INSERT INTO users VALUES(2,'@alice:example.net', 4);
INSERT INTO users VALUES(5,'@alice:example.net', 5);
INSERT INTO users VALUES(4,'@alice:example.net', 1);

INSERT INTO accounts VALUES(3, 4, 'alice example net batch', 'alice example net pickle', 1);
INSERT INTO accounts VALUES(2, 1, 'alice example com batch', 'alice example com pickle', 1);
INSERT INTO accounts VALUES(4, 3, 'bob example com batch', 'bob example com pickle', 0);

INSERT INTO rooms VALUES(7, 3, 'alice example net room A', 'prev batch 1');
INSERT INTO rooms VALUES(2, 3, 'alice example net room B', 'prev batch 2');
INSERT INTO rooms VALUES(4, 4, 'bob example com room C', 'bob com batch 3');
INSERT INTO rooms VALUES(5, 3, 'alice example net room C', 'prev batch 3');
INSERT INTO rooms VALUES(1, 3, 'alice example net room D', 'prev batch 4');
INSERT INTO rooms VALUES(3, 4, 'bob example com room A', 'bob com batch 1');
INSERT INTO rooms VALUES(9, 4, 'bob example com room B', 'bob com batch 2');

INSERT INTO session VALUES(1, 2, 'alice com key 1', 'alice com id 1', 1, 'alice com id 1', 11111111);
INSERT INTO session VALUES(2, 4, 'bob key 1', 'bob id 1', 1, 'bob id 1', 22222222);
INSERT INTO session VALUES(3, 4, 'bob key 2', 'bob id 2', 1, 'bob id 2', 33333333);
INSERT INTO session VALUES(4, 4, 'bob key 3', 'bob id 3', 2, 'bob id 3', 44444444);
INSERT INTO session VALUES(5, 3, 'net key 1', 'net id 1', 1, 'netid 1', 555555);

COMMIT;
