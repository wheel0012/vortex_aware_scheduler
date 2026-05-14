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

#include "mem_coalescer.h"

using namespace vortex;

MemCoalescer::MemCoalescer(
  const SimContext& ctx,
  const char* name,
  uint32_t input_size,
  uint32_t output_size,
  uint32_t line_size,
  uint32_t queue_size,
  uint32_t delay
) : SimObject<MemCoalescer>(ctx, name)
  , ReqIn(this)
  , RspIn(this)
  , ReqOut(this)
  , RspOut(this)
  , input_size_(input_size)
  , output_size_(output_size)
  , output_ratio_(input_size / output_size)
  , pending_rd_reqs_(queue_size)
  , sent_mask_(input_size)
  , line_size_(line_size)
  , delay_(delay)
{}

void MemCoalescer::reset() {
  sent_mask_.reset();
}

void MemCoalescer::tick() {
  // process outgoing responses
  if (!RspOut.empty()) {
    auto& out_rsp = RspOut.front();
    DT(4, this->name() << "-mem-rsp: " << out_rsp);
    auto& entry = pending_rd_reqs_.at(out_rsp.tag);

    // Warp-wide: each output slot has its own input-thread mask (recorded
    // at request-allocation time). Unionize masks of slots covered by
    // out_rsp.mask, intersected with entry.mask (threads not yet served).
    BitVector<> rsp_mask(input_size_);
    for (uint32_t o = 0; o < output_size_; ++o) {
      if (!out_rsp.mask.test(o))
        continue;
      const auto& sm = entry.slot_masks.at(o);
      for (uint32_t i = 0; i < input_size_; ++i) {
        if (sm.test(i) && entry.mask.test(i))
          rsp_mask.set(i);
      }
    }

    // build memory response
    LsuRsp in_rsp(input_size_);
    in_rsp.mask = rsp_mask;
    in_rsp.tag = entry.tag;
    in_rsp.cid = out_rsp.cid;
    in_rsp.uuid = out_rsp.uuid;

    // send memory response
    RspIn.push(in_rsp, 1);

    // track remaining responses
    assert(!entry.mask.none());
		entry.mask &= ~rsp_mask;
		if (entry.mask.none()) {
      // whole response received, release tag
			pending_rd_reqs_.release(out_rsp.tag);
		}
    RspOut.pop();
  }

  // process incoming requests
  if (ReqIn.empty())
    return;

  auto& in_req = ReqIn.front();
  assert(in_req.mask.size() == input_size_);
  assert(!in_req.mask.none());

  // ensure we can allocate a response tag
  if (pending_rd_reqs_.full()) {
    DT(4, "*** " << this->name() << "-queue-full: " << in_req);
    return;
  }

  uint64_t addr_mask = ~uint64_t(line_size_-1);

  // -----------------------------------------------------------------
  // Warp-wide coalescing: group all unsent threads by cache-line addr,
  // then assign each group to one output slot. Output slots beyond
  // output_size_ are deferred to a later cycle (sent_mask_ accumulates).
  // -----------------------------------------------------------------
  BitVector<> out_mask(output_size_);
  std::vector<uint64_t> out_addrs(output_size_);
  std::vector<BitVector<>> slot_masks(output_size_, BitVector<>(input_size_));
  BitVector<> cur_mask(input_size_);

  // Pass 1: discover unique line addresses (preserve first-seen order via
  // a parallel vector; small input_size makes the linear scan cheap).
  std::vector<uint64_t> uniq_lines;
  uniq_lines.reserve(output_size_);
  std::vector<BitVector<>> line_threads;
  line_threads.reserve(output_size_);

  for (uint32_t i = 0; i < input_size_; ++i) {
    if (sent_mask_.test(i) || !in_req.mask.test(i))
      continue;
    uint64_t la = in_req.addrs.at(i) & addr_mask;
    // find existing group
    int found = -1;
    for (uint32_t g = 0, ng = uniq_lines.size(); g < ng; ++g) {
      if (uniq_lines[g] == la) { found = static_cast<int>(g); break; }
    }
    if (found < 0) {
      uniq_lines.push_back(la);
      line_threads.emplace_back(input_size_);
      found = static_cast<int>(line_threads.size()) - 1;
    }
    line_threads[found].set(i);
  }

  // Pass 2: assign up to output_size_ unique lines to output slots this cycle.
  for (uint32_t o = 0; o < output_size_ && o < uniq_lines.size(); ++o) {
    out_mask.set(o);
    out_addrs[o] = uniq_lines[o];
    slot_masks[o] = line_threads[o];
    cur_mask |= line_threads[o];
  }

  assert(!out_mask.none());

  uint32_t tag = 0;
  if (!in_req.write) {
    // allocate a response tag for read requests; keep per-slot masks so
    // the response fan-out can find the right input threads.
    tag = pending_rd_reqs_.allocate(pending_req_t{in_req.tag, cur_mask, slot_masks});
  }

  // build memory request
  LsuReq out_req{output_size_};
  out_req.mask = out_mask;
  out_req.tag = tag;
  out_req.write = in_req.write;
  out_req.addrs = out_addrs;
  out_req.cid = in_req.cid;
  out_req.wid = in_req.wid;
  out_req.pc = in_req.pc;
  out_req.uuid = in_req.uuid;

  // send memory request
  ReqOut.push(out_req, delay_);
  DT(4, this->name() << "-mem-req: coalesced=" << cur_mask.count() << ", " << out_req);

  // track partial responses (one tick failed to drain the full input mask)
  perf_stats_.misses += (cur_mask.count() != in_req.mask.count());

  // update sent mask
  sent_mask_ |= cur_mask;
  if (sent_mask_ == in_req.mask) {
    ReqIn.pop();
    sent_mask_.reset();
  }
}

const MemCoalescer::PerfStats& MemCoalescer::perf_stats() const {
  return perf_stats_;
}