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

#ifndef __SCHEDULER_CCWS_H
#define __SCHEDULER_CCWS_H

#include <vector>
#include <deque>
#include <algorithm>
#include "types.h"

#ifndef VX_CCWS_VTA_SIZE
#define VX_CCWS_VTA_SIZE 8
#endif

#ifndef VX_CCWS_BASE_LLS
#define VX_CCWS_BASE_LLS 100
#endif

#ifndef VX_CCWS_LLD_SCORE
#define VX_CCWS_LLD_SCORE 10
#endif

#ifndef VX_CCWS_DYNAMIC_LLDS
#define VX_CCWS_DYNAMIC_LLDS 1
#endif

#ifndef VX_CCWS_K_THROTTLE
#define VX_CCWS_K_THROTTLE 8
#endif

#ifndef VX_CCWS_MAX_LLS
#define VX_CCWS_MAX_LLS 1000000
#endif

#ifndef VX_CCWS_LLS_CUTOFF
#define VX_CCWS_LLS_CUTOFF 0
#endif

#ifndef VX_CCWS_LLS_DECAY_PERIOD
#define VX_CCWS_LLS_DECAY_PERIOD 1
#endif

#ifndef VX_CCWS_LLS_DECAY_STEP
#ifdef VX_CCWS_LLS_DECAY
#define VX_CCWS_LLS_DECAY_STEP VX_CCWS_LLS_DECAY
#else
#define VX_CCWS_LLS_DECAY_STEP 1
#endif
#endif

#ifndef VX_CCWS_MAX_ACTIVE_LOAD_WARPS
#define VX_CCWS_MAX_ACTIVE_LOAD_WARPS 0
#endif

#ifndef VX_CCWS_GATE_WHOLE_WARP
#define VX_CCWS_GATE_WHOLE_WARP 0
#endif

namespace vortex {

class SchedulerCCWS {
public:
  struct PerfStats {
    uint64_t vta_inserts;
	    uint64_t vta_hits;
	    uint64_t throttled_loads;
	    uint64_t throttled_warps;
	    uint64_t fallback_issues;
    double avg_active_issue_candidates;
    double avg_lls;
    uint64_t max_lls;

    PerfStats()
      : vta_inserts(0)
	      , vta_hits(0)
	      , throttled_loads(0)
	      , throttled_warps(0)
	      , fallback_issues(0)
      , avg_active_issue_candidates(0)
      , avg_lls(0)
      , max_lls(0)
    {}
  };

	  SchedulerCCWS(size_t num_warps);

	  bool can_issue_load(size_t wid) const;
	  bool can_issue_warp(size_t wid) const;

  void on_l1_miss(size_t wid, uint64_t line_addr);
  void on_l1_eviction(size_t owner_wid, uint64_t line_addr);
  void tick();
  void set_active_warps(uint64_t active_warps);
  void reset_warp(size_t wid);
  void reset();
	  void record_issue_candidates(uint64_t candidates);
  void record_issue();
	  void record_throttled_load();
	  void record_throttled_warp();
	  void record_fallback_issue();
  PerfStats perf_stats() const;

private:
  std::vector<int> lls_;
  std::vector<std::deque<uint64_t>> vta_;

  int base_lls_;
  int lld_score_;
  int k_throttle_;
  int cutoff_;
  int lls_decay_period_;
  int lls_decay_step_;
  size_t vta_size_;

  uint64_t vta_inserts_;
	  uint64_t vta_hits_;
	  uint64_t throttled_loads_;
	  uint64_t throttled_warps_;
  uint64_t fallback_issues_;
  uint64_t issued_insts_;
  uint64_t active_warps_;
  uint64_t active_issue_candidates_;
  uint64_t issue_candidate_samples_;
  uint64_t tick_count_;
  uint64_t lls_samples_;
  uint64_t lls_sample_sum_;
  uint64_t max_lls_;
  size_t active_load_warp_limit_;

  bool dynamic_llds_enabled() const;
  uint64_t llds_value() const;
  uint64_t cutoff_value() const;
  uint64_t lls_value(size_t wid) const;
  uint64_t cumulative_lls() const;
  size_t active_load_warp_limit() const;
  size_t rank_by_lls(size_t wid) const;
};

} // namespace vortex

#endif // __SCHEDULER_CCWS_H
