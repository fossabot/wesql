/************************************************************************
 *
 * Copyright (c) 2016 Alibaba.com, Inc. All Rights Reserved
 * $Id:  learner_server.cc,v 1.0 05/08/2017 3:40:11 PM
 *jarry.zj(jarry.zj@alibaba-inc.com) $
 *
 ************************************************************************/

/**
 * @file learner_server.cc
 * @author jarry.zj(jarry.zj@alibaba-inc.com)
 * @date 05/08/2017 3:40:11 PM
 * @version 1.0
 * @brief
 *
 **/
#include "learner_server.h"

#include <atomic>

#include "io/easy_log.h"
#include "mem_paxos_log.h"
#include "paxos.pb.h"
#include "service.h"
#include "single_leader.h"

namespace alisql {

/*
 * LearnerServer implement
 */
LearnerServer::LearnerServer(uint64_t serverId)
    : RemoteServer(serverId), singleLeader(NULL), connectTimeout(1000) {}

void LearnerServer::stop(void*) {
  nextIndex = 1;
  disconnect(NULL);
  if (srv != nullptr) srv->getNet()->delConnDataById(serverId);
}

void LearnerServer::connect(void*) {
  if (addr == nullptr || !addr->isValid()) {
    addr = srv->createConnection(strAddr, getSharedThis(), connectTimeout,
                                 serverId);
  }
  if (addr == nullptr || !addr->isValid()) {
    easy_error_log(
        "failed to connect server %ld, now this server is disconnected, try to "
        "reconnect.\n",
        serverId);
  }
}

void LearnerServer::disconnect(void*) {
  if (addr != nullptr && addr->isValid()) {
    srv->disableConnection(addr);
    addr->clear();
  }
}

uint64_t LearnerServer::getLastLogIndex() {
  return singleLeader->getLog()->getLastLogIndex();
}

void LearnerServer::onConnectCb() {
  auto mlog = singleLeader->getLog();
  uint64_t ni = mlog->getLastLogIndex();
  if (ni == singleLeader->mockLastIndex_) ni++;
  nextIndex.store(ni);
  easy_log(
      "LearnerServer : update server %d 's nextIndex(new:%llu) when "
      "onConnect\n",
      serverId, nextIndex.load());
  /* try append once connected */
  singleLeader->appendLogToLearner(true);
}

void LearnerServer::sendMsg(void* ptr) {
  PaxosMsg* msg = (PaxosMsg*)ptr;
  assert(msg->msgtype() == Consensus::AppendLog);

  msg->set_msgid(msgId.fetch_add(1));
  if (addr == nullptr || !addr->isValid()) {
    addr = srv->createConnection(strAddr, getSharedThis(), connectTimeout,
                                 serverId);
    if (addr == nullptr || !addr->isValid()) {
      easy_error_log(
          "failed to connect server %ld, now this server is disconnected, try "
          "to reconnect.\n",
          serverId);
    }
    return;
  }

  if (netError.load()) {
    addr = srv->recreateConnectionIfNecessary(strAddr, addr, getSharedThis(),
                                              getConnTimeout());
    if (addr == nullptr || !addr->isValid()) {
      easy_error_log(
          "failed to connect server %ld, now this server is disconnected, try "
          "to reconnect.\n",
          serverId);
    }
    return;
  }

  msg->set_clusterid(singleLeader->getClusterId());
  msg->set_serverid(serverId);
  /* fill msg */
  if (waitForReply == 1) {
    easy_warn_log(
        "Try to send msg to server %ld, now we are waiting for response.",
        serverId);
    return;
  }
  // appendlogfillforeach
  singleLeader->appendLogFillForEach(msg, this);
  if (msg->entries_size() == 0) {
    return;
  }
  std::string buf;
  msg->SerializeToString(&buf);
  srv->sendPacket(addr, buf, msg->msgid());
  waitForReply = 1;
}

}  // namespace alisql
