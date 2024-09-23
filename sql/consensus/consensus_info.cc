/* Copyright (c) 2018, 2021, Alibaba and/or its affiliates. All rights reserved.
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.
   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL/Apsara GalaxyEngine hereby grant you an
   additional permission to link the program and your derivative works with the
   separately licensed software that they have included with
   MySQL/Apsara GalaxyEngine.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/consensus/consensus_info.h"
#include "sql/current_thd.h"
#include "sql/handler.h"
#include "sql/log.h"
#include "sql/mysqld.h"

const char *info_consensus_fields[] = {
    "number_of_lines",      "vote_for",         "current_term",
    "recover_status",       "last_leader_term", "start_apply_index",
    "cluster_id",           "cluster_info",     "cluster_learner_info",
    "cluster_recover_index"};

Consensus_info::Consensus_info(
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
    )
    : Rpl_info("Consensus info"
#ifdef HAVE_PSI_INTERFACE
               ,
               param_key_info_run_lock, param_key_info_data_lock,
               param_key_info_sleep_lock, param_key_info_thd_lock,
               param_key_info_data_cond, param_key_info_start_cond,
               param_key_info_stop_cond, param_key_info_sleep_cond
#endif
               ,
               0, ""),
      vote_for(0),
      current_term(1),
      recover_status(0),
      last_leader_term(0),
      start_apply_index(0),
      cluster_id(0),
      cluster_info(""),
      cluster_learner_info(""),
      cluster_recover_index(0) {
}

/**
Creates or reads information from the repository, initializing the
Consensus_info.
*/
int Consensus_info::init_info() {
  DBUG_ENTER("Consensus_info::init_info");

  enum_return_check check_return = ERROR_CHECKING_REPOSITORY;

  if (inited) DBUG_RETURN(0);

  mysql_mutex_init(key_LOCK_consensus_info, &LOCK_consensus_info,
                   MY_MUTEX_INIT_FAST);

  if ((check_return = check_info()) == ERROR_CHECKING_REPOSITORY) goto err;

  if (handler->init_info()) goto err;

  if (check_return != REPOSITORY_DOES_NOT_EXIST) {
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

void Consensus_info::end_info() {
  DBUG_ENTER("Consensus_info::end_info");

  if (!inited) DBUG_VOID_RETURN;

  mysql_mutex_destroy(&LOCK_consensus_info);
  handler->end_info();
  inited = 0;
  DBUG_VOID_RETURN;
}

int Consensus_info::flush_info(bool force, bool need_commit) {
  DBUG_ENTER("Consensus_info::flush_info");
  int error = 0;
  if (!inited) DBUG_RETURN(0);
  /*
  We update the sync_period at this point because only here we
  now that we are handling a master info. This needs to be
  update every time we call flush because the option maybe
  dinamically set.
  */
  mysql_mutex_lock(&LOCK_consensus_info);

  if (write_info(handler))
    goto err;

  error = handler->flush_info(force);

  if (error) {
    if (force && need_commit && current_thd)
      ha_rollback_trans(current_thd, true);
    goto err;
  } else if (force && current_thd && need_commit) {
    error = ha_commit_trans(current_thd, true, true);
  }

  mysql_mutex_unlock(&LOCK_consensus_info);
  DBUG_RETURN(error);
err:
  sql_print_error("Consensus_info::flush_info error.");
  mysql_mutex_unlock(&LOCK_consensus_info);
  abort();
  DBUG_RETURN(1);
}

bool Consensus_info::set_info_search_keys(Rpl_info_handler *to) {
  DBUG_ENTER("Consensus_info::set_info_search_keys");
  if (to->set_info(0, (int)get_number_info_consensus_fields()))
    DBUG_RETURN(true);
  DBUG_RETURN(false);
}

bool Consensus_info::read_info(Rpl_info_handler *from) {
  DBUG_ENTER("Consensus_info::read_info");

  ulong temp_vote_for = 0;
  ulong temp_current_term = 0;
  ulong temp_recover_status = 0;
  ulong temp_local_term = 0;
  ulong temp_start_apply_index = 0;
  ulong temp_cluster_id = 0;
  char temp_cluster_info[CLUSTER_CONF_STR_LENGTH];
  char temp_cluster_learner_info[CLUSTER_CONF_STR_LENGTH];
  ulong temp_cluster_recover_index = 0;

  if (from->prepare_info_for_read() ||
      !!from->get_info(consensus_config_name, sizeof(consensus_config_name),
                       ""))
    DBUG_RETURN(true);

  if (!!from->get_info(&temp_vote_for, 0) ||
      !!from->get_info(&temp_current_term, 0) ||
      !!from->get_info(&temp_recover_status, 0) ||
      !!from->get_info(&temp_local_term, 0) ||
      !!from->get_info(&temp_start_apply_index, 0) ||
      !!from->get_info(&temp_cluster_id, 0) ||
      !!from->get_info(temp_cluster_info, sizeof(temp_cluster_info),
                       "") ||
      !!from->get_info(temp_cluster_learner_info,
                       sizeof(temp_cluster_learner_info), "") ||
      !!from->get_info(&temp_cluster_recover_index, 0))
    DBUG_RETURN(true);

  vote_for = temp_vote_for;
  current_term = temp_current_term;
  recover_status = temp_recover_status;
  last_leader_term = temp_local_term;
  start_apply_index = temp_start_apply_index;
  cluster_id = temp_cluster_id;
  cluster_info = std::string(temp_cluster_info, strlen(temp_cluster_info));
  cluster_learner_info =
      std::string(temp_cluster_learner_info, strlen(temp_cluster_learner_info));
  cluster_recover_index = temp_cluster_recover_index;

  DBUG_RETURN(false);
}

bool Consensus_info::write_info(Rpl_info_handler *to) {
  DBUG_ENTER("ConsensusLogManager::write_info");
  if (to->prepare_info_for_write() ||
      to->set_info((int)get_number_info_consensus_fields()) ||
      to->set_info((ulong)vote_for) || to->set_info((ulong)current_term) ||
      to->set_info((ulong)recover_status) ||
      to->set_info((ulong)last_leader_term) ||
      to->set_info((ulong)start_apply_index) ||
      to->set_info((ulong)cluster_id) || to->set_info(cluster_info.c_str()) ||
      to->set_info(cluster_learner_info.c_str()) ||
      to->set_info((ulong)cluster_recover_index))
    DBUG_RETURN(true);
  DBUG_RETURN(false);
}

size_t Consensus_info::get_number_info_consensus_fields() {
  return sizeof(info_consensus_fields) / sizeof(info_consensus_fields[0]);
}

void Consensus_info::set_nullable_fields(MY_BITMAP *nullable_fields) {
  bitmap_init(nullable_fields, nullptr,
              Consensus_info::get_number_info_consensus_fields());
  bitmap_clear_all(nullable_fields);
}
