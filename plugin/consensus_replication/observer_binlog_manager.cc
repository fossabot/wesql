
#include <stddef.h>

#include "consensus_applier.h"
#include "consensus_binlog.h"
#include "consensus_binlog_recovery.h"
#include "consensus_log_manager.h"
#include "observer_binlog_manager.h"
#include "plugin.h"

#include "my_loglevel.h"
#include "mysql/components/services/log_builtins.h"
#include "sql/debug_sync.h"  // DEBUG_SYNC
#include "sql/log.h"
#include "sql/rpl_commit_stage_manager.h"
#include "sql/sql_lex.h"

static int consensus_binlog_manager_binlog_recovery(
    Binlog_manager_param *param) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (!opt_bin_log || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  if (build_consensus_log_index(param->binlog)) return 1;

  assert(param->binlog == consensus_log_manager.get_binlog());

  return opt_initialize ? 0 : consensus_binlog_recovery(param->binlog);
}

static int consensus_binlog_manager_after_binlog_recovery(
    Binlog_manager_param *param) {
  DBUG_TRACE;
  int error = 0;

  /* If the plugin is not enabled, return success. */
  if (!opt_bin_log || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  assert(param->binlog == consensus_log_manager.get_binlog());

  /* Open a new binlog file if not exits */
  std::vector<std::string> binlog_file_list;
  consensus_log_manager.get_log_file_index()->get_log_file_list(
      binlog_file_list);

  mysql_mutex_t *log_lock = param->binlog->get_log_lock();
  mysql_mutex_lock(log_lock);

  if (binlog_file_list.empty()) {
    if (param->binlog->open_binlog(opt_bin_logname, nullptr, max_binlog_size,
                                   false, true /*need_lock_index=true*/,
                                   true /*need_sid_lock=true*/, nullptr)) {
      error = 1;
    }
    consensus_log_manager.set_start_without_log(true);
  } else {
    if (param->binlog->open_exist_consensus_binlog(
            opt_bin_logname, max_binlog_size, false,
            true /*need_lock_index=true*/)) {
      error = 1;
    }
  }

  mysql_mutex_unlock(log_lock);

  return error;
}

static int consensus_binlog_manager_gtid_recovery(Binlog_manager_param *) {
  /* gtid recovery in hook server_state.after_engine_recovery */
  return 0;
}

static int consensus_binlog_manager_new_file(
    Binlog_manager_param *param, const char *log_file_name,
    bool null_created_arg, bool need_sid_lock,
    Format_description_log_event *extra_description_event [[maybe_unused]],
    bool &write_file_name_to_index_file) {
  DBUG_TRACE;
  int error = 0;
  uint32 tv_sec = 0;

  /* If the plugin is not enabled, return success. */
  if (!opt_bin_log || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  mysql_mutex_assert_owner(param->binlog->get_log_lock());

  if (param->binlog->is_consensus_write)
    tv_sec = consensus_log_manager.get_event_timestamp();

  error = param->binlog->init_consensus_binlog(
      null_created_arg, need_sid_lock, write_file_name_to_index_file,
      consensus_log_manager.get_current_index(), tv_sec);

  if (!error) {
    if (!param->binlog->is_consensus_write)
      consensus_log_manager.set_first_event_in_file(true);

    std::string file_name(log_file_name);
    consensus_log_manager.get_log_file_index()->add_to_index_list(
        consensus_log_manager.get_current_index(), tv_sec, file_name);
  }

  return error;
}

static int consensus_binlog_manager_after_purge_file(
    Binlog_manager_param *param, const char *log_file_name) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  return consensus_binlog_after_purge_file(param->binlog, log_file_name);
}

static int consensus_binlog_manager_before_flush(Binlog_manager_param *param) {
  DBUG_TRACE;
  int error = 0;

  /* If the plugin is not enabled, before commit should return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  THD *thd = param->thd;
  mysql_rwlock_rdlock(consensus_log_manager.get_consensuslog_status_lock());

  /* Check server status */
  mysql_mutex_lock(consensus_log_manager.get_log_term_lock());
  if (consensus_log_manager.get_status() !=
          Consensus_Log_System_Status::BINLOG_WORKING ||
      thd->consensus_context.consensus_term !=
          consensus_log_manager.get_current_term() ||
      rpl_consensus_log_get_term() !=
          consensus_log_manager.get_current_term() ||
      rpl_consensus_get_term() != consensus_log_manager.get_current_term()) {
    error = 1;
  }
  mysql_mutex_unlock(consensus_log_manager.get_log_term_lock());

  if (error) {
    mysql_rwlock_unlock(consensus_log_manager.get_consensuslog_status_lock());
    thd->mark_transaction_to_rollback(true);
    thd->commit_error = THD::CE_COMMIT_ERROR;
    my_error(ER_BINLOG_LOGGING_IMPOSSIBLE, MYF(0), "Consensus Not Leader");
  } else {
    // consensuslog_status_lock got
    thd->consensus_context.status_locked = true;
  }

  return error;
}

static int consensus_binlog_manager_write_transaction(
    Binlog_manager_param *param, Gtid_log_event *gtid_event,
    binlog_cache_data *cache_data, bool have_checksum) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  return write_cache_to_consensus_log(param->thd, param->binlog, *gtid_event,
                                      cache_data, have_checksum);
}

