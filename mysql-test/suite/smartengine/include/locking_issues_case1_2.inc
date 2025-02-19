#
# Check concurrent locking issues:
#   Locking rows that do not exist without using all primary key columns in a
#   WHERE clause
#
# To call this, set $isolation_level and call this file
#
# let $isolation_level = REPEATABLE READ;
# --source suite/smartengine/include/locking_issues_case1_2.inc
#

--echo
--echo -----------------------------------------------------------------------
--echo - Locking issues case 1.2:
--echo -   Locking rows that do not exist without using all primary key
--echo -   columns in a WHERE clause
--echo -   using $isolation_level transaction isolation level
--echo -----------------------------------------------------------------------

--disable_warnings
DROP TABLE IF EXISTS t0;
--enable_warnings

CREATE TABLE t0(id1 INT, id2 INT, value INT, PRIMARY KEY(id1, id2)) ENGINE=SMARTENGINE;
INSERT INTO t0 VALUES (1,1,0), (3,3,0), (4,4,0), (6,6,0);

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;
SELECT * FROM t0 WHERE id1=1 FOR UPDATE;

connection con2;
eval SET SESSION TRANSACTION ISOLATION LEVEL $isolation_level;
BEGIN;
SELECT * FROM t0 WHERE id1=1 AND id2=4 FOR UPDATE;
INSERT INTO t0 VALUES (1,5,0);

connection con1;
COMMIT;

connection default;
disconnect con1;
disconnect con2;

DROP TABLE t0;
