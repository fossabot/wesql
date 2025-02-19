--echo ###################################################################
--echo case 4 duplicate entry on BLOB for DDL and online DML
--echo ###################################################################
--echo case 4.1 DDL to modify PRIMARY KEY to BLOB column
--echo ###################################################################
eval CREATE TABLE t1(a INT PRIMARY KEY, b TEXT NOT NULL, c INT NOT NULL)CHARSET $cs COLLATE $coll;
INSERT INTO t1 VALUES(1, '1111111', 2);
INSERT INTO t1 VALUES(2, '2222222', 4);

SET DEBUG_SYNC= 'se.inplace_copy_ddl_scan_base_begin SIGNAL start_dml WAIT_FOR dml_done';
send ALTER TABLE t1 DROP PRIMARY KEY, ADD PRIMARY KEY(b(4)), ALGORITHM=INPLACE, LOCK=DEFAULT;

  --echo # Switch to dml connection
  connection dml;
  # wait the modify pk ddl enter the inplace_copy_ddl_scan_base_begin point
  SET DEBUG_SYNC= 'now WAIT_FOR start_dml';
  INSERT INTO t1 VALUES(3, '2222333', 6);
  SELECT * FROM t1;
  # signal modify pk DDL that DML finished
  SET DEBUG_SYNC= 'now SIGNAL dml_done';

--echo # Switch to connection default
connection default;
--error 1062
reap;
CHECK TABLE t1;
SELECT * FROM t1;
SHOW CREATE TABLE t1;
DROP TABLE t1;

eval CREATE TABLE t1(a INT PRIMARY KEY, b TEXT NOT NULL, c INT NOT NULL)CHARSET $cs COLLATE $coll;
INSERT INTO t1 VALUES(1, '1111111', 2);
INSERT INTO t1 VALUES(2, '2222111', 4);

SET DEBUG_SYNC= 'se.inplace_unique_check_constraint_begin SIGNAL start_dml WAIT_FOR dml_done';
send ALTER TABLE t1 DROP PRIMARY KEY, ADD PRIMARY KEY(b(4)), ALGORITHM=INPLACE, LOCK=DEFAULT;

  --echo # Switch to dml connection
  connection dml;
  # wait the modify pk ddl enter the inplace_unique_check_constraint_begin point
  SET DEBUG_SYNC= 'now WAIT_FOR start_dml';
  INSERT INTO t1 VALUES(3, '2222333', 6);
  SELECT * FROM t1;
  # signal modify pk DDL that DML finished
  SET DEBUG_SYNC= 'now SIGNAL dml_done';

--echo # Switch to connection default
connection default;
--error 1062
reap;
CHECK TABLE t1;
SELECT * FROM t1;
SHOW CREATE TABLE t1;
DROP TABLE t1;

eval CREATE TABLE t1(a INT PRIMARY KEY, b TEXT NOT NULL, c INT NOT NULL)CHARSET $cs COLLATE $coll;
INSERT INTO t1 VALUES(1, '1111111', 2);
INSERT INTO t1 VALUES(2, '2222222', 4);

SET DEBUG_SYNC= 'se.inplace_unique_check_constraint_done SIGNAL start_dml WAIT_FOR dml_done';
send ALTER TABLE t1 DROP PRIMARY KEY, ADD PRIMARY KEY(b(4)), ALGORITHM=INPLACE, LOCK=DEFAULT;

  --echo # Switch to dml connection
  connection dml;
  # wait the modify pk ddl enter the inplace_unique_check_constraint_done point
  SET DEBUG_SYNC= 'now WAIT_FOR start_dml';
  INSERT INTO t1 VALUES(3, '2222333', 6);
  SELECT * FROM t1;
  # signal modify pk DDL that DML finished
  SET DEBUG_SYNC= 'now SIGNAL dml_done';

