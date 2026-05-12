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

#include <cassert>
#include <algorithm>
#include "scheduler_ccws.h"

namespace vortex {

SchedulerCCWS::SchedulerCCWS(size_t num_warps)
  : lls_(num_warps, VX_CCWS_BASE_LLS)
  , vta_(num_warps)
  , base_lls_(VX_CCWS_BASE_LLS)
  , lld_score_(VX_CCWS_LLD_SCORE)
  , cutoff_(VX_CCWS_LLS_CUTOFF)
  , lls_decay_period_(VX_CCWS_LLS_DECAY_PERIOD)
  , lls_decay_step_(VX_CCWS_LLS_DECAY_STEP)
  , vta_size_(VX_CCWS_VTA_SIZE)
  , vta_inserts_(0)
  , vta_hits_(0)
  , throttled_loads_(0)
  , throttled_warps_(0)
  , fallback_issues_(0)
  , active_issue_candidates_(0)
  , issue_candidate_samples_(0)
  , tick_count_(0)
  , lls_samples_(0)
  , lls_sample_sum_(0)
  , max_lls_(0)
  , active_load_warp_limit_(
      num_warps ? std::max<size_t>(1,
        std::min<size_t>(VX_CCWS_MAX_ACTIVE_LOAD_WARPS, num_warps)) : 0)
{
}

bool SchedulerCCWS::can_issue_warp(size_t wid) const {
  if (wid >= lls_.size())
    return true;

  uint64_t total_lls = this->cumulative_lls();

  // No detected lost locality.
  // Do not throttle.
  if (total_lls == 0)
    return true;

  // If a cumulative cutoff is set and the whole core is below it, keep all loads enabled.
  if (cutoff_ > 0 && total_lls <= static_cast<uint64_t>(cutoff_))
    return true;

  // Allow only the high-LLS prefix selected by cumulative LLS.
  return this->rank_by_lls(wid) < this->active_load_warp_limit();
}

bool SchedulerCCWS::can_issue_load(size_t wid) const {
  return this->can_issue_warp(wid);
}

void SchedulerCCWS::on_l1_miss(size_t wid, uint64_t line_addr) {
  if (wid >= lls_.size())
    return;

  // Probe VTA on a real L1 miss. VTA insertion happens only on eviction.
  auto& entries = vta_.at(wid);
  auto it = std::find(entries.begin(), entries.end(), line_addr);

  if (it != entries.end()) {
    // VTA hit = lost locality detected
    entries.erase(it);          // 중요
    ++vta_hits_;

    lls_.at(wid) = std::max<int>(lls_.at(wid), lld_score_);
    max_lls_ = std::max<uint64_t>(max_lls_, static_cast<uint64_t>(lls_.at(wid)));
  }
}

void SchedulerCCWS::on_l1_eviction(size_t owner_wid, uint64_t line_addr) {
  if (owner_wid >= vta_.size())
    return;

  // Record replaced lines in VTA for their owner warp.
  auto& entries = vta_.at(owner_wid);

  auto it = std::find(entries.begin(), entries.end(), line_addr);
  if (it != entries.end()) {
    entries.erase(it);
  }

  entries.push_back(line_addr);
  ++vta_inserts_;

  if (entries.size() > vta_size_) {
    entries.pop_front();
  }
}

void SchedulerCCWS::tick() {
  ++tick_count_;
  uint64_t lls_sum = 0;
  if (lls_decay_period_ > 0 &&
      lls_decay_step_ > 0 &&
      (tick_count_ % static_cast<uint64_t>(lls_decay_period_)) == 0) {
    for (auto &score : lls_) {
      score = std::max<int>(base_lls_, score - lls_decay_step_);
    }
  }
  for (size_t wid = 0, nw = lls_.size(); wid < nw; ++wid) {
    lls_sum += this->lls_value(wid);
  }
  lls_sample_sum_ += lls_sum;
  ++lls_samples_;
}

void SchedulerCCWS::reset_warp(size_t wid) {
  if (wid >= lls_.size())
    return;

  lls_.at(wid) = base_lls_;
  vta_.at(wid).clear();
}

void SchedulerCCWS::reset() {
  for (size_t wid = 0, nw = lls_.size(); wid < nw; ++wid) {
    this->reset_warp(wid);
  }
  vta_inserts_ = 0;
  vta_hits_ = 0;
  throttled_loads_ = 0;
  throttled_warps_ = 0;
  fallback_issues_ = 0;
  active_issue_candidates_ = 0;
  issue_candidate_samples_ = 0;
  tick_count_ = 0;
  lls_samples_ = 0;
  lls_sample_sum_ = 0;
  max_lls_ = 0;
}

void SchedulerCCWS::record_issue_candidates(uint64_t candidates) {
  active_issue_candidates_ += candidates;
  ++issue_candidate_samples_;
}

void SchedulerCCWS::record_throttled_load() {
  ++throttled_loads_;
}

void SchedulerCCWS::record_throttled_warp() {
  ++throttled_warps_;
}

void SchedulerCCWS::record_fallback_issue() {
  ++fallback_issues_;
}

SchedulerCCWS::PerfStats SchedulerCCWS::perf_stats() const {
  PerfStats perf;
  perf.vta_inserts = vta_inserts_;
  perf.vta_hits = vta_hits_;
  perf.throttled_loads = throttled_loads_;
  perf.throttled_warps = throttled_warps_;
  perf.fallback_issues = fallback_issues_;
  perf.avg_active_issue_candidates =
      issue_candidate_samples_ ? ((double)active_issue_candidates_ / issue_candidate_samples_) : 0.0;
  //printf("LLS_SAMPLESUM_%d samples %d size %d\n", lls_sample_sum_, lls_samples_, lls_.size());
  perf.avg_lls = (lls_samples_ && !lls_.empty()) ? ((double)lls_sample_sum_ / (lls_samples_ * lls_.size())) : 0.0;
  perf.max_lls = max_lls_;
  return perf;
}


uint64_t SchedulerCCWS::lls_value(size_t wid) const {
  if (wid >= lls_.size())
    return 0;
  return static_cast<uint64_t>(std::max<int>(0, lls_.at(wid) - base_lls_));
}

uint64_t SchedulerCCWS::cumulative_lls() const {
  uint64_t total = 0;
  for (size_t wid = 0, nw = lls_.size(); wid < nw; ++wid) {
    total += this->lls_value(wid);
  }
  return total;
}

size_t SchedulerCCWS::active_load_warp_limit() const {
  if (lls_.empty())
    return 0;

  size_t hard_limit = active_load_warp_limit_;

  if (cutoff_ <= 0)
    return hard_limit;

  uint64_t cumulative = 0;
  size_t selected = 0;
  for (size_t rank = 0; rank < lls_.size(); ++rank) {
    size_t ranked_wid = lls_.size();
    for (size_t wid = 0; wid < lls_.size(); ++wid) {
      if (this->rank_by_lls(wid) == rank) {
        ranked_wid = wid;
        break;
      }
    }
    if (ranked_wid == lls_.size())
      break;

    uint64_t score = this->lls_value(ranked_wid);
    if (selected != 0 && cumulative + score > static_cast<uint64_t>(cutoff_))
      break;

    cumulative += score;
    ++selected;

    if (cumulative == static_cast<uint64_t>(cutoff_))
      break;
  }

  selected = std::max<size_t>(selected, 1);
  return std::min<size_t>(selected, hard_limit);
}

size_t SchedulerCCWS::rank_by_lls(size_t wid) const {
    if (wid >= lls_.size())
        return lls_.size();

    const auto my_score = this->lls_value(wid);

    size_t rank = 0;

    for (size_t other = 0; other < lls_.size(); ++other) {
        if (other == wid)
        continue;

        const auto other_score = this->lls_value(other);

        // Higher LLS has higher priority.
        if (other_score > my_score) {
        ++rank;
        continue;
        }

        // Tie-breaker: lower WID gets higher priority.
        // This makes the ranking deterministic.
        if (other_score == my_score && other < wid) {
        ++rank;
        continue;
        }
    }

    return rank;
}

} // namespace vortex
