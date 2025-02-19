
# Creating tables
--disable_warnings
drop table if exists t1, t2;
--enable_warnings

create table t1 (a int) ENGINE = SMARTENGINE;
create table t2 (a int) engine=myisam;

#
# This test checks binlog_cache_use and binlog_cache_disk_use when
# transactions are committed and after when they are aborted.
#

#
# Checking commit.
#
--echo **** Preparing the enviroment to check commit and its effect on status variables.
--echo **** Expected: binlog_cache_use = 0, binlog_cache_disk_use = 0.
--echo **** Expected: binlog_stmt_cache_use = 1, binlog_stmt_cache_disk_use = 0.
flush status;
let $exp_cache= 0;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 0;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 1;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}

--echo **** Transactional changes which are long enough so they will be flushed to disk...
--echo **** Expected: binlog_cache_use = 1, binlog_cache_disk_use = 1.
--echo **** Expected: binlog_stmt_cache_use = 1, binlog_stmt_cache_disk_use = 0.
let $1=2000;
disable_query_log;
begin;
while ($1)
{
 eval insert into t1 values( $1 );
 dec $1;
}
commit;
enable_query_log;
let $exp_cache= 1;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 1;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 1;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}

--echo **** Transactional changes which should not be flushed to disk and so should not
--echo **** increase either binlog_cache_disk_use or binlog_stmt_cache_disk_use.
--echo **** Expected: binlog_cache_use = 2, binlog_cache_disk_use = 1.
--echo **** Expected: binlog_stmt_cache_use = 1, binlog_stmt_cache_disk_use = 0.
begin;
insert into t1 values( 1 );
commit;
let $exp_cache= 2;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 1;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 1;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}

--echo **** Non-Transactional changes which should not be flushed to disk and so should not
--echo **** increase either binlog_cache_disk_use or binlog_stmt_cache_disk_use.
--echo **** Expected: binlog_cache_use = 2, binlog_cache_disk_use = 1.
--echo **** Expected: binlog_stmt_cache_use = 2, binlog_stmt_cache_disk_use = 0.
begin;
insert into t2 values( 1 );
commit;
let $exp_cache= 2;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 1;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 2;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}

--echo **** Mixed changes which should not be flushed to disk and so should not
--echo **** increase either binlog_cache_disk_use or binlog_stmt_cache_disk_use.
--echo **** Expected: binlog_cache_use = 3, binlog_cache_disk_use = 1.
--echo **** Expected: binlog_stmt_cache_use = 3, binlog_stmt_cache_disk_use = 0.
begin;
insert into t1 values( 1 );
insert into t2 values( 1 );
commit;
let $exp_cache= 3;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 1;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 3;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}

#
# Checking abort.
#
--echo **** Preparing the enviroment to check abort and its effect on the status variables.
--echo **** Expected: binlog_cache_use = 0, binlog_cache_disk_use = 0.
--echo **** Expected: binlog_stmt_cache_use = 1, binlog_stmt_cache_disk_use = 0.
flush status;
let $exp_cache= 0;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 0;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 1;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}

--echo **** Transactional changes which are long enough so they will be flushed to disk...
--echo **** Expected: binlog_cache_use = 1, binlog_cache_disk_use = 1.
--echo **** Expected: binlog_stmt_cache_use = 1, binlog_stmt_cache_disk_use = 0.
let $1=2000;
disable_query_log;
begin;
while ($1)
{
 eval insert into t1 values( $1 );
 dec $1;
}
rollback;
enable_query_log;
let $exp_cache= 1;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 1;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 1;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}

--echo **** Transactional changes which should not be flushed to disk and so should not
--echo **** increase either binlog_cache_disk_use or binlog_stmt_cache_disk_use.
--echo **** Expected: binlog_cache_use = 2, binlog_cache_disk_use = 1.
--echo **** Expected: binlog_stmt_cache_use = 1, binlog_stmt_cache_disk_use = 0.
begin;
insert into t1 values( 1 );
rollback;
let $exp_cache= 2;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 1;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 1;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}

--echo **** Non-Transactional changes which should not be flushed to disk and so should not
--echo **** increase either binlog_cache_disk_use or binlog_stmt_cache_disk_use.
--echo **** Expected: binlog_cache_use = 2, binlog_cache_disk_use = 1.
--echo **** Expected: binlog_stmt_cache_use = 2, binlog_stmt_cache_disk_use = 0.
begin;
insert into t2 values( 1 );
rollback;
let $exp_cache= 2;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 1;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 2;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}

--echo **** Mixed changes which should not be flushed to disk and so should not
--echo **** increase either binlog_cache_disk_use or binlog_stmt_cache_disk_use.
--echo **** Expected: binlog_cache_use = 3, binlog_cache_disk_use = 1.
--echo **** Expected: binlog_stmt_cache_use = 3, binlog_stmt_cache_disk_use = 0.
begin;
insert into t1 values( 1 );
insert into t2 values( 1 );
rollback;
let $exp_cache= 3;
let $got_cache= query_get_value(show status like "binlog_cache_use", Value, 1);
let $exp_disk= 1;
let $got_disk= query_get_value(show status like "binlog_cache_disk_use", Value, 1);
let $exp_stmt_cache= 3;
let $got_stmt_cache= query_get_value(show status like "binlog_stmt_cache_use", Value, 1);
let $exp_stmt_disk= 0;
let $got_stmt_disk= query_get_value(show status like "binlog_stmt_cache_disk_use", Value, 1);
if (`SELECT $got_cache <> $exp_cache || $got_disk <> $exp_disk || $got_stmt_cache <> $exp_stmt_cache || $got_stmt_disk <> $exp_stmt_disk`)
{
  -- echo "Expected: binlog_cache_use = $exp_cache, binlog_cache_disk_use = $exp_disk but got binlog_cache_use = $got_cache, binlog_cache_disk_use = $got_disk"
  -- echo "Expected: binlog_stmt_cache_use = $exp_stmt_cache, binlog_stmt_cache_disk_use = $exp_stmt_disk but got binlog_stmt_cache_use = $got_stmt_cache, binlog_stmt_cache_disk_use = $got_stmt_disk"
  -- die
}
drop table t1, t2;

