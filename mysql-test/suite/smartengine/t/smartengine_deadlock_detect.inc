#
# Some basic sanity tests for deadlock detection.
#
--source suite/smartengine/include/have_smartengine.inc

set @prior_smartengine_lock_wait_timeout = @@smartengine_lock_wait_timeout;
set @prior_smartengine_deadlock_detect = @@smartengine_deadlock_detect;
set global smartengine_lock_wait_timeout = 100000;
set global smartengine_deadlock_detect = ON;

create table t (i int primary key);
create table r1 (id int primary key, value int);
insert into r1 values (1,1),(2,2),(3,3),(4,4),(5,5),(6,6),(7,7),(8,8),(9,9),(10,10);
create table r2 like r1;
insert into r2 select * from r1;

# deadlock on scanned locking reads
connect (con1,localhost,root,,);
let $con1= `SELECT CONNECTION_ID()`;
begin;
update r2 set value=100 where id=9;

connect (con2,localhost,root,,);
let $con2= `SELECT CONNECTION_ID()`;
begin;
update r1 set value=100 where id=8;
--send select * from r2 for update;

connection con1;
let $wait_condition =
`SELECT CONCAT('select count(*) = 1 from information_schema.SMARTENGINE_TRX where THREAD_ID = ', '$con2', ' and WAITING_KEY != ""')`;
--source include/wait_condition.inc
--error ER_LOCK_DEADLOCK
select * from r1 for update;
rollback;

connection con2;
--reap;
rollback;

connection con1;
begin;
insert into t values (1);

connection con2;
begin;
insert into t values (2);

connect (con3,localhost,root,,);
begin;
insert into t values (3);

connection con1;
--send select * from t where i = 2 for update

connection con2;
let $wait_condition =
`SELECT CONCAT('select count(*) = 1 from information_schema.SMARTENGINE_TRX where THREAD_ID = ', '$con1', ' and WAITING_KEY != ""')`;
--source include/wait_condition.inc

--send select * from t where i = 3 for update

connection con3;
let $wait_condition =
`SELECT CONCAT('select count(*) = 1 from information_schema.SMARTENGINE_TRX where THREAD_ID = ', '$con2', ' and WAITING_KEY != ""')`;
--source include/wait_condition.inc

select * from t;
--error ER_LOCK_DEADLOCK
insert into t values (4), (1);
--echo # Transaction should be rolled back
select * from t;
rollback;

connection con2;
--reap
rollback;

connection con1;
--reap
rollback;

connection default;
disconnect con1;
disconnect con2;
disconnect con3;

set global smartengine_lock_wait_timeout = @prior_smartengine_lock_wait_timeout;
set global smartengine_deadlock_detect = @prior_smartengine_deadlock_detect;
drop table t,r1,r2;
