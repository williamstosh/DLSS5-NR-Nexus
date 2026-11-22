/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <span>

namespace renodx::addons::dlss5::internal::present_frame_policy {

enum class SourceKind : uint8_t { SR, APPLICATION, SDK_SCOPE, TAGGED_RESOURCE };

struct Frame {
  SourceKind kind = SourceKind::SR;
  uintptr_t source = 0;
  uint64_t generation = 0;
  uint64_t sequence = 0;
  // Zero denotes an unbounded sequence; finite source counters are 1-based.
  uint64_t sequence_period = 0;
  uint64_t output_epoch = 0;
  uint32_t pass_begin = 0;
  uint32_t pass_count = 1;
  uint32_t roles = 1;
  bool reuse_edits = false;
};

constexpr bool Known(const Frame& frame) {
  return frame.source != 0 && frame.generation != 0 && frame.sequence != 0
      && (frame.sequence_period == 0 || frame.sequence <= frame.sequence_period)
      && frame.pass_count != 0 && frame.roles != 0;
}

constexpr bool SameProducer(const Frame& first, const Frame& second) {
  return first.kind == second.kind && first.source == second.source && first.generation == second.generation
      && first.sequence_period == second.sequence_period && first.output_epoch == second.output_epoch;
}

constexpr bool SameSource(const Frame& first, const Frame& second) {
  return SameProducer(first, second) && first.pass_begin == second.pass_begin
      && first.pass_count == second.pass_count && first.roles == second.roles;
}

enum class Claim : uint8_t { UNPROVEN, NEW_FRAME, REPEATED, STALE };

struct Clock {
  Frame frame;
  uint64_t claims = 0;
  uint64_t outputs = 0;
  // Ordered consumption is permitted; GPU completion remains an adapter receipt.
  bool complete = false;
};

constexpr Claim Classify(const Clock& clock, const Frame& frame) {
  if (!Known(frame)) return Claim::UNPROVEN;
  if (Known(clock.frame) && SameProducer(clock.frame, frame)) {
    if (frame.sequence != clock.frame.sequence) {
      if (frame.sequence_period == 0) {
        if (frame.sequence < clock.frame.sequence) return Claim::STALE;
      } else {
        const auto distance = (frame.sequence > clock.frame.sequence
            ? frame.sequence - clock.frame.sequence
            : frame.sequence_period - clock.frame.sequence + frame.sequence);
        if (distance >= frame.sequence_period / 2) return Claim::STALE;
      }
    }
    if (frame.sequence == clock.frame.sequence && SameSource(clock.frame, frame)) return Claim::REPEATED;
  }
  return Claim::NEW_FRAME;
}

inline Claim Observe(Clock* clock, const Frame& frame) {
  const auto claim = Classify(*clock, frame);
  if (claim == Claim::NEW_FRAME) {
    clock->frame = frame;
    clock->complete = false;
    ++clock->claims;
  }
  return claim;
}

constexpr bool NeedsReset(const Frame& previous, const Frame& current, uint64_t previous_output,
                          bool reset, bool input_discontinuous) {
  return reset || input_discontinuous
      || (Known(previous) && Known(current) && !SameSource(previous, current))
      || (current.sequence != 0 && previous_output != 0 && current.sequence != previous_output + 1);
}

enum class Execution : uint8_t { REFRESH, REUSE };

constexpr Execution Schedule(uint64_t sequence, uint64_t anchor_sequence, bool valid, bool reused, bool allow_reuse) {
  return (allow_reuse && valid && !reused && sequence == anchor_sequence + 1
      ? Execution::REUSE : Execution::REFRESH);
}

struct Product {
  bool prepared = false;
  bool model_invoked = false;
  bool model_recorded = false;
  bool edit_reused = false;
  bool output_ready = false;
  bool group_complete = false;
};

struct GroupResult {
  uint32_t ready_roles = 0;
  bool prepared = false;
  bool complete = false;
};

constexpr GroupResult Resolve(std::span<const Product> products, uint32_t required_roles,
                              bool same_phase, uint32_t composed_roles = 0) {
  GroupResult result{.ready_roles = composed_roles};
  bool refreshed = false;
  bool reused = false;
  for (uint32_t role = 0; role < products.size(); ++role) {
    if ((required_roles & (1u << role)) == 0) continue;
    const auto& product = products[role];
    result.prepared |= product.prepared;
    if (product.output_ready && product.group_complete && (product.model_recorded || product.edit_reused)) {
      result.ready_roles |= 1u << role;
      refreshed |= product.model_recorded;
      reused |= product.edit_reused;
    }
  }
  result.complete = required_roles != 0 && (result.ready_roles & required_roles) == required_roles
      && (refreshed || reused) && (!same_phase || !refreshed || !reused);
  return result;
}

inline void Publish(Clock* clock, const Frame& frame, bool ordered, bool complete) {
  if (!Known(frame) || Classify(*clock, frame) != Claim::REPEATED) return;
  if (ordered && complete && !clock->complete) {
    clock->complete = true;
    ++clock->outputs;
  }
}

}  // namespace renodx::addons::dlss5::internal::present_frame_policy
