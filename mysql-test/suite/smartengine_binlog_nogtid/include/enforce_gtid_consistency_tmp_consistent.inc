# copied from extra/binlog_tests/enforce_gtid_consistency_tmp_consistent.test
--let $error_code= 0
--let $statement_ends_transaction= 1
--let $gtid_violation= 0
--let $sync_point= before_execute_sql_command

# When CREATE TEMPORARY and DROP TEMPORARY occur *outside* a
# transaction, it is GTID-consistent.

--echo ---- CREATE TEMPORARY outside trx ----
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) ENGINE = SMARTENGINE
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc

--echo ---- DROP TEMPORARY outside trx ----
--let $statement= DROP TEMPORARY TABLE t1
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc

# ALTER is always GTID-consistent. In fact, ALTER TEMPORARY does not
# exist.  ALTER without the TEMPORARY keyword can operate on a
# temporary table, but even then it has an implicit commit.

if ($gtid_next != GTID)
{
  --echo ---- ALTER without TEMPORARY, on temp table, in trx ----

  # If gtid_next=GTID, it generates
  # ER_CANT_DO_IMPLICIT_COMMIT_IN_TRX_WHEN_GTID_NEXT_IS_SET.
  CREATE TEMPORARY TABLE t1 (a INT) ENGINE = SMARTENGINE;
  --let $pre_statement= BEGIN
  --let $statement= ALTER TABLE t1 ADD COLUMN (b INT)
  --source extra/binlog_tests/enforce_gtid_consistency_statement.inc

  # DROP without TEMPORARY is always GTID-consistent, since it has an
  # implicit commit even if it operates on a temporary table.

  --echo ---- DROP without TEMPORARY, on temp table ----

  --let $pre_statement= BEGIN
  --let $statement= DROP TABLE t1
  --source extra/binlog_tests/enforce_gtid_consistency_statement.inc

  --let $gtid_violation= 0
  DROP TABLE IF EXISTS t1; # May still exist if gtid_violation=1.

  # When SQL_LOG_BIN=0, it is GTID-consistent since nothing is logged.
}

SET SQL_LOG_BIN = 0;

--echo ---- CREATE TEMPORARY in trx, SQL_LOG_BIN=0 (SMARTENGINE, BEGIN) ----
--let $pre_statement= BEGIN
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) ENGINE = SMARTENGINE
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
COMMIT;
DROP TEMPORARY TABLE t1;

--echo ---- CREATE TEMPORARY in trx, SQL_LOG_BIN=0 (MyISAM, BEGIN) ----
--let $pre_statement= BEGIN
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) ENGINE = MyISAM
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
COMMIT;
DROP TEMPORARY TABLE t1;

SET AUTOCOMMIT = 0;

--echo ---- CREATE TEMPORARY in trx, SQL_LOG_BIN=0 (SMARTENGINE, AUTOCOMMIT=0) ----
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) ENGINE = SMARTENGINE
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
COMMIT;
DROP TEMPORARY TABLE t1;
COMMIT;

--echo ---- CREATE TEMPORARY in trx, SQL_LOG_BIN=0 (MyISAM, AUTOCOMMIT=0) ----
--let $statement= CREATE TEMPORARY TABLE t1 (a INT) ENGINE = MyISAM
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
COMMIT;
DROP TEMPORARY TABLE t1;

SET AUTOCOMMIT = 1;

--echo ---- DROP TEMPORARY in trx, SQL_LOG_BIN=0 (SMARTENGINE, BEGIN) ----
CREATE TEMPORARY TABLE t1 (a INT) ENGINE = SMARTENGINE;
--let $pre_statement= BEGIN
--let $statement= DROP TEMPORARY TABLE t1
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
COMMIT;

--echo ---- DROP TEMPORARY in trx, SQL_LOG_BIN=0 (MyISAM, BEGIN) ----
CREATE TEMPORARY TABLE t1 (a INT) ENGINE = MyISAM;
--let $pre_statement= BEGIN
--let $statement= DROP TEMPORARY TABLE t1
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
COMMIT;

SET AUTOCOMMIT = 0;

--echo ---- DROP TEMPORARY in trx, SQL_LOG_BIN=0 (SMARTENGINE, AUTOCOMMIT=0) ----
CREATE TEMPORARY TABLE t1 (a INT) ENGINE = SMARTENGINE;
COMMIT;
--let $statement= DROP TEMPORARY TABLE t1
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
COMMIT;

--echo ---- DROP TEMPORARY in trx, SQL_LOG_BIN=0 (MyISAM, AUTOCOMMIT=0) ----
CREATE TEMPORARY TABLE t1 (a INT) ENGINE = MyISAM;
COMMIT;
--let $statement= DROP TEMPORARY TABLE t1
--source extra/binlog_tests/enforce_gtid_consistency_statement.inc
COMMIT;

SET AUTOCOMMIT = 1;

SET SQL_LOG_BIN = 1;
