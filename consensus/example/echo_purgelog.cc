// Portions Copyright (c) 2023 ApeCloud, Inc.
// Copyright (c) 2016 Alibaba.com, Inc. All Rights Reserved

#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <thread>

#include "files.h"
#include "paxos.h"
#include "paxos.pb.h"
#include "paxos_log.h"
#include "paxos_server.h"
#include "rd_paxos_log.h"

using namespace alisql;

/*
 * Apply thread: once a log entry is committed, the apply thread will echo the
 * value of the entry. it also can be set to state machine or ack to the client
 * in KV server.
 */
void applyThread(Paxos* paxos) {
  std::shared_ptr<PaxosLog> log = paxos->getLog();
  uint64_t appliedIndex = 0;
  while (1) {
    /*uint64_t commitIndex= */ paxos->waitCommitIndexUpdate(appliedIndex);
    uint64_t i = 0;
    for (i = appliedIndex + 1; i <= paxos->getCommitIndex(); ++i) {
      LogEntry entry;
      log->getEntry(i, entry);
      if (entry.optype() > 10) continue;
      // std::cout<< "LogIndex "<<i <<": key:"<< entry.ikey()<<", value:"<<
      // entry.value()<< std::endl<< std::flush;
      std::cout << "====> CommittedMsg:" << entry.value() << ", LogIndex:" << i
                << std::endl
                << std::flush;
    }
    appliedIndex = i - 1;
    paxos->updateAppliedIndex(appliedIndex);
  }

  std::cout << "====> ApplyThread: exit." << std::endl << std::flush;
}

bool myPurgeLog(const LogEntry& le) {
  std::cout << "Purge log " << le.value() << std::endl;
  return true;
}

// How to run this example:
// 1. Add -D WITH_EXAMPLES=ON to cmake command line.
// 2. Compile the example: make echo_purgelog.
// 3. Run 3 instances of this example:
//    ./consensus/example/echo_purgelog 127.0.0.1:10001 127.0.0.1:10002
//    127.0.0.1:10003 1
//    ./consensus/example/echo_purgelog 127.0.0.1:10001 127.0.0.1:10002
//    127.0.0.1:10003 2
//    ./consensus/example/echo_purgelog 127.0.0.1:10001 127.0.0.1:10002
//    127.0.0.1:10003 3
// 4. Input some string in the first instance, and you will see the string
// echoed in the other two instances.
// 5. You will see that "Purge log " will be echoed.
int main(int argc, char* argv[]) {
  if (argc < 5) {
    std::cerr << "Usage: ./echo <ip1:port1> <ip2:port2> <ip3:port3> "
                 "<currentIndex> [<witness_ip:witness_port>]"
              << std::endl;
    std::cerr << "Example: ./echo 127.0.0.1:10001 127.0.0.1:10002 "
                 "127.0.0.1:10003 1 127.0.0.1:10004"
              << std::endl;
    return 1;
  }
  int index = atol(argv[4]);
  if (index < 1 || index > 3) {
    std::cerr << "Error: currentIndex should be 1, 2 or 3." << std::endl;
    return 1;
  }
  std::cout << "Current Instance IP:PORT " << argv[index] << std::endl;

  setenv("easy_log_level", "3", 1);
  extern easy_log_level_t easy_log_level;
  easy_log_level = easy_log_level;
  // easy_log_level= EASY_LOG_ERROR;

  /* Server list. */
  std::vector<std::string> strConfig;
  strConfig.emplace_back(argv[1]);
  strConfig.emplace_back(argv[2]);
  strConfig.emplace_back(argv[3]);

  /* You can use the RDPaxosLog (based on RocksDB) by default, you can also
   * implement a new log based on the interface PaxosLog by yourself. */

  std::string logDir = std::string("paxosLogTestDir") + strConfig[index - 1];
  deleteDir(logDir.c_str());
  std::shared_ptr<PaxosLog> rlog = std::make_shared<RDPaxosLog>(
      logDir, true, RDPaxosLog::DEFAULT_WRITE_BUFFER_SIZE);

  /* Init paxos consensus here. */
  uint64_t purgeLogTimeout = 10000;  // 10s
  Paxos* paxos1 = new Paxos(5000, rlog, purgeLogTimeout);
  // must initAutoPurgeLog before init
  paxos1->initAutoPurgeLog(true, true, myPurgeLog);
  paxos1->init(strConfig, index, new ClientService());

  /* init witness node */
  std::vector<std::string> witnesslist;
  for (int i = 5; i < argc; ++i) {
    witnesslist.emplace_back(argv[i]);
  }
  /* add learner after we have a leader */
  while (1) {
    sleep(1);
    if (paxos1->getState() == paxos1->LEADER) {
      paxos1->changeLearners(Paxos::CCAddNode, witnesslist);
    }
    if (paxos1->getCurrentLeader() != 0) {
      /* have leader */
      break;
    }
  }

  /* Start the apply thread. */
  std::thread th1(applyThread, paxos1);

  while (true) {
    std::string key = "keykey", value, line;
    std::cin >> value;
    if (paxos1->getState() != Paxos::LEADER) {
      std::cerr << "====> Error: I'm not leader!! Leader is server:"
                << paxos1->getCurrentLeader() << std::endl;
      continue;
    }

    // std::cin>> "set">> key>> "=">> value>> std::endl;
    std::cout << "====> Input value:" << value << " ." << std::endl;

    LogEntry le;
    le.set_index(0);
    le.set_optype(1);
    // le.set_ikey(key);
    le.set_value(value);

    /* Send the log entry by Paxos consensus. */
    paxos1->replicateLog(le);
  }

  th1.join();

  delete paxos1;

  return 0;
}  // function main
