// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace vortex {

class WarpSchedTrace {
public:
  struct Row {
    uint64_t cycle = 0;
    uint32_t core_id = 0;
    uint32_t issue_slot = 0;
    bool issued = false;
    int preferred_wid = -1;
    int intended_wid = -1;
    int actual_wid = -1;
    int selected_wid = -1;
    std::string pc;
    std::string inst_type;
    std::string score;
    std::string candidate_mask;
    std::string ready_mask;
    std::string ibuffer_empty_mask;
    bool ibuffer_empty = false;
    bool preferred_blocked = false;
    std::string preferred_block_reason = "none";
    bool not_ready_fallback = false;
    bool fallback = false;
    std::string stall_reason = "none";
    bool mismatch = false;
    std::string mismatch_reason = "none";
    std::string score_vector;
  };

  static void configure(bool enabled,
                        const std::string& path = "issue_trace.csv",
                        const std::string& policy = "RoundRobin") {
    configured_ = true;
    initialized_ = true;
    enabled_ = enabled;
    path_ = path.empty() ? "issue_trace.csv" : path;
    policy_ = policy.empty() ? "RoundRobin" : policy;
    header_written_ = false;
    if (file_.is_open()) {
      file_.close();
    }
  }

  static bool enabled() {
    init_from_env();
    return enabled_;
  }

  static const std::string& policy_name() {
    init_from_env();
    return policy_;
  }

  static const std::string& path() {
    init_from_env();
    return path_;
  }

  static bool userpc_only() {
    init_from_env();
    return userpc_only_;
  }

  static void write_issue(const Row& row) {
    if (!enabled())
      return;

    open();
    write_csv(policy_);
    file_ << row.cycle << ','
          << row.core_id << ','
          << row.issue_slot << ',';
    write_csv(row.issued ? "true" : "false");
    file_ << row.preferred_wid << ','
          << row.intended_wid << ','
          << row.actual_wid << ','
          << row.selected_wid << ',';
    write_csv(row.pc);
    write_csv(row.inst_type);
    write_csv(row.score);
    write_csv(row.candidate_mask);
    write_csv(row.ready_mask);
    write_csv(row.ibuffer_empty_mask);
    file_ << (row.ibuffer_empty ? "true" : "false") << ','
          << (row.preferred_blocked ? "true" : "false") << ',';
    write_csv(row.preferred_block_reason);
    file_ << (row.not_ready_fallback ? "true" : "false") << ',';
    file_ << (row.fallback ? "true" : "false") << ',';
    write_csv(row.stall_reason);
    file_ << (row.mismatch ? "true" : "false") << ',';
    write_csv(row.mismatch_reason);
    write_csv(row.score_vector, true);
  }

private:
  static void init_from_env() {
    if (initialized_)
      return;

    initialized_ = true;
    if (configured_)
      return;

    const char* enable = std::getenv("VX_TRACE_WARP_SCHED");
    enabled_ = enable && std::string(enable) != "0" && std::string(enable) != "false";

    const char* path = std::getenv("VX_TRACE_WARP_SCHED_FILE");
    if (path && path[0] != '\0') {
      path_ = path;
    }

    const char* policy = std::getenv("VX_WARP_SCHED_POLICY");
    if (policy && policy[0] != '\0') {
      policy_ = policy;
    }

    const char* userpc_only = std::getenv("VX_TRACE_WARP_SCHED_USERPC_ONLY");
    userpc_only_ = userpc_only
                && std::string(userpc_only) != "0"
                && std::string(userpc_only) != "false";
  }

  static void open() {
    if (file_.is_open())
      return;

    file_.open(path_);
    if (!file_.is_open()) {
      throw std::runtime_error("failed to open warp scheduler trace: " + path_);
    }
    write_header();
  }

  static void write_header() {
    if (header_written_)
      return;

    file_ << "policy,cycle,core_id,issue_slot,issued,preferred_wid,intended_wid,actual_wid,"
             "selected_wid,pc,inst_type,score,candidate_mask,ready_mask,"
             "ibuffer_empty_mask,ibuffer_empty,preferred_blocked,preferred_block_reason,"
             "not_ready_fallback,fallback,stall_reason,mismatch,"
             "mismatch_reason,score_vector\n";
    header_written_ = true;
  }

  static void write_csv(const std::string& value, bool last = false) {
    bool quote = false;
    for (char ch : value) {
      if (ch == ',' || ch == '"' || ch == '\n') {
        quote = true;
        break;
      }
    }
    if (quote) {
      file_ << '"';
      for (char ch : value) {
        if (ch == '"') {
          file_ << "\"\"";
        } else {
          file_ << ch;
        }
      }
      file_ << '"';
    } else {
      file_ << value;
    }
    file_ << (last ? '\n' : ',');
  }

  inline static bool configured_ = false;
  inline static bool initialized_ = false;
  inline static bool enabled_ = false;
  inline static bool userpc_only_ = false;
  inline static bool header_written_ = false;
  inline static std::string path_ = "issue_trace.csv";
  inline static std::string policy_ = "RoundRobin";
  inline static std::ofstream file_;
};

} // namespace vortex