--echo # Switch to connection default
connection default;
--error 1062
reap;
CHECK TABLE t1;
SELECT * FROM t1;
SHOW CREATE TABLE t1;
DROP TABLE t1;

--echo ###################################################################
--echo case 4.2 DDL to ADD UNIQUE SECONDARY KEY on BLOB column
--echo ###################################################################
eval CREATE TABLE t1(a INT PRIMARY KEY, b TEXT NOT NULL, c INT NOT NULL)CHARSET $cs COLLATE $coll;
INSERT INTO t1 VALUES(1, '1111111', 2);
INSERT INTO t1 VALUES(2, '2222222', 4);

SET DEBUG_SYNC= 'se.inplace_create_sk_scan_base_begin SIGNAL start_dml WAIT_FOR dml_done';
send ALTER TABLE t1 ADD UNIQUE KEY(b(4)), ALGORITHM=INPLACE, LOCK=DEFAULT;

  --echo # Switch to dml connection
  connection dml;
  # wait the add uk ddl enter the inplace_create_sk_scan_base_begin point
  SET DEBUG_SYNC= 'now WAIT_FOR start_dml';
  INSERT INTO t1 VALUES(3, '2222333', 6);
  SELECT * FROM t1;
  # signal modify pk DDL that DML finished
  SET DEBUG_SYNC= 'now SIGNAL dml_done';

--echo # Switch to connection default
connection default;
--error 1062
reap;
CHECK TABLE t1;
SELECT * FROM t1;
SHOW CREATE TABLE t1;
DROP TABLE t1;

eval CREATE TABLE t1(a INT PRIMARY KEY, b TEXT NOT NULL, c INT NOT NULL)CHARSET $cs COLLATE $coll;
INSERT INTO t1 VALUES(1, '1111111', 2);
INSERT INTO t1 VALUES(2, '2222222', 4);

SET DEBUG_SYNC= 'se.inplace_create_sk_check_constraint_begin SIGNAL start_dml WAIT_FOR dml_done';
send ALTER TABLE t1 ADD UNIQUE KEY(b(4)), ALGORITHM=INPLACE, LOCK=DEFAULT;

  --echo # Switch to dml connection
  connection dml;
  # wait the add uk ddl enter the inplace_create_sk_check_constraint_begin point
  SET DEBUG_SYNC= 'now WAIT_FOR start_dml';
  INSERT INTO t1 VALUES(3, '2222333', 6);
  SELECT * FROM t1;
  # signal modify pk DDL that DML finished
  SET DEBUG_SYNC= 'now SIGNAL dml_done';

--echo # Switch to connection default
connection default;
--error 1062
reap;
CHECK TABLE t1;
SELECT * FROM t1;
SHOW CREATE TABLE t1;
DROP TABLE t1;

eval CREATE TABLE t1(a INT PRIMARY KEY, b TEXT NOT NULL, c INT NOT NULL)CHARSET $cs COLLATE $coll;
INSERT INTO t1 VALUES(1, '1111111', 2);
INSERT INTO t1 VALUES(2, '2222222', 4);

SET DEBUG_SYNC= 'se.inplace_create_sk_check_constraint_done SIGNAL start_dml WAIT_FOR dml_done';
send ALTER TABLE t1 ADD UNIQUE KEY(b(4)), ALGORITHM=INPLACE, LOCK=DEFAULT;

  --echo # Switch to dml connection
  connection dml;
  # wait the add uk ddl enter the inplace_create_sk_check_constraint_done point
  SET DEBUG_SYNC= 'now WAIT_FOR start_dml';
  INSERT INTO t1 VALUES(3, '2222333', 6);
  SELECT * FROM t1;
  # signal modify pk DDL that DML finished
  SET DEBUG_SYNC= 'now SIGNAL dml_done';

--echo # Switch to connection default
connection default;
--error 1062
reap;
CHECK TABLE t1;
SELECT * FROM t1;
SHOW CREATE TABLE t1;
DROP TABLE t1;

