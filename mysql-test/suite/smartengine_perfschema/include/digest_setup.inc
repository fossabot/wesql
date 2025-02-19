# Making sure not to run when ps-protocol is set.
# Test requires: sp-protocol/ps-protocol/view-protocol/cursor-protocol disabled
--source include/no_protocol.inc

--echo ####################################
--echo # SETUP
--echo ####################################
# Database setup
--disable_warnings
CREATE DATABASE statements_digest;
--enable_warnings
USE statements_digest;

--disable_query_log
SET DEFAULT_STORAGE_ENGINE=SMARTENGINE;
--enable_query_log

# Table set up for queries
--disable_warnings
CREATE TABLE t1(a int);
CREATE TABLE t2(a int);
CREATE TABLE t3(a int, b int);
CREATE TABLE t4(a int, b int);
CREATE TABLE t5(a int, b int, c int);
CREATE TABLE t6(a int, b int, c int, d int);
CREATE TABLE t11 (c CHAR(4));
CREATE TABLE t12 (c CHAR(4));
--enable_warnings

