/************************************************************************
 *
 * Copyright (c) 2016 Alibaba.com, Inc. All Rights Reserved
 * $Id:  paxos_server.h,v 1.0 07/30/2016 03:19:53 PM
 *yingqiang.zyq(yingqiang.zyq@alibaba-inc.com) $
 *
 ************************************************************************/

/**
 * @file paxos_server.h
 * @author yingqiang.zyq(yingqiang.zyq@alibaba-inc.com)
 * @date 07/30/2016 03:19:53 PM
 * @version 1.0
 * @brief
 *
 **/

#ifndef cluster_paxos_server_INC
#define cluster_paxos_server_INC

#include <service.h>
#include <stdint.h>

#include <memory>

#include "easyNet.h"
#include "io/easy_log.h"
#include "msg_compress.h"
#include "paxos.pb.h"
#include "single_process_queue.h"
#include "thread_timer.h"

namespace alisql {
class Paxos;
typedef std::chrono::steady_clock::time_point TimePoint;
typedef std::chrono::microseconds MSType;

/**
 * @class Server
 *
 * @brief
 *
 **/
// class Server : public std::enable_shared_from_this<Server>{
class Server : public NetServer {
 public:
  Server(uint64_t serverIdArg)
      : serverId(serverIdArg),
        paxos(NULL),
        forceSync(false),
        electionWeight(5),
        learnerSource(0),
        sendByAppliedIndex(false),
        flowControl(0) {}
  virtual ~Server() {}

  virtual void beginRequestVote(void*) = 0;
  virtual void beginLeadership(void*) = 0;
  virtual void stepDown(void*) = 0;
  virtual void stop(void*) = 0;
  virtual void sendMsg(void*) = 0;
  virtual void connect(void*) = 0;
  virtual void disconnect(void*) = 0;
  virtual void fillInfo(void*) = 0;
  virtual void fillFollowerMeta(void*) = 0;
  virtual uint64_t getLastAckEpoch() const = 0;
  virtual void setLastAckEpoch(uint64_t epoch) = 0;
  virtual uint64_t getMatchIndex() const = 0;
  virtual void resetMatchIndex(uint64_t /* matchIndex */) {}
  virtual uint64_t getAppliedIndex() const = 0;
  virtual bool haveVoted() const = 0;
  virtual void setMsgCompressOption(void*) = 0;
  /* TODO:
  virtual bool isCaughtUp() const = 0;
  */
  virtual uint64_t getLastLogIndex();
  virtual uint64_t getLastCachedLogIndex();

  // const uint64_t serverId;
  uint64_t serverId;
  // std::string strAddr;
  Paxos* paxos;
  bool forceSync;
  uint electionWeight;
  uint64_t learnerSource;
  bool sendByAppliedIndex;  // default false
  int64_t flowControl;

  /*
private:
  Server ( const Server &other );   // copy constructor
  const Server& operator = ( const Server &other ); // assignment operator
  */

}; /* end of class Server */

/**
 * @class LocalServer
 *
 * @brief
 *
 **/
class LocalServer : public Server {
 public:
  LocalServer(uint64_t serverId);
  virtual ~LocalServer() {}

  virtual void beginRequestVote(void*) override {}
  virtual void beginLeadership(void*) override;
  virtual void stepDown(void*) override {}
  virtual void stop(void*) override {}
  virtual void sendMsg(void*) override;
  virtual void connect(void*) override {}
  virtual void disconnect(void*) override {}
  virtual void fillInfo(void*) override;
  virtual void fillFollowerMeta(void*) override;
  virtual uint64_t getLastAckEpoch() const override;
  virtual void setLastAckEpoch(uint64_t) override {}
  virtual uint64_t getMatchIndex() const override {
    return lastSyncedIndex.load();
  }
  virtual uint64_t getAppliedIndex() const override;
  virtual bool haveVoted() const override;
  virtual void setMsgCompressOption(void*) override {}
  /* TODO:
  virtual bool isCaughtUp() const;
  */

  virtual uint64_t appendLog(LogEntry& entry);
  virtual uint64_t writeLog(LogEntry& entry);
  virtual uint64_t writeLogDone(uint64_t logIndex);
  virtual void writeCacheLogDone();
  uint64_t writeLogDoneInternal(uint64_t logIndex, bool forceSend = false);

  std::atomic<uint64_t> lastSyncedIndex;
  /* for logType node, try more times for electionWeightAction */
  bool logType;
  /* timeout for long rtt learner (default 0) */
  uint64_t learnerConnTimeout;
  uint64_t cidx;

}; /* end of class LocalServer */

/**
 * @class AliSQLServer
 *
 * @brief
 *
 **/
class AliSQLServer : public LocalServer {
 public:
  AliSQLServer(uint64_t serverId) : LocalServer(serverId) {}
  virtual ~AliSQLServer() {}
  virtual uint64_t writeLogDone(uint64_t logIndex) override;
  void setLastNonCommitDepIndex(uint64_t logIndex);

}; /* end of class LocalServer */

class SendMsgTask;

/**
 * @class RemoteServer
 *
 * @brief
 *
 **/
class RemoteServer : public Server {
 public:
  RemoteServer(uint64_t serverId);
  virtual ~RemoteServer();

