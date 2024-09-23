#include "consensus_applier_worker.h"

#include "sql/current_thd.h"  // current_thd
#include "sql/handler.h"
#include "sql/log.h"
#include "sql/mysqld.h"

const char *info_consensus_applier_worker_fields[] = {"number_of_lines", "id",
                                                      "consensus_apply_index"};

/*
  index value of some outstanding slots of info_consensus_applier_worker_fields
*/
enum {
  LINE_FOR_ID = 2,
};

const uint info_applier_worker_table_pk_field_indexes[] = {
    LINE_FOR_ID - 1,
    0,
};

Consensus_applier_worker::Consensus_applier_worker(
#ifdef HAVE_PSI_INTERFACE
    PSI_mutex_key *param_key_info_run_lock,
    PSI_mutex_key *param_key_info_data_lock,
    PSI_mutex_key *param_key_info_sleep_lock,
    PSI_mutex_key *param_key_info_thd_lock,
    PSI_mutex_key *param_key_info_data_cond,
    PSI_mutex_key *param_key_info_start_cond,
    PSI_mutex_key *param_key_info_stop_cond,
    PSI_mutex_key *param_key_info_sleep_cond
#endif
    ,
    uint param_id)
    : Rpl_info("Consensus applier worker"
#ifdef HAVE_PSI_INTERFACE
               ,
               param_key_info_run_lock, param_key_info_data_lock,
               param_key_info_sleep_lock, param_key_info_thd_lock,
               param_key_info_data_cond, param_key_info_start_cond,
               param_key_info_stop_cond, param_key_info_sleep_cond
#endif
               ,
               param_id + 1, ""),
      consensus_apply_index(0),
      id(param_id) {
  assert(internal_id == id + 1);
}

/**
Creates or reads information from the repository, initializing the
Consensus_info.
*/
int Consensus_applier_worker::init_info(bool on_recovery) {
  DBUG_ENTER("Consensus_applier_worker::init_info");

  enum_return_check check_return = ERROR_CHECKING_REPOSITORY;

  if (inited) DBUG_RETURN(0);

  mysql_mutex_init(key_LOCK_consensus_applier_worker,
                   &LOCK_Consensus_applier_worker, MY_MUTEX_INIT_FAST);

  check_return = check_info();
  if (check_return == ERROR_CHECKING_REPOSITORY ||
      (check_return == REPOSITORY_DOES_NOT_EXIST && on_recovery))
    goto err;

  if (handler->init_info()) goto err;

  if (on_recovery) {
    if (read_info(handler)) goto err;
  }

  inited = 1;
  if (flush_info(true, true)) goto err;

  DBUG_RETURN(0);
err:
  handler->end_info();
  inited = 0;
  abort();
  DBUG_RETURN(1);
}

void Consensus_applier_worker::end_info() {
  DBUG_ENTER("Consensus_applier_worker::end_info");

  if (!inited) DBUG_VOID_RETURN;

  mysql_mutex_destroy(&LOCK_Consensus_applier_worker);
  handler->end_info();
  inited = 0;
  DBUG_VOID_RETURN;
}

int Consensus_applier_worker::flush_info(bool force, bool need_commit) {
  DBUG_ENTER("Consensus_applier_worker::flush_info");
  int error = 0;
  if (!inited) DBUG_RETURN(0);
  /*
  We update the sync_period at this point because only here we
  now that we are handling a master info. This needs to be
  update every time we call flush because the option maybe
  dinamically set.
  */
  mysql_mutex_lock(&LOCK_Consensus_applier_worker);

  if (write_info(handler))
    goto err;

  error = handler->flush_info(force);

  if (error) {
    if (force && need_commit && current_thd)
      ha_rollback_trans(current_thd, true);
    goto err;
  } else if (force && need_commit && current_thd) {
    error = ha_commit_trans(current_thd, true, true);
  }

  mysql_mutex_unlock(&LOCK_Consensus_applier_worker);
  DBUG_RETURN(error);

err:
  sql_print_error("Consensus_applier_worker::flush_info error.");
  mysql_mutex_unlock(&LOCK_Consensus_applier_worker);
  abort();
  DBUG_RETURN(1);
}

bool Consensus_applier_worker::set_info_search_keys(Rpl_info_handler *to) {
  DBUG_ENTER("Consensus_applier_worker::set_info_search_keys");
  /* primary keys are Id */
  if (to->set_info(LINE_FOR_ID - 1, (int)internal_id)) DBUG_RETURN(true);
  DBUG_RETURN(false);
}

bool Consensus_applier_worker::read_info(Rpl_info_handler *from) {
  DBUG_ENTER("Consensus_applier_worker::read_info");

  ulong temp_apply_index = 0;
  int temp_internal_id = 0;
  char number_of_lines[FN_REFLEN] = {0};

  if (from->prepare_info_for_read() ||
      !!from->get_info(number_of_lines, sizeof(number_of_lines), ""))
    DBUG_RETURN(true);

  if (!!from->get_info(&temp_internal_id, 0) ||
      !!from->get_info(&temp_apply_index, 0))
    DBUG_RETURN(true);

  internal_id = (uint)temp_internal_id;
  consensus_apply_index = temp_apply_index;

  DBUG_RETURN(false);
}

bool Consensus_applier_worker::write_info(Rpl_info_handler *to) {
  DBUG_ENTER("Consensus_applier_worker::write_info");
  if (to->prepare_info_for_write() ||
      to->set_info((int)get_number_fields()) ||
      to->set_info((int)internal_id) ||
      to->set_info((ulong)consensus_apply_index))
    DBUG_RETURN(true);
  DBUG_RETURN(false);
}

size_t Consensus_applier_worker::get_number_fields() {
  return sizeof(info_consensus_applier_worker_fields) /
         sizeof(info_consensus_applier_worker_fields[0]);
}

void Consensus_applier_worker::set_nullable_fields(MY_BITMAP *nullable_fields) {
  bitmap_init(nullable_fields, nullptr,
              Consensus_applier_worker::get_number_fields());
  bitmap_set_all(nullable_fields);       // All fields may be NULL except for
  bitmap_clear_bit(nullable_fields, 0);  // NUMBER_OF_LINES
}

const uint *Consensus_applier_worker::get_table_pk_field_indexes() {
  return info_applier_worker_table_pk_field_indexes;
}

int Consensus_applier_worker::commit_positions(uint64 event_consensus_index) {
  saved_consensus_apply_index = get_consensus_apply_index();
  set_consensus_apply_index(event_consensus_index);
  return flush_info(true);
}

int Consensus_applier_worker::rollback_positions() {
  set_consensus_apply_index(saved_consensus_apply_index);
  return 0;
}


/**
   Clean up a part of Worker info table that is regarded in
   in gaps collecting at recovery.

   @return false as success true as failure
*/
bool Consensus_applier_worker::reset_recovery_info() {
  DBUG_TRACE;
  set_consensus_apply_index(0);
  return flush_info(true, true);
}

int Consensus_applier_worker::init_worker(ulong i) {
  DBUG_TRACE;
  if (init_info(false)) return 1;

  id = i;
  return 0;
}