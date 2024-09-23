use mysql;


-- ------------------------------------------------------------------------
-- Following are set of "configuration tables" that used to configure
-- the Consensus replication.
-- ------------------------------------------------------------------------
set @have_innodb= (select count(engine) from information_schema.engines where engine='INNODB' and support != 'NO');
set @is_mysql_encrypted = (select ENCRYPTION from information_schema.INNODB_TABLESPACES where NAME='mysql');

-- Tables below are NOT treated as DD tables by MySQL server yet.

SET FOREIGN_KEY_CHECKS= 1;

# Added sql_mode elements and making it as SET, instead of ENUM

set default_storage_engine=InnoDB;

-- Consensus replication

SET @have_consensus_replication= (select count(plugin_name) from information_schema.plugins where plugin_name='consensus_replication');
SET @cmd = "CREATE TABLE IF NOT EXISTS consensus_info (
  number_of_lines INTEGER UNSIGNED NOT NULL COMMENT 'Number of lines in the file or rows in the table. Used to version table definitions.',
  vote_for BIGINT UNSIGNED COMMENT 'current vote for', current_term BIGINT UNSIGNED COMMENT 'current term',
  recover_status BIGINT UNSIGNED COMMENT 'recover status', last_leader_term BIGINT UNSIGNED COMMENT 'last leader term',
  start_apply_index BIGINT UNSIGNED COMMENT 'start apply index', cluster_id BIGINT UNSIGNED COMMENT 'cluster identifier',
  cluster_info varchar(6000) COMMENT 'cluster config information', cluster_learner_info varchar(6000) COMMENT 'cluster learner config information',
  cluster_config_recover_index BIGINT UNSIGNED COMMENT 'cluster config recover index',
  PRIMARY KEY(number_of_lines)) DEFAULT CHARSET=utf8 COMMENT 'Consensus cluster consensus information'";
SET @str = IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
SET @str = IF(@have_consensus_replication = 1, @str, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS consensus_applier_worker (
  Number_of_lines INTEGER UNSIGNED NOT NULL COMMENT 'Number of lines in the file.',
  Id INTEGER UNSIGNED NOT NULL,
  Consensus_apply_index BIGINT UNSIGNED NOT  NULL COMMENT 'The consensus log applyed index in the consensus log',
  PRIMARY KEY(Id)) DEFAULT CHARSET=utf8 STATS_PERSISTENT=0 COMMENT 'Consensus applier Worker Information'";
SET @str = IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
SET @str = IF(@have_consensus_replication = 1, @str, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;

SET @cmd = "CREATE TABLE IF NOT EXISTS consensus_applier_info (
  Number_of_lines INTEGER UNSIGNED NOT NULL COMMENT 'Number of lines in the file.',
  Number_of_workers INTEGER UNSIGNED,
  Consensus_apply_index BIGINT UNSIGNED NOT  NULL COMMENT 'The consensus log applyed index in the consensus log',
  PRIMARY KEY(number_of_lines)) DEFAULT CHARSET=utf8 STATS_PERSISTENT=0 COMMENT 'Consensus Log Information'";
SET @str = IF(@have_innodb <> 0, CONCAT(@cmd, ' ENGINE= INNODB ROW_FORMAT=DYNAMIC TABLESPACE=mysql ENCRYPTION=\'', @is_mysql_encrypted,'\''), CONCAT(@cmd, ' ENGINE= MYISAM'));
SET @str = IF(@have_consensus_replication = 1, @str, 'SET @dummy = 0');
PREPARE stmt FROM @str;
EXECUTE stmt;
DROP PREPARE stmt;
