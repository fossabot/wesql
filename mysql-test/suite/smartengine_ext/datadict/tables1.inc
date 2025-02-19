# suite/smartengine_ext/datadict/tables1.inc
#
# Auxiliary script to be sourced by
#    is_tables_mysql.test
#    is_tables_is.test
#
# Author:
# 2008-01-23 mleich WL#4203 Reorganize and fix the data dictionary tests of
#                           testsuite smartengine_ext
#                   Create this script based on older scripts and new code.
#

--disable_warnings
DROP DATABASE IF EXISTS test1;
--enable_warnings
CREATE DATABASE test1;

--source suite/smartengine_ext/datadict/tables2.inc

# Create a low privileged user.
# Note: The database test1 is just a "home" for the low privileged user
#       and not in the focus of testing.
--error 0,ER_CANNOT_USER
DROP   USER testuser1@localhost;
CREATE USER testuser1@localhost;
GRANT SELECT ON test1.* TO testuser1@localhost;

--echo # Establish connection testuser1 (user=testuser1)
--replace_result $MASTER_MYPORT MYSQL_PORT $MASTER_MYSOCK MYSQL_SOCK
connect (testuser1,localhost,testuser1,,test1);
--source suite/smartengine_ext/datadict/tables2.inc

--echo # Switch to connection default and close connection testuser1
connection default;
disconnect testuser1;
DROP USER testuser1@localhost;
DROP DATABASE test1;
