// syntony-test.cpp
// Mu crash-consensus throughput benchmark for the Syntony NSL testbed.
// Same logic as external/mu/crash-consensus/demo/src/main-st.cpp,
// with YAML config file support.
//
// Usage:
//   syntony-test -c mu.yaml -c mu_nsl-node1.yaml
//   syntony-test <node_id> <payload_size> <outstanding_req>  (legacy)

#include <chrono>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <yaml-cpp/yaml.h>

#include <dory/crash-consensus.hpp>

#include "helpers.hpp"
#include "timers.h"

// Thread names set by Mu internally (must match ConsensusConfig in config.hpp).
static constexpr const char* kHandoverThread   = "thd_handover";
static constexpr const char* kConsensusThread  = "thd_consensus";
static constexpr const char* kSwitcherThread   = "thd_switcher";
static constexpr const char* kHeartbeatThread  = "thd_heartbeat";
static constexpr const char* kFollowerThread   = "thd_follower";
static constexpr const char* kFileWatcherThread = "thd_filewatcher";

/**
 * Re-pin Mu's internal threads to the cores specified in name_to_core.
 * Mu pins threads during startup (to default cores 0,2,4,6,8,10); calling
 * this after commitHandler() overrides those assignments.
 */
static void repin_threads(
    const std::unordered_map<std::string, int>& name_to_core) {
  DIR* dir = opendir("/proc/self/task");
  if (!dir) {
    std::cerr << "Warning: cannot open /proc/self/task for thread repinning\n";
    return;
  }

  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    if (ent->d_name[0] == '.') continue;

    std::string comm_path =
        std::string("/proc/self/task/") + ent->d_name + "/comm";
    std::ifstream f(comm_path);
    if (!f) continue;

    std::string name;
    std::getline(f, name);

    auto it = name_to_core.find(name);
    if (it == name_to_core.end()) continue;

    pid_t tid = static_cast<pid_t>(std::stoi(ent->d_name));
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(it->second, &mask);
    if (sched_setaffinity(tid, sizeof(mask), &mask) != 0) {
      std::cerr << "Warning: failed to repin thread " << name << " to core "
                << it->second << "\n";
    } else {
      std::cout << "Repinned thread " << name << " -> core " << it->second
                << "\n";
    }
  }
  closedir(dir);
}

void benchmark(int id, std::vector<int> remote_ids, int times, int payload_size,
               int outstanding_req, int latency_buf_sz, int profiling_start,
               const std::unordered_map<std::string, int>& thread_pins);

