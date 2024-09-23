// Portions Copyright (c) 2023 ApeCloud, Inc.
// Copyright (c) 2016 Alibaba.com, Inc. All Rights Reserved

#include "paxos_server.h"

#include "fail_point.h"
#include "multi_process_queue.h"
#include "paxos.h"

namespace alisql {

FAIL_POINT_DEFINE(isolateAFollowerOneWay);
FAIL_POINT_DEFINE(isolateAFollowerBothWay);

uint64_t Server::getLastLogIndex() {
  if (paxos) {
    return paxos->log_->getLastLogIndex();
  }
  return 0;
}

uint64_t Server::getLastCachedLogIndex() {
  if (paxos) {
    return paxos->log_->getLastCachedLogIndex();
  }
  return 0;
}

/* Implement for LocalServer */
LocalServer::LocalServer(uint64_t serverId)
    : Server(serverId),
      lastSyncedIndex(1),
      logType(false),
      learnerConnTimeout(0),
      cidx(1000) {}

void LocalServer::beginLeadership(void*) {
  lastSyncedIndex.store(getLastLogIndex());
}

void LocalServer::sendMsg(void* ptr) {
  PaxosMsg* msg = (PaxosMsg*)ptr;
  msg->set_serverid(serverId);
  /* TODO put the msg into service or drop it */
}

uint64_t LocalServer::getLastAckEpoch() const {
  if (paxos) {
    return paxos->getCurrentEpoch();
  }
  return 0;
}

uint64_t LocalServer::getAppliedIndex() const {
  if (paxos) {
    return paxos->getAppliedIndex();
  }
  return 0;
}

void LocalServer::fillInfo(void* ptr) {
  // The result is only accurate when executed on a leader.
  Paxos::ClusterInfoType ci;
  std::vector<Paxos::ClusterInfoType>* cis =
      static_cast<std::vector<Paxos::ClusterInfoType>*>(ptr);

  ci.serverId = serverId;
  ci.ipPort = strAddr;
  ci.matchIndex = lastSyncedIndex.load();
  ci.nextIndex = 0;
  ci.role = Paxos::LEADER;
  ci.hasVoted = 1;
  ci.forceSync = forceSync;
  ci.electionWeight = electionWeight;
  ci.learnerSource = 0;
  ci.appliedIndex = getAppliedIndex();
  ci.pipelining = false;
  ci.useApplied = false;

  cis->push_back(std::move(ci));
}

void LocalServer::fillFollowerMeta(void* ptr) { return; }

bool LocalServer::haveVoted() const {
  if (paxos) {
    return paxos->votedFor_ == serverId;
  }
  return false;
}

uint64_t LocalServer::appendLog(LogEntry& entry) {
  if (paxos) {
    return paxos->log_->appendWithCheck(entry);
  }
  return 0;
}

uint64_t LocalServer::writeLog(LogEntry& entry) {
  uint64_t logIndex = appendLog(entry);
  if (logIndex > 0) writeLogDoneInternal(logIndex);
  // writeLogDone(logIndex);
  return logIndex;
}

uint64_t LocalServer::writeLogDoneInternal(uint64_t logIndex, bool forceSend) {
  bool ret = false;
  for (;;) {
    uint64_t old = lastSyncedIndex.load();
    if (old > logIndex ||
        (old < logIndex &&
         (ret = lastSyncedIndex.compare_exchange_weak(old, logIndex))))
      break;
  }
  if (ret && paxos->getConsensusAsync()) paxos->cond_.notify_all();

  if (forceSend) /* for large trx, send directly after sync partial */
  {
    easy_info_log("Server %d : writeLogDoneInternal logIndex:%ld\n", serverId,
                  logIndex);
    paxos->appendLog(false);
  }
  return 0;
}

uint64_t LocalServer::writeLogDone(uint64_t logIndex) {
  return writeLogDoneInternal(logIndex);
}

void LocalServer::writeCacheLogDone() {
  if (paxos && paxos->getReplicateWithCacheLog()) {
    paxos->appendLog(false);
  }
}

uint64_t AliSQLServer::writeLogDone(uint64_t logIndex) {
  // tested by case AliSQLServer_writeLogDone
  if (!paxos) {
    return 0;
  }

  bool ret = false;
  for (;;) {
    uint64_t old = lastSyncedIndex.load();
    if (old >= logIndex ||
        (old < logIndex &&
         (ret = lastSyncedIndex.compare_exchange_weak(old, logIndex))))
      break;
  }
  if (ret && paxos->getConsensusAsync()) paxos->cond_.notify_all();
  /*
   * In AliSQLServer mode, we write local log first, so we do not need to
   * tryUpdateCommitIndex here. Later, we will write local log and send msg in
   * the same time, at that time we should call tryUpdateCommitIndex here.
   */
  int tmp = 0;

  tmp = paxos->tryUpdateCommitIndex();
  easy_info_log(
      "Server %d : writeLogDone logIndex:%ld, tryUpdateCommitIndex return:%d\n",
      serverId, logIndex, tmp);

  if (paxos->getReplicateWithCacheLog() == false) paxos->appendLog(false);

  return logIndex;
}

void AliSQLServer::setLastNonCommitDepIndex(uint64_t logIndex) {
  if (paxos) {
    /* AliSQLServer do not append log by replicateLog */
    paxos->cdrMgr_.setLastNonCommitDepIndex(logIndex);
  }
}

/* Implement for RemoteServer */
RemoteServer::RemoteServer(uint64_t serverId)
    : Server(serverId),
      sendMsgQueue(nullptr),
      nextIndex(1),
      matchIndex(0),
      lastAckEpoch(0),
      voted(false),
      isLeader(false),
      isLearner(false),
      hasMatched(false),
      needAddr(true),
      learnerReadyForAppendLog(false),
      connected(false),
      disablePipelining(false),
      lostConnect(false),
      netError(true),
      isStop(false),
      waitForReply(0),
      guardId(1),
      msgId(1),
      appliedIndex(0),
      lastEntrySize(0) {
  sendMsgQueue = std::make_shared<SingleProcessQueue<SendMsgTask>>();
  lastSendTP = now();
  lastMergeTP = now() - std::chrono::hours(1);
}

RemoteServer::~RemoteServer() { stop(nullptr); }

void RemoteServer::beginRequestVote(void*) {
  voted = false;
  if (paxos) {
    lastAckEpoch.store(paxos->getCurrentEpoch());
  }
}

void RemoteServer::beginLeadership(void* skipReset) {
  if (!skipReset) {
    nextIndex = getLastLogIndex() + 1;
    if (isLearner && sendByAppliedIndex && paxos != nullptr)
      nextIndex = paxos->getAppliedIndex() + 1;
    resetMatchIndex(0);
  }

  if (!isLearner || paxos->option.enableLearnerHeartbeat_) {
    isLeader = true;
    // heartbeatTimer.resetTimer(false);
    if (heartbeatTimer != nullptr) heartbeatTimer->restart();
  }

  waitForReply = 0;
  lostConnect.store(false);
  sendMsgQueue->start();
  /*
  if (paxos)
  {
    lastAckEpoch.store(paxos->getCurrentEpoch());
  }
  */
  isStop.store(false);
}

void RemoteServer::stepDown(void*) {
  resetMatchIndex(0);
  nextIndex = 1;
  isLeader = false;
  /* Stop heartbeatTimer when we're step down. */
  if (heartbeatTimer != nullptr) heartbeatTimer->stop();
}

void RemoteServer::stop(void*) {
  bool rStop = false;
  if (isStop.compare_exchange_weak(rStop, true)) {
    stepDown(NULL);
    if (sendMsgQueue && sendMsgQueue->stop(false)) {
      sendMsgQueue.reset();
      sendMsgQueue = nullptr;
    }
    disconnect(NULL);
    if (srv != nullptr) srv->getNet()->delConnDataById(serverId);
  }
}

uint64_t RemoteServer::getConnTimeout() {
  if (!isLearner || !paxos || paxos->getLocalServer()->learnerConnTimeout == 0)
    return paxos ? paxos->getHeartbeatTimeout() / 4 : 1000;
  else
    return paxos->getLocalServer()->learnerConnTimeout;
}

void RemoteServer::connect(void* ptr) {
  if (addr == nullptr || !addr->isValid()) {
    uint64_t cidx;
    if (paxos && paxos->getEnableDynamicEasyIndex()) {
      // make sure cidx > 256, serverId < 1000
      cidx = paxos->getLocalServer()->cidx + serverId;
      paxos->getLocalServer()->cidx += 1000;
    } else
      cidx = serverId;
    easy_info_log("Connect server %d, cidx %llu", serverId, cidx);
    addr =
        srv->createConnection(strAddr, getSharedThis(), getConnTimeout(), cidx);
    if (addr == nullptr || !addr->isValid()) {
      easy_error_log(
          "failed to connect server %ld, now this server is disconnected, try "
          "to reconnect.\n",
          serverId);
    }
  }
}

void RemoteServer::disconnect(void* ptr) {
  if (addr && addr->isValid()) {
    srv->disableConnection(addr);
    addr->clear();
  }
}

void RemoteServer::sendMsg(void* ptr) { sendMsgFunc(false, false, ptr); }

void RemoteServer::sendMsgFunc(bool lockless, bool force, void* ptr) {
  if (isLearner) {
    if ((learnerSource == 0 && paxos && paxos->getState() != Paxos::LEADER))
      return;

    if (learnerSource != 0 && paxos &&
        (paxos->getLocalServer()->serverId != learnerSource))
      return;
  }

  // tested by case flow_control
  if (!force && flowControl < Paxos::FlowControlMode::Normal) return;

  PaxosMsg* msg = (PaxosMsg*)ptr;
  // assert(!msg->has_msgid());
  msg->set_msgid(msgId.fetch_add(1));
  if (lockless || msg->msgtype() != Paxos::AppendLog) {
    // if lockless is true and msgtype is AppendLog, 
    // the message won't carry any log entry
    sendMsgFuncInternal(lockless, force, ptr, false);
  } else {
    PaxosMsg* arg = new PaxosMsg(*msg);
    assert(msg->msgtype() == Paxos::AppendLog);
    if (sendMsgQueue &&
        sendMsgQueue->push(new SendMsgTask(arg, getSharedThis(), force))) {
      auto wqueue =
          std::weak_ptr<SingleProcessQueue<SendMsgTask>>(sendMsgQueue);
      srv->sendAsyncEvent(
          &SingleProcessQueue<SendMsgTask>::mergeableProcessWeak, wqueue,
          RemoteServer::sendMsgFuncAsync);
    }
  }

  /*
  if (paxos && paxos->getState() == Paxos::FOLLOWER)
  {
    paxos->updateFollowerMetaNo();
    easy_warn_log("Server %d : updateFollowerMetaNo\n",
  paxos->getLocalServer()->serverId);
  }
  */
}

void RemoteServer::sendMsgFuncAsync(SendMsgTask* task) {
  task->server->sendMsgFuncInternal(false, task->force, (void*)task->msg, true);
}

void RemoteServer::sendMsgFuncInternal(bool lockless, bool force, void* ptr,
                                       bool async) {
#ifdef FAIL_POINT
  auto tmpServerId = this->serverId;
  // This failpoint is used to simulate that the leader can't send msg to target
  // server(target serverId is stored in FailPointData).
  if (isolateAFollowerOneWay.evaluateFailIf(
          [&tmpServerId](FailPointData& data) {
            if (tmpServerId == data.data_.u64) return true;
            return false;
          })) {
    easy_error_log(
        "isolateAFollowerOneWay failpoint triggered, target server id=%d",
        tmpServerId);
    return;
  }

  if (paxos) {
    // This failpoint is used to simulate that the leader can't send msg to
    // target server(target serverId is stored in FailPointData). And the target
    // server can't send msg to others.
    auto myServerId = paxos->getLocalServer()->serverId;
    if (isolateAFollowerBothWay.evaluateFailIf(
            [&tmpServerId, &myServerId](FailPointData& data) {
              if (tmpServerId == data.data_.u64 || myServerId == data.data_.u64)
                return true;
              return false;
            })) {
      easy_error_log(
          "isolateAFollowerBothWay failpoint triggered, my server id=%d, "
          "target server id=%d",
          myServerId, tmpServerId);
      return;
    }
  }
#endif
  PaxosMsg* msg = (PaxosMsg*)ptr;
  uint64_t logSize = 0;
  bool lostConnectMode = false;

  if (isStop.load()) return;
  /* Skip send msg this time, connect action will done before next send msg. */
  if (addr == nullptr || !addr->isValid()) {
    addr = srv->createConnection(strAddr, getSharedThis(), getConnTimeout(),
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
    easy_info_log(
        "Try to send msg to server %ld, now this server is disconnected, try "
        "to reconnect.\n",
        serverId);
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
  // if remote server is responsible to connect with a new learner
  // the message should be sent after onConnectCb is triggered(learnerReadyForAppendLog
  // == true) the message will not be missed, because in onConnectCb, it will
  // resend this msg
  if (isLearner && !learnerReadyForAppendLog) {
    easy_warn_log(
        "Server %llu : remote server %llu is not ready for sending log",
        paxos->getLocalServer()->serverId, serverId);
    return;
  }

  msg->set_clusterid(paxos->getClusterId());

  /* Fill the each server part. */
  if (msg->msgtype() == Paxos::AppendLog)
    msg->set_serverid(serverId);
  else {
    if (paxos != NULL)
      msg->set_serverid(paxos->getLocalServer()->serverId);
    else /* For AppendLog and unit test */
      msg->set_serverid(serverId);
  }
  if (isLearner) {
    msg->set_serverid(serverId);
  }

  if (msg->msgtype() == Paxos::AppendLog) {
    bool islargeEntry = lastEntrySize >= paxos->getMaxPipeliningEntrySize();
    auto localNextIndex = nextIndex.load();
    auto lastLogIndex = paxos->getReplicateWithCacheLog()
                            ? paxos->getLastCachedLogIndex()
                            : paxos->getLastLogIndex();
    bool isDelay = lastLogIndex > localNextIndex + paxos->getMaxDelayIndex();
    uint64_t maxSendIndex = paxos->getMaxDelayIndex() / 10;
    maxSendIndex = (maxSendIndex < 2) ? 2 : maxSendIndex;
    /* Case when matchIndex is 0, this is not SendTooMuch case, at this time
     * we're recalculate the matchIndex. */
    bool isSendTooMuch = matchIndex.load() != 0 &&
                         ((localNextIndex - matchIndex.load()) >= maxSendIndex);

    if (!isLearner || paxos->getEnableLearnerPipelining()) {
      if (!isLearner)  // learner does not maintain epoch information
      {
        auto currentEpoch = paxos->getCurrentEpoch();
        if (lostConnect.load()) {
          lostConnectMode = true;
        } else if (currentEpoch != 0 &&
                   lastAckEpoch.load() < currentEpoch - 1) {
          easy_log(
              "Detect lost connect to server %llu, currentEpoch:%llu, "
              "lastAckEpoch:%llu!",
              serverId, paxos->getCurrentEpoch(), lastAckEpoch.load());
          lostConnect.store(true);
          lostConnectMode = true;
        }

        if (lostConnectMode && !force) {
          /* We only send "empty" heartbeat(force) msg to the lostConnectMode
           * server! */
          easy_warn_log(
              "Try to send msg to server %ld, now this server is in lost "
              "connect mode, ignore.\n",
              serverId);
          return;
        }
      }

      if (lostConnectMode && !disablePipelining) {
        easy_warn_log(
            "Try to send msg to server %ld, server is in lost connect mode, "
            "disable pipelining.\n",
            serverId, lastLogIndex, localNextIndex);
        disablePipelining = true;
        guardId = msg->msgid() - 1;
        if (matchIndex.load() != 0) nextIndex.store(matchIndex.load() + 1);
      }

      if (islargeEntry && !disablePipelining) {
        disablePipelining = true;
        guardId = msg->msgid() - 1;
        if (matchIndex.load() != 0) nextIndex.store(matchIndex.load() + 1);
        easy_warn_log(
            "Try to send msg to server %ld, we are sending large "
            "LogEntry(lli:%llu, nextIndex:%llu), disable pipelining.\n",
            serverId, lastLogIndex, localNextIndex);
      }
      if (isDelay && !disablePipelining) {
        disablePipelining = true;
        guardId = msg->msgid() - 1;
        if (matchIndex.load() != 0) nextIndex.store(matchIndex.load() + 1);
        easy_warn_log(
            "Try to send msg to server %ld, we are delay too much(lli:%llu, "
            "nextIndex:%llu), disable pipelining.\n",
            serverId, lastLogIndex, localNextIndex);
      }
      if (isSendTooMuch && !disablePipelining) {
        disablePipelining = true;
        guardId = msg->msgid() - 1;
        if (matchIndex.load() != 0) nextIndex.store(matchIndex.load() + 1);
        easy_warn_log(
            "Try to send msg to server %ld, we have send too much this server, "
            "ignore this send and disable pipelining(matchIndex:%llu, "
            "nextIndex:%llu).\n",
            serverId, matchIndex.load(), localNextIndex);
      }

      if ((!lostConnectMode && !islargeEntry && !isDelay && !isSendTooMuch &&
           lastLogIndex < localNextIndex + paxos->getMinDelayIndex()) &&
          disablePipelining) {
        disablePipelining = false;
        easy_warn_log(
            "Try to send msg to server %ld, enable pipelining(lli:%llu, "
            "nextIndex:%llu).\n",
            serverId, matchIndex.load(), localNextIndex);
      }
    }

    uint64_t timeout = paxos->getPipeliningTimeout() * 1000;
    if (waitForReply == 1) {
      if (isLearner && !paxos->getEnableLearnerPipelining()) {
        easy_warn_log(
            "Try to send msg to server %ld, now we are waiting for response, "
            "and this is learner skip.",
            serverId);
        return;
      }
      if (lostConnectMode) {
        easy_warn_log(
            "Try to send msg to server %ld, now this server is in lost connect "
            "mode, ignore.\n",
            serverId);
        return;
      }
      uint64_t maxPacketSize =
          isDelay ? (paxos->getMaxPacketSize() * paxos->getLargeBatchRatio())
                  : paxos->getMaxPacketSize();
      uint64_t msSinceLastSendTP = diffMS(lastSendTP);
      bool isTimeout = timeout != 0 && msSinceLastSendTP > timeout;
      if (!isTimeout &&
          !paxos->getLog()->getLeftSize(nextIndex, maxPacketSize)) {
        easy_warn_log(
            "Try to send msg to server %ld, now we are waiting for response, "
            "ignore.\n",
            serverId);
        return;
      }
      if (disablePipelining) {
        easy_warn_log(
            "Try to send msg to server %ld, now we are disable pipelining, "
            "ignore.\n",
            serverId);
        return;
      }
      if (isTimeout) {
        easy_warn_log(
            "Force to send msg to server %ld, because past time reaches %d ms "
            "and exceeds timeout (%d ms).\n",
            serverId, msSinceLastSendTP, timeout);
      } else {
        easy_warn_log(
            "Force to send msg to server %ld, because the left log size is too "
            "large.\n",
            serverId);
      }

      if (timeout != 0) lastSendTP = now();
    }
    waitForReply = 1;

    Paxos::LogFillModeT mode = Paxos::NormalMode;
    if (lostConnectMode)
      mode = Paxos::EmptyMode;
    else if ((isLearner && !paxos->getEnableLearnerPipelining()) || isDelay)
      mode = Paxos::LargeBatchMode;

    // tested by case flow_control
    /* here we use this pointer is safe. */
    if (flowControl >= Paxos::FlowControlMode::Slow) {
      if (async)
        logSize = paxos->appendLogFillForEachAsync(msg, this, mode);
      else if (!lockless)
        logSize = paxos->appendLogFillForEach(msg, this, mode);
      else {
        // TODO: any problem? 
        // won't carry any log entry if async=0 && lockless=1
      }
    }

    ++(paxos->stats_.countMsgAppendLog);
    if (msg->entries_size() == 0)
      ++(paxos->stats_.countHeartbeat);
    else
#if GOOGLE_PROTOBUF_VERSION >= 3009002
      lastEntrySize = msg->entries().rbegin()->ByteSizeLong();
#else
      lastEntrySize = msg->entries().rbegin()->ByteSize();
#endif
  } else if (msg->msgtype() == Paxos::RequestVote)
    ++(paxos->stats_.countMsgRequestVote);

  /* If there are log left, we try to send the continue log entries. */
  if (logSize >= paxos->getMaxPacketSize() && matchIndex.load() != 0 &&
      !paxos->cdrMgr_.inRecovery)
    paxos->appendLogToServerByPtr(getSharedThis(), true, false);

  // assert(msg->entries_size() <= 1);
  int64_t lli = -1;
  if (paxos) {
    if (paxos->getReplicateWithCacheLog())
      lli = paxos->getLastCachedLogIndex();
    else
      lli = paxos->getLastLogIndex();
  }
  easy_info_log(
      "Server %d : Send msg msgId(%llu) to server %ld, term:%ld, "
      "startLogIndex:%ld, entries_size:%d, log_size:%llu lli:%ld\n",
      paxos ? paxos->getLocalServer()->serverId : 0, msg->msgid(), serverId,
      msg->term(),
      msg->entries_size() >= 1 ? msg->entries().begin()->index() : -1,
      msg->entries_size(), logSize, lli);

  if (msg->entries_size() > 0) {
    assert(msg->prevlogindex() == msg->entries().begin()->index() - 1);
    if (isLearner && !paxos->option.enableLearnerHeartbeat_)
      heartbeatTimer->stop();
  } else if (msg->msgtype() == Paxos::AppendLog && !force) {
    easy_warn_log(
        "Server %d : Skip send msg msgId(%llu) to server %ld because the "
        "entries_size is 0, and not force\n",
        paxos ? paxos->getLocalServer()->serverId : 0, msg->msgid(), serverId);
    waitForReply = 0;
    if (isLearner && !paxos->option.enableLearnerHeartbeat_) {
      easy_warn_log(
          "Server %d : current server is learner but msg entries_size is 0, "
          "start heartbeat.",
          paxos ? paxos->getLocalServer()->serverId : 0);
      heartbeatTimer->restart();
    }
    return;
  }

  if (paxos->cdrMgr_.inRecovery) {
    /* reset term and lastlogindex */
    msg->set_term(paxos->getTerm());
    msg->set_lastlogindex(paxos->cdrMgr_.lastLogIndex);
    /* in this case, prevLogTerm is not set */
  }

  if (msg->msgtype() == Paxos::AppendLog)
    msgCompress(msgCompressOption, *msg, logSize);

  std::string buf;
  msg->SerializeToString(&buf);

  /*
   * If we send RequestVote here, we don't reset the heartbeatTimer.
   * Because we're candidate now, and resetTimer will enable heartbeatTimer !
   * XXX here we only reset heartbeatTimer when msg type is AppendLog (not
   * include LeaderCommand) !!
   */
  if (isLeader && msg->msgtype() == Paxos::AppendLog) heartbeatTimer->restart();
  srv->sendPacket(addr, buf, msg->msgid());
}

void RemoteServer::onConnectCb() {
  connected.store(true);
  if (paxos && (paxos->getState() == Paxos::LEADER || isLearner)) {
    /* XXX We reset nextIndex to matchIndex+1 on connected, because some resend
     * msg may be lost in the disconnected period (libeasy's mechanism). */
    uint64_t oldNextIndex = nextIndex;
    resetNextIndex();
    learnerReadyForAppendLog = true;
    easy_log(
        "Server %d : update server %d 's nextIndex(old:%llu,new:%llu) when "
        "onConnect\n",
        paxos->getLocalServer()->serverId, serverId, oldNextIndex,
        nextIndex.load());
    hasMatched = false;

    /* XXX we send append log to other servers only when we're leader, we also
     * judge it in appendLogToServer */
    if (isLearner) paxos->appendLogToLearner(nullptr, true);
  }
  // Paxos::heartbeatCallback(this);
}

void RemoteServer::onDisconnectCb() {
  connected.store(false);
  if (paxos && (paxos->getState() == Paxos::LEADER || isLearner)) {
    learnerReadyForAppendLog = false;
  }
}

void RemoteServer::resetNextIndex() {
  paxos->resetNextIndexForServer(getSharedThis());
}

void RemoteServer::fillInfo(void* ptr) {
  // The result is only accurate when executed on a leader.
  Paxos::ClusterInfoType ci;
  std::vector<Paxos::ClusterInfoType>* cis =
      static_cast<std::vector<Paxos::ClusterInfoType>*>(ptr);

  ci.serverId = serverId;
  ci.ipPort = strAddr;
  ci.matchIndex = matchIndex;
  ci.nextIndex = nextIndex;
  if (isLearner)
    ci.role = Paxos::LEARNER;
  else
    ci.role = Paxos::FOLLOWER;
  ci.hasVoted = voted;
  ci.forceSync = forceSync;
  ci.electionWeight = electionWeight;
  if (!isLearner)
    ci.learnerSource = 0;
  else
    ci.learnerSource = learnerSource;
  ci.appliedIndex = getAppliedIndex();
  if (isLearner && !paxos->getEnableLearnerPipelining())
    ci.pipelining = false;
  else
    ci.pipelining = !disablePipelining;
  ci.useApplied = sendByAppliedIndex;

  cis->push_back(std::move(ci));
}

void RemoteServer::fillFollowerMeta(void* ptr) {
  if (!isLearner ||
      (learnerSource != paxos->getLocalServer()->serverId &&
       (learnerSource == 0 ||
        diffMS(lastMergeTP) > paxos->getMaxMergeReportTimeout() * 1000)))
    return;

  ::google::protobuf::RepeatedPtrField<::alisql::ClusterInfoEntry>* fms =
      static_cast<
          ::google::protobuf::RepeatedPtrField<::alisql::ClusterInfoEntry>*>(
          ptr);

  ::alisql::ClusterInfoEntry* entry = fms->Add();

  entry->set_serverid(serverId);
  entry->set_matchindex(matchIndex.load());
  entry->set_nextindex(nextIndex.load());
  entry->set_appliedindex(appliedIndex.load());
  entry->set_learnersource(learnerSource);  // for check in leader
}

void RemoteServer::setMsgCompressOption(void* ptr) {
  if (ptr == nullptr) return;
  msgCompressOption = *(MsgCompressOption*)ptr;
}

}  // namespace alisql