static int consensus_binlog_manager_after_write(Binlog_manager_param *) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  rpl_consensus_write_log_cache_done();

  DBUG_EXECUTE_IF("consensus_replication_crash_after_write_cache",
                  { DBUG_SUICIDE(); });

  return 0;
}

static int consensus_binlog_manager_after_flush(Binlog_manager_param *param,
                                                bool &delay_update_binlog_pos) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  delay_update_binlog_pos = true;
  return 0;
}

static int consensus_binlog_manager_after_sync(Binlog_manager_param *param,
                                               bool &delay_update_binlog_pos) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  delay_update_binlog_pos = true;
  return 0;
}

static int consensus_binlog_manager_after_enrolling_stage(
    Binlog_manager_param *param, int stage) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  /* No action is required for other stages */
  if (Commit_stage_manager::StageID(stage) !=
      Commit_stage_manager::COMMIT_STAGE)
    return 0;

  DEBUG_SYNC(param->thd, "consensus_replication_after_sync");
  DBUG_EXECUTE_IF("consensus_replication_crash_after_sync", { DBUG_SUICIDE(); });

  uint64 group_max_log_index = 0;
  for (THD *head = param->thd; head; head = head->next_to_commit) {
    if (head->consensus_context.consensus_index > group_max_log_index) {
      group_max_log_index = head->consensus_context.consensus_index;
    }
  }

  /* The group log may potentially be truncated. */
  mysql_mutex_lock(consensus_log_manager.get_consensuslog_truncate_lock());
  if (group_max_log_index > 0 &&
      group_max_log_index < consensus_log_manager.get_current_index()) {
    consensus_log_manager.set_sync_index_if_greater(group_max_log_index);
  }
  mysql_mutex_unlock(consensus_log_manager.get_consensuslog_truncate_lock());

  /* The group log may potentially be truncated. If not,
   * group_max_log_index is not more than sync_index. */
  if (group_max_log_index > 0 &&
      group_max_log_index <= consensus_log_manager.get_sync_index()) {
    if (!opt_initialize) rpl_consensus_write_log_done(group_max_log_index);
  }

  for (THD *head = param->thd; head; head = head->next_to_commit) {
    /* whether consensus layer allow to commit */
    if (head->consensus_context.consensus_error ==
        Consensus_binlog_context_info::CSS_NONE) {
      consensus_before_commit(head);
    }

    /* updates the binlog end position */
    if (head->consensus_context.consensus_error ==
        Consensus_binlog_context_info::CSS_NONE) {
      const char *binlog_file = nullptr;
      my_off_t pos = 0;

      head->get_trans_fixed_pos(&binlog_file, &pos);
      if (binlog_file != nullptr && pos > 0) {
        param->binlog->update_binlog_end_pos(binlog_file, pos);
      }
    }
  }

  /* after waitCommitIndexUpdate */
  DEBUG_SYNC(param->thd, "consensus_replication_after_consensus_commit");
  DBUG_EXECUTE_IF("consensus_replication_crash_after_commit",
                  { DBUG_SUICIDE(); });

  // Release consensus log lock for leader
  mysql_rwlock_rdlock(consensus_log_manager.get_consensuslog_commit_lock());
  param->thd->consensus_context.commit_locked = true;
  assert(param->thd->consensus_context.status_locked);
  mysql_rwlock_unlock(consensus_log_manager.get_consensuslog_status_lock());
  param->thd->consensus_context.status_locked = false;

  return 0;
}

