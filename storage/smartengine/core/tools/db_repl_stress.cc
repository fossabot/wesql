//  Portions Copyright (c) 2023, ApeCloud Inc Holding Limited
//  Portions Copyright (c) 2020, Alibaba Group Holding Limited
//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#ifndef GFLAGS
#include <cstdio>
int main() {
  fprintf(stderr, "Please install gflags to run smartengine tools\n");
  return 1;
}
#else

#include <atomic>
#include <cstdio>

#include <gflags/gflags.h>

#include "db/db.h"
#include "util/testutil.h"
#include "write_batch/write_batch_internal.h"

// Run a thread to perform Put's.
// Another thread uses GetUpdatesSince API to keep getting the updates.
// options :
// --num_inserts = the num of inserts the first thread should perform.
// --wal_ttl = the wal ttl for the run.

using namespace smartengine;
using namespace util;
using namespace common;
using namespace db;
using namespace table;

using GFLAGS::ParseCommandLineFlags;
using GFLAGS::SetUsageMessage;

struct DataPumpThread {
  size_t no_records;
  DB* db;  // Assumption DB is Open'ed already.
};

static std::string RandomString(Random* rnd, int len) {
  std::string r;
  test::RandomString(rnd, len, &r);
  return r;
}

static void DataPumpThreadBody(void* arg) {
  DataPumpThread* t = reinterpret_cast<DataPumpThread*>(arg);
  DB* db = t->db;
  Random rnd(301);
  size_t i = 0;
  while (i++ < t->no_records) {
    if (!db->Put(WriteOptions(), Slice(RandomString(&rnd, 500)),
                 Slice(RandomString(&rnd, 500)))
             .ok()) {
      fprintf(stderr, "Error in put\n");
      exit(1);
    }
  }
}

struct ReplicationThread {
  std::atomic<bool> stop;
  DB* db;
  volatile size_t no_read;
};

//static void ReplicationThreadBody(void* arg) {
//  ReplicationThread* t = reinterpret_cast<ReplicationThread*>(arg);
//  DB* db = t->db;
//  unique_ptr<TransactionLogIterator> iter;
//  SequenceNumber currentSeqNum = 1;
//  while (!t->stop.load(std::memory_order_acquire)) {
//    iter.reset();
//    Status s;
//    while (!db->GetUpdatesSince(currentSeqNum, &iter).ok()) {
//      if (t->stop.load(std::memory_order_acquire)) {
//        return;
//      }
//    }
//    fprintf(stderr, "Refreshing iterator\n");
//    for (; iter->Valid(); iter->Next(), t->no_read++, currentSeqNum++) {
//      BatchResult res = iter->GetBatch();
//      if (res.sequence != currentSeqNum) {
//        fprintf(stderr, "Missed a seq no. b/w %ld and %ld\n",
//                (long)currentSeqNum, (long)res.sequence);
//        exit(1);
//      }
//    }
//  }
//}

DEFINE_uint64(num_inserts, 1000,
              "the num of inserts the first thread should"
              " perform.");

int main(int argc, const char** argv) {
  SetUsageMessage(
      std::string("\nUSAGE:\n") + std::string(argv[0]) +
      " --num_inserts=<num_inserts>");
  ParseCommandLineFlags(&argc, const_cast<char***>(&argv), true);

  Env* env = Env::Default();
  std::string default_db_path;
  env->GetTestDirectory(&default_db_path);
  default_db_path += "db_repl_stress";
  Options options;
  DB* db;
  DestroyDB(default_db_path, options);

  Status s = DB::Open(options, default_db_path, &db);

  if (!s.ok()) {
    fprintf(stderr, "Could not open DB due to %s\n", s.ToString().c_str());
    exit(1);
  }

  DataPumpThread dataPump;
  dataPump.no_records = FLAGS_num_inserts;
  dataPump.db = db;
  env->StartThread(DataPumpThreadBody, &dataPump);

  ReplicationThread replThread;
  replThread.db = db;
  replThread.no_read = 0;
  replThread.stop.store(false, std::memory_order_release);

  //env->StartThread(ReplicationThreadBody, &replThread);
  while (replThread.no_read < FLAGS_num_inserts)
    ;
  replThread.stop.store(true, std::memory_order_release);
  if (replThread.no_read < dataPump.no_records) {
    // no. read should be => than inserted.
    fprintf(stderr,
            "No. of Record's written and read not same\nRead : %ld"
            " Written : %ld\n",
            replThread.no_read, dataPump.no_records);
    exit(1);
  }
  fprintf(stderr, "Successful!\n");
  exit(0);
}

#endif  // GFLAGS