--let $gtid_violation= 1
--let $error_code= ER_GTID_UNSAFE_CREATE_SELECT
--let $error_message= Statement violates GTID consistency: CREATE TABLE ... SELECT.
--let $statement_ends_transaction= 1
--let $sync_point= before_execute_sql_command

CREATE TABLE t0 (a INT) ENGINE = SMARTENGINE;
CREATE TABLE empty_smartengine (a INT) ENGINE = SMARTENGINE;
CREATE TABLE empty_myisam (a INT) ENGINE = MyISAM;
CREATE TABLE nonempty_smartengine (a INT) ENGINE = SMARTENGINE;
CREATE TABLE nonempty_myisam (a INT) ENGINE = MyISAM;
INSERT INTO nonempty_smartengine VALUES (1);
INSERT INTO nonempty_myisam VALUES (1);

# CREATE...SELECT for base table is not allowed, regardless of engine,
# since it may get logged as CREATE followed by BEGIN; rows; COMMIT
# (on this server if binlog_format='row' or on a slave that uses
# binlog_format='row').
#
# There is a corner case in the server code when 0 rows are selected,
# so we test that too.

#--echo ---- CREATE ... SELECT (value, 1 row) ----
#--let $statement= CREATE TABLE t1 (a INT) SELECT 1
#--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
#DROP TABLE IF EXISTS t1;
#
#--echo ---- CREATE ... SELECT (SMARTENGINE, 1 row) ----
#--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM nonempty_smartengine
#--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
#DROP TABLE IF EXISTS t1;
#
#--echo ---- CREATE ... SELECT (MyISAM, 1 row) ----
#--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM nonempty_myisam
#--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
#DROP TABLE IF EXISTS t1;
#
#--echo ---- CREATE ... SELECT (SMARTENGINE, 0 row) ----
#--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM empty_smartengine
#--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
#DROP TABLE IF EXISTS t1;

#--echo ---- CREATE ... SELECT (MyISAM, 0 row) ----
#--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM empty_myisam
#--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
#DROP TABLE IF EXISTS t1;

--echo ---- CREATE ... SELECT (value, 1 row, ongoing trx) ----
if ($gtid_next == GTID)
{
  --let $error_code= ER_CANT_DO_IMPLICIT_COMMIT_IN_TRX_WHEN_GTID_NEXT_IS_SET
  --let $error_message= Cannot execute statements with implicit commit inside a transaction
}

if ($gtid_next == GTID)
{
--let $pre_statement= BEGIN; INSERT INTO t0 VALUES (1);
--let $statement= CREATE TABLE t1 (a INT) SELECT 1
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;
}

--echo ---- CREATE ... SELECT (SMARTENGINE, 1 row, ongoing trx) ----
if ($gtid_next == GTID)
{
--let $pre_statement= BEGIN; INSERT INTO t0 VALUES (1);
--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM nonempty_smartengine
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;
}

--echo ---- CREATE ... SELECT (MyISAM, 1 row, ongoing trx) ----
if ($gtid_next == GTID)
{
--let $pre_statement= BEGIN; INSERT INTO t0 VALUES (1);
--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM nonempty_myisam
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;
}

--echo ---- CREATE ... SELECT (SMARTENGINE, 0 row, ongoing trx) ----
if ($gtid_next == GTID)
{
--let $pre_statement= BEGIN; INSERT INTO t0 VALUES (1);
--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM empty_smartengine
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;
}

--echo ---- CREATE ... SELECT (MyISAM, 0 row, ongoing trx) ----
if ($gtid_next == GTID)
{
--let $pre_statement= BEGIN; INSERT INTO t0 VALUES (1);
--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM empty_myisam
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;
}

DROP TABLE empty_myisam, empty_smartengine, nonempty_myisam, nonempty_smartengine, t0;