static int consensus_binlog_manager_before_finish_in_engines(
    Binlog_manager_param *param, bool finish_commit) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  THD *thd = param->thd;

  // Consensus log lock has not released
  if (finish_commit && thd->consensus_context.status_locked) {
    mysql_rwlock_unlock(consensus_log_manager.get_consensuslog_status_lock());
    thd->consensus_context.status_locked = false;
  }

  if (thd->commit_error != THD::CE_COMMIT_ERROR &&
      thd->consensus_context.consensus_error !=
          Consensus_binlog_context_info::CSS_NONE) {
    thd->mark_transaction_to_rollback(true);
    thd->commit_error = THD::CE_COMMIT_ERROR;

    if (thd->consensus_context.consensus_error ==
        Consensus_binlog_context_info::CSS_LEADERSHIP_CHANGE)
      my_error(ER_CONSENSUS_LEADERSHIP_CHANGE, MYF(0));
    else if (thd->consensus_context.consensus_error ==
             Consensus_binlog_context_info::CSS_LOG_TOO_LARGE)
      my_error(ER_CONSENSUS_LOG_TOO_LARGE, MYF(0));
    else if (thd->consensus_context.consensus_error ==
             Consensus_binlog_context_info::CSS_SHUTDOWN)
      my_error(ER_SERVER_SHUTDOWN, MYF(0));
    else
      my_error(ER_CONSENSUS_OTHER_ERROR, MYF(0));
  }

  return (thd->consensus_context.consensus_error !=
          Consensus_binlog_context_info::CSS_NONE);
}

static int consensus_binlog_manager_after_finish_commit(
    Binlog_manager_param *param) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  THD *thd = param->thd;

  if (thd->consensus_context.commit_locked) {
    mysql_rwlock_unlock(consensus_log_manager.get_consensuslog_commit_lock());
    thd->consensus_context.commit_locked = false;
  }

  uint64 commit_index = rpl_consensus_get_commit_index();

  update_consensus_applied_index(commit_index);

  return 0;
}

static int consensus_binlog_manager_before_rotate(Binlog_manager_param *param,
                                                  bool &do_rotate) {
  DBUG_TRACE;

  do_rotate = true;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  THD *thd = param->thd;

  /* Check server status */
  assert(!thd->consensus_context.status_locked);
  mysql_rwlock_rdlock(consensus_log_manager.get_consensuslog_status_lock());
  if (consensus_log_manager.get_status() !=
      Consensus_Log_System_Status::BINLOG_WORKING) {
    do_rotate = false;
    mysql_rwlock_unlock(consensus_log_manager.get_consensuslog_status_lock());
  } else {
    thd->consensus_context.status_locked = true;
  }

  return 0;
}

static int consensus_binlog_manager_after_rotate(Binlog_manager_param *param) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  THD *thd = param->thd;

  if (thd->consensus_context.status_locked) {
    mysql_rwlock_unlock(consensus_log_manager.get_consensuslog_status_lock());
    thd->consensus_context.status_locked = false;
  }

  return 0;
}

static int consensus_binlog_manager_rotate_and_purge(
    Binlog_manager_param *param, bool force_rotate) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  return rotate_consensus_log(param->thd, force_rotate);
}

static int consensus_binlog_manager_reencrypt_logs(Binlog_manager_param *) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  return reencrypt_consensus_logs();
}

static int consensus_binlog_manager_purge_logs(Binlog_manager_param *,
                                               ulong purge_time,
                                               ulong purge_size,
                                               const char *to_log,
                                               bool auto_purge) {
  DBUG_TRACE;

  /* If the plugin is not enabled, return success. */
  if (opt_initialize || !plugin_is_consensus_replication_enabled()) return 0;

  /* If the plugin is not running, return failed. */
  if (!plugin_is_consensus_replication_running()) return 1;

  return purge_consensus_logs_on_conditions(purge_time, purge_size, to_log,
                                            auto_purge);
}

Binlog_manager_observer cr_binlog_mananger_observer = {
    sizeof(Binlog_manager_observer),

    consensus_binlog_manager_binlog_recovery,
    consensus_binlog_manager_after_binlog_recovery,
    consensus_binlog_manager_gtid_recovery,
    consensus_binlog_manager_new_file,
    consensus_binlog_manager_after_purge_file,
    consensus_binlog_manager_before_flush,
    consensus_binlog_manager_write_transaction,
    consensus_binlog_manager_after_write,
    consensus_binlog_manager_after_flush,
    consensus_binlog_manager_after_sync,
    consensus_binlog_manager_after_enrolling_stage,
    consensus_binlog_manager_before_finish_in_engines,
    consensus_binlog_manager_after_finish_commit,
    consensus_binlog_manager_before_rotate,
    consensus_binlog_manager_after_rotate,
    consensus_binlog_manager_rotate_and_purge,
    consensus_binlog_manager_purge_logs,
    consensus_binlog_manager_reencrypt_logs,
};
