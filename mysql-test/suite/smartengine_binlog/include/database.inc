# copied extra/binlog_tests/database.test;
source include/have_log_bin.inc;
source include/have_debug.inc;

# Checking that the drop of a database does not replicate anything in
# addition to the drop of the database

reset master;
create database testing_1;
use testing_1;
create table t1 (a int);
create function sf1 (a int) returns int return a+1;
create trigger tr1 before insert on t1 for each row insert into t2 values (2*new.a);
create procedure sp1 (a int) insert into t1 values(a);
drop database testing_1;
--let $mask_binlog_commit_events= 1
source include/show_binlog_events.inc;
--let $mask_binlog_commit_events= 0

# BUG#38773: DROP DATABASE cause switch to stmt-mode when there are
# temporary tables open

use test;
reset master;
create temporary table tt1 (a int);
create table t1 (a int);
insert into t1 values (1);
disable_warnings;
drop database if exists mysqltest1;
enable_warnings;
insert into t1 values (1);
drop table tt1, t1;
--let $mask_binlog_commit_events= 1
source include/show_binlog_events.inc;
--let $mask_binlog_commit_events= 0

FLUSH STATUS;

--echo
--echo # 'DROP TABLE IF EXISTS <deleted table>' are binlogged
--echo # for each table in SEs not supporting atomic DDL which
--echo # were dropped by failed 'DROP DATABASE'.
--echo #
--echo # Unlike test for bug#11765416/58381 this test uses MyISAM
--echo # for t1. So failed DROP DATABASE will have side-effect even
--echo # though removal of InnoDB tables is rolled back.
--echo # DROP TABLE IF EXISTS is logged for MyISAM table in this case.
RESET MASTER;
CREATE DATABASE testing_1;
USE testing_1;
CREATE TABLE t1(c1 INT) ENGINE=MyISAM;
CREATE TABLE t2(c1 INT PRIMARY KEY) ENGINE=SMARTENGINE;

--echo #
--echo # 'DROP DATABASE' will fail but will delete table t1.
--echo #

SET SESSION DEBUG='+d,rm_table_no_locks_abort_after_atomic_tables';
--error ER_UNKNOWN_ERROR
DROP DATABASE testing_1;
SET SESSION DEBUG='-d,rm_table_no_locks_abort_after_atomic_tables';
let $wait_binlog_event= DROP TABLE IF EXIST;
source include/wait_for_binlog_event.inc;
let $MYSQLD_DATADIR= `SELECT @@datadir`;

--echo # Cleanup.
DROP DATABASE testing_1;


--echo #
--echo # Bug#11765416 58381: FAILED DROP DATABASE CAN BREAK STATEMENT
--echo #              BASED REPLICATION
--echo #

USE test;
--disable_warnings
DROP DATABASE IF EXISTS db1;
DROP TABLE IF EXISTS t3;
--enable_warnings

CREATE DATABASE db1;
CREATE TABLE db1.t1 (a INT) ENGINE = SMARTENGINE;
CREATE TABLE db1.t2 (b INT, KEY(b)) ENGINE = SMARTENGINE;
RESET MASTER;

SET SESSION DEBUG='+d,rm_table_no_locks_abort_after_atomic_tables';
--error ER_UNKNOWN_ERROR
DROP DATABASE db1;
SET SESSION DEBUG='-d,rm_table_no_locks_abort_after_atomic_tables';
# Both t1 and t2 remain as the whole statement is rolled back.
SHOW TABLES FROM db1;
--let $mask_binlog_commit_events= 1
--source include/show_binlog_events.inc # Check that the binlog is empty.
--let $mask_binlog_commit_events= 0

# Cleanup
DROP DATABASE db1;
RESET MASTER;
