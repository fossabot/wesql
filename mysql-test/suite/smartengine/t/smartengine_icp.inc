#
# Testing Index Condition Pushdown for MyRocks
#  Test file parameter:  $cf_name  specifies the CF to store test data in
#                                  It can be forward or reverse-ordered CF
#
select * from information_schema.engines where ENGINE = 'SMARTENGINE';

--disable_warnings
drop table if exists t0,t1,t2,t3;
--enable_warnings
create table t0 (a int) engine=innodb;
insert into t0 values (0),(1),(2),(3),(4),(5),(6),(7),(8),(9);

create table t1(a int) engine=innodb;
insert into t1 select A.a + B.a* 10 + C.a * 100 from t0 A, t0 B, t0 C;

eval
create table t2 (
  pk int primary key,
  kp1 int,
  kp2 int,
  col1 int,
  key (kp1,kp2) comment '$cf_name'
) ENGINE = SMARTENGINE;

insert into t2 select a,a,a,a from t1;

--echo # Try a basic case:
--replace_column 10 #
explain
select * from t2 where kp1 between 1 and 10 and mod(kp2,2)=0;
select * from t2 where kp1 between 1 and 10 and mod(kp2,2)=0;

--echo # Check that ICP doesnt work for columns where column value
--echo # cant be restored from mem-comparable form:

eval
create table t3 (
  pk int primary key,
  kp1 int,
  kp2 varchar(10) collate utf8_general_ci,
  col1 int,
  key (kp1,kp2) comment '$cf_name'
) ENGINE = SMARTENGINE;

--echo # This must not use ICP:
--replace_column 10 #
explain
select * from t3 where kp1=3 and kp2 like '%foo%';
select * from t3 where kp1=3 and kp2 like '%foo%';

--replace_column 10 #
explain format=json
select * from t3 where kp1 between 2 and 4 and mod(kp1,3)=0 and kp2 like '%foo%';

--echo # Check that we handle the case where out-of-range is encountered sooner
--echo # than matched index condition
--replace_column 10 #
explain
select * from t2 where kp1< 3 and kp2+1>50000;
select * from t2 where kp1< 3 and kp2+1>50000;

--replace_column 10 #
explain
select * from t2 where kp1< 3 and kp2+1>50000;
select * from t2 where kp1< 3 and kp2+1>50000;

--echo # Try doing backwards scans
--replace_column 10 #
explain
select * from t2 where kp1 between 1 and 10 and mod(kp2,2)=0 order by kp1 desc;
select * from t2 where kp1 between 1 and 10 and mod(kp2,2)=0 order by kp1 desc;

--replace_column 10 #
explain
select * from t2 where kp1 >990 and mod(kp2,2)=0 order by kp1 desc;
select * from t2 where kp1 >990 and mod(kp2,2)=0 order by kp1 desc;

--replace_column 10 #
explain
select * from t2 where kp1< 3 and kp2+1>50000 order by kp1 desc;
select * from t2 where kp1< 3 and kp2+1>50000 order by kp1 desc;

drop table t0,t1,t2,t3;

--echo #
--echo # Check how ICP affects counters
--echo #
--echo # First, some preparations
--echo #

eval
create table t4 (
  id int,
  id1 int,
  id2 int,
  value int,
  value2 varchar(100),
  primary key (id),
  key id1_id2 (id1, id2) comment '$cf_name'
) ENGINE = SMARTENGINE charset=latin1 collate latin1_bin;

insert into t4 values
(1,1,1,1,1), (2,1,2,2,2), (3,1,3,3,3),(4,1,4,4,4),(5,1,5,5,5),
(6,1,6,6,6), (7,1,7,7,7), (8,1,8,8,8),(9,1,9,9,9),(10,1,10,10,10);

--echo #
--echo # Now, the test itself
--echo #

--echo # ============== index-only query ==============
--replace_column 10 #
explain
select id1,id2 from t4 force index (id1_id2) where id1=1 and id2 % 10 = 1;
select id1,id2 from t4 force index (id1_id2) where id1=1 and id2 % 10 = 1;

--echo # ============== Query without ICP ==============
set optimizer_switch='index_condition_pushdown=off';
--replace_column 10 #
explain
select * from t4 force index (id1_id2) where id1=1 and id2 % 10 = 1;
select * from t4 force index (id1_id2) where id1=1 and id2 % 10 = 1;

--echo # ============== Query with ICP ==============
set optimizer_switch='index_condition_pushdown=on';
--replace_column 10 #
explain
select * from t4 force index (id1_id2) where id1=1 and id2 % 10 = 1;
select * from t4 force index (id1_id2) where id1=1 and id2 % 10 = 1;

drop table t4;
