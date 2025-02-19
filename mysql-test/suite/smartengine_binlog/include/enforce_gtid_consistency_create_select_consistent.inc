# copied from extra/binlog_tests/enforce_gtid_consistency_create_select_consistent.test
--let $gtid_violation= 0
--let $error_code= 0
--let $statement_ends_transaction= 1
--let $sync_point= before_execute_sql_command

# CREATE TEMPORARY...SELECT is GTID consistent since it cannot
# generate row events, since temporary DML is never logged in row
# format.

CREATE TABLE empty_smartengine (a INT) ENGINE = SMARTENGINE;
CREATE TABLE empty_myisam (a INT) ENGINE = MyISAM;
CREATE TABLE nonempty_smartengine (a INT) ENGINE = SMARTENGINE;
CREATE TABLE nonempty_myisam (a INT) ENGINE = MyISAM;
INSERT INTO nonempty_smartengine VALUES (1);
INSERT INTO nonempty_myisam VALUES (1);

--echo ---- CREATE TEMPORARY ... SELECT (value, 1 row) ----
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) SELECT 1
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

--echo ---- CREATE TEMPORARY ... SELECT (SMARTENGINE, 1 row) ----
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) SELECT * FROM nonempty_smartengine
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

--echo ---- CREATE TEMPORARY ... SELECT (MyISAM, 1 row) ----
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) SELECT * FROM nonempty_myisam
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

--echo ---- CREATE TEMPORARY ... SELECT (SMARTENGINE, 0 row) ----
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) SELECT * FROM empty_smartengine
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

--echo ---- CREATE TEMPORARY ... SELECT (MyISAM, 0 row) ----
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) SELECT * FROM empty_myisam
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

# When SQL_LOG_BIN=0, it is GTID-consistent since nothing is logged.
SET SQL_LOG_BIN = 0;

--echo ---- CREATE ... SELECT, SQL_LOG_BIN=0 (value, 1 row) ----
--let $statement= CREATE TABLE t1 (a INT) SELECT 1
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

--echo ---- CREATE ... SELECT, SQL_LOG_BIN=0 (SMARTENGINE, 1 row) ----
--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM nonempty_smartengine
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

--echo ---- CREATE ... SELECT, SQL_LOG_BIN=0 (MyISAM, 1 row) ----
--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM nonempty_myisam
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

--echo ---- CREATE ... SELECT, SQL_LOG_BIN=0 (SMARTENGINE, 0 row) ----
--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM empty_smartengine
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

--echo ---- CREATE ... SELECT, SQL_LOG_BIN=0 (MyISAM, 0 row) ----
--let $statement= CREATE TABLE t1 (a INT) SELECT * FROM empty_myisam
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
DROP TABLE IF EXISTS t1;

DROP TABLE empty_smartengine, empty_myisam, nonempty_smartengine, nonempty_myisam;

SET SQL_LOG_BIN = 1;
