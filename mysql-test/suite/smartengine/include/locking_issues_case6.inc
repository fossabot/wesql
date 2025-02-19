#
# Check concurrent locking issues:
#   Changing primary key
#
# To call this, set $isolation_level and call this file
#
# let $isolation_level = REPEATABLE READ;
# --source suite/smartengine/include/locking_issues_case6.inc
#

--echo
--echo -----------------------------------------------------------------------
--echo - Locking issues case 6:
--echo -   Changing primary key
--echo -   using $isolation_level transaction isolation level
--echo -----------------------------------------------------------------------

--disable_warnings
DROP TABLE IF EXISTS t0;
--enable_warnings

CREATE TABLE t0(id INT AUTO_INCREMENT PRIMARY KEY, value INT) ENGINE=SMARTENGINE;

# Insert 200,000 rows, breaking it up into inserts of 1000 rows at a time
--echo Inserting 200,000 rows
--disable_query_log
#SET @save_smartengine_bulk_load=@@smartengine_bulk_load;
#SET smartengine_bulk_load=1;
#SET @save_smartengine_write_disable_wal=@@smartengine_write_disable_wal;
#SET GLOBAL smartengine_write_disable_wal=1;
let $i = 1;
while ($i <= 200) {
  eval BEGIN;
  let $j = 1;
  while ($j <= 100) {
    eval INSERT INTO t0(value) VALUES (0),(0),(0),(0),(0),(0),(0),(0),(0),(0);
    inc $j;
  }
  eval COMMIT;
  inc $i;
}
#SET smartengine_bulk_load=@save_smartengine_bulk_load;
#SET GLOBAL smartengine_write_disable_wal=@save_smartengine_write_disable_wal;
--enable_query_log

UPDATE t0 SET value=100 WHERE id=190000;

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;
let $ID = `SELECT connection_id()`;
send SELECT * FROM t0 WHERE value > 0 FOR UPDATE;

connection con2;
let $wait_condition = SELECT 1 FROM information_schema.processlist
                      WHERE id = $ID AND state = "Sending data";
--source include/wait_condition.inc
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;
UPDATE t0 SET id=200001 WHERE id=190000;
COMMIT;

connection con1;
--error 0,ER_LOCK_DEADLOCK
reap;
--echo ERROR: $mysql_errno
COMMIT;

connection default;
disconnect con1;
disconnect con2;

DROP TABLE t0;