  virtual void beginRequestVote(void*) override;
  virtual void beginLeadership(void*) override;
  virtual void stepDown(void*) override;
  virtual void stop(void*) override;
  virtual void sendMsg(void*) override;
  virtual void connect(void*) override;
  virtual void disconnect(void*) override;
  virtual void fillInfo(void*) override;
  virtual void fillFollowerMeta(void*) override;
  virtual uint64_t getLastAckEpoch() const override {
    return lastAckEpoch.load();
  }
  virtual void setLastAckEpoch(uint64_t epoch) override {
    lastAckEpoch.store(epoch);
    lostConnect.store(false);
  }
  virtual uint64_t getMatchIndex() const override { return matchIndex; }
  virtual uint64_t getAppliedIndex() const override {
    return appliedIndex.load();
  }
  virtual void resetMatchIndex(uint64_t matchIndex_) override {
    matchIndex.store(matchIndex_);
    hasMatched = false;
  }
  virtual bool haveVoted() const override { return voted; }
  virtual void setMsgCompressOption(void*) override;
  /* TODO:
  virtual bool isCaughtUp() const;
  */
  void sendMsgFunc(bool lockless, bool force, void*);
  static void sendMsgFuncAsync(SendMsgTask* task);
  void sendMsgFuncInternal(bool lockless, bool force, void* ptr, bool async);
  void setAddr(NetAddressPtr addrArg) { addr = addrArg; }
  virtual void onConnectCb();
  virtual void onDisconnectCb();
  void resetNextIndex();
  static TimePoint now() { return std::chrono::steady_clock::now(); }
  static uint64_t diffMS(TimePoint tp) {
    TimePoint ntp = now();
    return std::chrono::duration_cast<std::chrono::microseconds>(ntp - tp)
        .count();
  }
  std::shared_ptr<RemoteServer> getSharedThis() {
    return std::dynamic_pointer_cast<RemoteServer>(shared_from_this());
  }

  /* sendMsgQueue may be freed in async thread. */
  std::shared_ptr<SingleProcessQueue<SendMsgTask>> sendMsgQueue;
  std::atomic<uint64_t> nextIndex;
  std::atomic<uint64_t> matchIndex;
  std::atomic<uint64_t> lastAckEpoch;
  bool voted;
  bool isLeader;
  bool isLearner;
  bool logType;
  NetAddressPtr addr;
  std::shared_ptr<Service> srv;
  bool hasMatched;
  bool needAddr;
  bool learnerReadyForAppendLog;
  std::atomic<bool> connected;
  std::atomic<bool> disablePipelining;
  std::atomic<bool> lostConnect;
  std::atomic<bool> netError;
  std::atomic<bool> isStop;
  std::unique_ptr<ThreadTimer> heartbeatTimer;
  std::atomic<uint64_t> waitForReply;
  /*
    A msg should not reset waitForReply if msgId is less than guardId.
    This variable is set if disablePipelining.
   */
  std::atomic<uint64_t> guardId;
  std::atomic<uint64_t> msgId;
  std::atomic<uint64_t> appliedIndex;
  TimePoint lastSendTP;
  TimePoint lastMergeTP;
  MsgCompressOption msgCompressOption;
  /*
    It record the last logEntry size send to this server.
    We disable pipelining whenever large entry is send.
  */
  std::atomic<uint64_t> lastEntrySize;

 protected:
  uint64_t getConnTimeout();
}; /* end of class RemoteServer */

class SendMsgTask {
 public:
  PaxosMsg* msg;
  std::shared_ptr<RemoteServer> server;
  bool force;
  SendMsgTask(PaxosMsg* m, std::shared_ptr<RemoteServer> s, bool f)
      : msg(m), server(s), force(f) {}
  ~SendMsgTask() {
    delete msg;
    msg = NULL;
  }
  bool merge(const SendMsgTask* task) {
    assert(server == task->server);
    // assert(msg->msgtype() == Paxos::AppendLog);
    // assert(task->msg->msgtype() == Paxos::AppendLog);
    if (msg->term() == task->msg->term() && !force && !task->force) {
      if (task->msg->commitindex() > msg->commitindex())
        msg->set_commitindex(task->msg->commitindex());
      return true;
    }
    return false;
  }
  void printMergeInfo(uint64_t mergedNum) {
    easy_warn_log("SendMsgTask: %llu tasks merged for server %llu", mergedNum,
                  server->serverId);
  }
};

}  // namespace alisql

#endif  //#ifndef cluster_paxos_server_INC