int main(int argc, char* argv[]) {
  constexpr int nr_procs = 3;
  constexpr int minimum_id = 1;
  int id = 0, payload_size = 64, outstanding_req = 8;
  int latency_buf_sz = 0, profiling_start = 0;

  bool pin_threads = true;
  int handover_core    = 0;
  int consensus_core   = 2;
  int switcher_core    = 4;
  int heartbeat_core   = 6;
  int follower_core    = 8;
  int filewatcher_core = 10;

  // Collect -c <file> arguments (multiple allowed, merged in order)
  std::vector<std::string> config_files;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "-c" && i + 1 < argc) {
      config_files.push_back(argv[++i]);
    }
  }

  if (!config_files.empty()) {
    for (const auto& path : config_files) {
      YAML::Node cfg = YAML::LoadFile(path);
      if (cfg["node_id"])         id              = cfg["node_id"].as<int>();
      if (cfg["payload_size"])    payload_size    = cfg["payload_size"].as<int>();
      if (cfg["outstanding_req"]) outstanding_req = cfg["outstanding_req"].as<int>();
      if (cfg["latency_buf_sz"]) latency_buf_sz = cfg["latency_buf_sz"].as<int>();
      if (cfg["profiling_start"]) profiling_start = cfg["profiling_start"].as<int>();
      if (cfg["pin_threads"])      pin_threads      = cfg["pin_threads"].as<bool>();
      if (cfg["handover_core"])    handover_core    = cfg["handover_core"].as<int>();
      if (cfg["consensus_core"])   consensus_core   = cfg["consensus_core"].as<int>();
      if (cfg["switcher_core"])    switcher_core    = cfg["switcher_core"].as<int>();
      if (cfg["heartbeat_core"])   heartbeat_core   = cfg["heartbeat_core"].as<int>();
      if (cfg["follower_core"])    follower_core    = cfg["follower_core"].as<int>();
      if (cfg["filewatcher_core"]) filewatcher_core = cfg["filewatcher_core"].as<int>();
    }
    if (id == 0) {
      throw std::runtime_error("node_id not set in config");
    }
  } else {
    // Legacy positional args
    if (argc < 4) {
      throw std::runtime_error(
          "Usage: syntony-test -c global.yaml -c node.yaml\n"
          "   or: syntony-test <id> <payload_size> <outstanding_req>");
    }
    id              = atoi(argv[1]);
    payload_size    = atoi(argv[2]);
    outstanding_req = atoi(argv[3]);
  }

  constexpr int maximum_id = minimum_id + nr_procs - 1;
  if (id < minimum_id || id > maximum_id) {
    throw std::runtime_error("Invalid id: must be between " +
                             std::to_string(minimum_id) + " and " +
                             std::to_string(maximum_id));
  }

  std::cout << "USING PAYLOAD SIZE = " << payload_size << std::endl;
  std::cout << "USING OUTSTANDING_REQ = " << outstanding_req << std::endl;

  std::unordered_map<std::string, int> thread_pins;
  if (pin_threads) {
    thread_pins[kHandoverThread]    = handover_core;
    thread_pins[kConsensusThread]   = consensus_core;
    thread_pins[kSwitcherThread]    = switcher_core;
    thread_pins[kHeartbeatThread]   = heartbeat_core;
    thread_pins[kFollowerThread]    = follower_core;
    thread_pins[kFileWatcherThread] = filewatcher_core;
    std::cout << "THREAD PINNING: consensus=" << consensus_core
              << " follower=" << follower_core
              << " switcher=" << switcher_core
              << " heartbeat=" << heartbeat_core
              << " handover=" << handover_core
              << " filewatcher=" << filewatcher_core << std::endl;
  } else {
    std::cout << "THREAD PINNING: disabled" << std::endl;
  }

  std::vector<int> remote_ids;
  for (int i = 0, min_id = minimum_id; i < nr_procs; i++, min_id++) {
    if (min_id != id) remote_ids.push_back(min_id);
  }

  const int times =
      static_cast<int>(1.5 * 1024) * 1024 * 1024 / (payload_size + 64);
  benchmark(id, remote_ids, times, payload_size, outstanding_req, latency_buf_sz,
           profiling_start, thread_pins);

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(60));
  }

  return 0;
}

void benchmark(int id, std::vector<int> remote_ids, int times, int payload_size,
               int outstanding_req, int latency_buf_sz, int profiling_start,
               const std::unordered_map<std::string, int>& thread_pins) {
  dory::Consensus consensus(id, remote_ids, outstanding_req, dory::ThreadBank::A);
  consensus.commitHandler([]([[maybe_unused]] bool leader,
                             [[maybe_unused]] uint8_t* buf,
                             [[maybe_unused]] size_t len) {});

  // commitHandler() starts Mu's internal threads. Give them a moment to start
  // and set their names before re-pinning.
  if (!thread_pins.empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    repin_threads(thread_pins);
  }

  // Wait enough time for the consensus to become ready
  std::cout << "Wait some time" << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(5 + 3 - id));

  if (id == 1) {
    TIMESTAMP_INIT;

    std::vector<uint8_t> payload_buffer(payload_size + 2);
    uint8_t* payload = &payload_buffer[0];

    std::vector<TIMESTAMP_T> timestamps_start(times);
    std::vector<std::pair<int, TIMESTAMP_T>> timestamps_ranges(times);
    TIMESTAMP_T loop_time;

    mkrndstr_ipa(payload_size, payload);
    consensus.propose(payload, payload_size);

    int offset = 2;

    std::vector<std::vector<uint8_t>> payloads(8192);
    for (size_t i = 0; i < payloads.size(); i++) {
      payloads[i].resize(payload_size);
      mkrndstr_ipa(payload_size, &(payloads[i][0]));
    }

    std::cout << "Started" << std::endl;

    TIMESTAMP_T start_meas, end_meas;

    bool profiling_started = (profiling_start == 0);
    GET_TIMESTAMP(start_meas);
    for (int i = 0; i < times; i++) {
      GET_TIMESTAMP(timestamps_start[i]);

      dory::ProposeError err;
      if ((err = consensus.propose(&(payloads[i % 8192][0]), payload_size)) !=
          dory::ProposeError::NoError) {
        i -= 1;
        switch (err) {
          case dory::ProposeError::FastPath:
          case dory::ProposeError::FastPathRecyclingTriggered:
          case dory::ProposeError::SlowPathCatchFUO:
          case dory::ProposeError::SlowPathUpdateFollowers:
          case dory::ProposeError::SlowPathCatchProposal:
          case dory::ProposeError::SlowPathUpdateProposal:
          case dory::ProposeError::SlowPathReadRemoteLogs:
          case dory::ProposeError::SlowPathWriteAdoptedValue:
          case dory::ProposeError::SlowPathWriteNewValue:
            std::cout << "Error: in leader mode. Code: "
                      << static_cast<int>(err) << std::endl;
            break;

          case dory::ProposeError::SlowPathLogRecycled:
            std::cout << "Log recycled, waiting a bit..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
            break;

          case dory::ProposeError::MutexUnavailable:
          case dory::ProposeError::FollowerMode:
            std::cout << "Error: in follower mode. Potential leader: "
                      << consensus.potentialLeader() << std::endl;
            break;

          default:
            std::cout << "Bug in code. You should only handle errors here"
                      << std::endl;
        }
      }

      // Poll replication progress (same as main-st-lat.cpp)
      GET_TIMESTAMP(loop_time);
      auto [id_posted, id_replicated] = consensus.proposedReplicatedRange();
      (void)id_posted;
      timestamps_ranges[i] =
          std::make_pair(static_cast<int>(id_replicated - offset), loop_time);

      if (!profiling_started && i >= profiling_start) {
        profiling_started = true;
        GET_TIMESTAMP(start_meas);
      }
    }
    GET_TIMESTAMP(end_meas);
    int profiled_count = times - profiling_start;
    std::cout << "Replicated " << profiled_count << " commands of size "
              << payload_size << " bytes in "
              << ELAPSED_NSEC(start_meas, end_meas) << " ns" << std::endl;

    // Post-process: match proposals to replication timestamps,
    // skipping the first profiling_start proposals as warmup.
    const int lat_cap = latency_buf_sz > 0 ? latency_buf_sz : 0;
    int start_range = 0;
    int lat_cnt = 0;

    if (lat_cap > 0) {
      std::cout << "Latency samples:";
      __uint128_t lat_sum = 0;
      for (int i = 0; i < times && lat_cnt < lat_cap; i++) {
        auto [last_id, timestamp] = timestamps_ranges[i];
        for (int j = start_range; j < last_id && lat_cnt < lat_cap; j++) {
          if (j >= profiling_start) {
            uint64_t lat = ELAPSED_NSEC(timestamps_start[j], timestamp);
            std::cout << " " << lat;
            lat_sum += lat;
            lat_cnt++;
          }
        }
        if (start_range < last_id) {
          start_range = last_id;
        }
      }
      std::cout << std::endl;

      if (lat_cnt > 0) {
        uint64_t lat_avg = static_cast<uint64_t>(lat_sum / lat_cnt);
        std::cout << "Average latency: " << lat_avg << " ns (" << lat_cnt
                  << " samples)" << std::endl;
      }
    }

    exit(0);
  }
}
