/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

#include <present_frame_policy.hpp>

int main() {
  namespace policy = renodx::addons::dlss5::internal::present_frame_policy;
  uint32_t cases = 0;
  uint32_t failures = 0;
  const auto check = [&](bool condition, std::string_view message) {
    ++cases;
    if (!condition) {
      ++failures;
      std::cerr << "FAIL present frame policy: " << message << '\n';
    }
  };
  constexpr policy::Frame first{.source = 17, .generation = 4, .sequence = 1, .output_epoch = 9, .reuse_edits = true};
  policy::Clock clock;
  check(policy::Observe(&clock, {}) == policy::Claim::UNPROVEN && clock.claims == 0, "unknown source cannot claim");
  uint64_t anchor_sequence = 0;
  bool anchor_valid = false;
  bool anchor_reused = false;
  for (uint64_t sequence = 1; sequence <= 4; ++sequence) {
    auto frame = first;
    frame.sequence = sequence;
    check(policy::Observe(&clock, frame) == policy::Claim::NEW_FRAME && clock.claims == sequence,
          "one claim per new base frame");
    const auto execution = policy::Schedule(sequence, anchor_sequence, anchor_valid, anchor_reused, true);
    check(execution == (sequence % 2 == 0 ? policy::Execution::REUSE : policy::Execution::REFRESH),
          "base frames refresh and reuse in 1/0/1/0 cadence");
    anchor_reused = execution == policy::Execution::REUSE;
    anchor_sequence = sequence;
    anchor_valid = true;
    auto output = policy::Product{.model_recorded = !anchor_reused, .edit_reused = anchor_reused,
        .output_ready = true, .group_complete = true};
    const auto result = policy::Resolve(std::span{&output, 1}, 1, true);
    policy::Publish(&clock, frame, false, result.complete);
    check(!clock.complete && clock.outputs == sequence - 1, "unordered output cannot publish");
    policy::Publish(&clock, frame, true, result.complete);
    check(clock.complete && clock.outputs == sequence, "publish a complete ordered group");
    auto same_frame = frame;
    same_frame.reuse_edits = false;
    check(policy::Observe(&clock, same_frame) == policy::Claim::REPEATED && clock.claims == sequence,
          "execution setting does not grant a new source frame");
    policy::Publish(&clock, same_frame, true, true);
    check(clock.outputs == sequence, "duplicate delivery cannot advance output history");
  }
  check(policy::Observe(&clock, first) == policy::Claim::STALE && clock.frame.sequence == 4 && clock.claims == 4,
        "late frame cannot roll history back");
  auto changed = clock.frame;
  changed.generation++;
  check(policy::Observe(&clock, changed) == policy::Claim::NEW_FRAME && !clock.complete,
        "real source generation change cannot reuse the prior product");
  check(policy::NeedsReset(first, changed, 4, false, false), "source change resets its history");
  changed.roles = 3;
  check(policy::Observe(&clock, changed) == policy::Claim::NEW_FRAME && !clock.complete,
        "a new required role has its own complete group");
  auto old_role = changed;
  old_role.sequence--;
  old_role.roles = 1;
  check(policy::Observe(&clock, old_role) == policy::Claim::STALE && clock.frame.roles == 3,
        "a late frame cannot evade the source clock by changing roles");
  check(policy::NeedsReset(clock.frame, changed, changed.sequence - 1, false, true), "unknown input continuity restarts on recovery");
  auto other_producer = changed;
  other_producer.kind = policy::SourceKind::SDK_SCOPE;
  other_producer.sequence = 1;
  check(policy::Observe(&clock, other_producer) == policy::Claim::NEW_FRAME,
        "independent source namespaces may use the same generation and counter values");
  changed.output_epoch++;
  check(policy::Observe(&clock, changed) == policy::Claim::NEW_FRAME, "buffer lifetime invalidates prior delivery");

  auto finite = first;
  finite.kind = policy::SourceKind::SDK_SCOPE;
  finite.sequence_period = uint64_t{1} << 32;
  finite.sequence = finite.sequence_period;
  policy::Clock finite_clock;
  check(policy::Observe(&finite_clock, finite) == policy::Claim::NEW_FRAME, "finite source counter accepts its last value");
  auto wrapped = finite;
  wrapped.sequence = 1;
  check(policy::Observe(&finite_clock, wrapped) == policy::Claim::NEW_FRAME && finite_clock.claims == 2,
        "a real counter wrap claims the next frame without inventing a source generation");
  check(policy::NeedsReset(finite, wrapped, finite.sequence, false, false), "counter wrap restarts the model history");
  check(policy::Observe(&finite_clock, finite) == policy::Claim::STALE && finite_clock.frame.sequence == 1,
        "a late pre-wrap frame cannot roll the source clock back");
  wrapped.sequence = 2;
  check(policy::Observe(&finite_clock, wrapped) == policy::Claim::NEW_FRAME && finite_clock.claims == 3,
        "a legal frame after wrap advances normally");

  std::array products{
      policy::Product{.model_recorded = true, .output_ready = true, .group_complete = true},
      policy::Product{.prepared = true},
  };
  check(!policy::Resolve(products, 3, true).complete, "a missing or only prepared role cannot publish half a group");
  products[1] = {.edit_reused = true, .output_ready = true, .group_complete = true};
  check(!policy::Resolve(products, 3, true).complete, "one group cannot mix refresh and reuse phases");
  products[0] = products[1];
  check(policy::Resolve(products, 3, true).complete, "both ordered reused roles form a complete group");
  products[0] = {};
  check(policy::Resolve(products, 3, true, 1).complete, "actual composition may supply the missing color role");
  products[1].group_complete = false;
  check(!policy::Resolve(products, 3, true, 1).complete, "composition cannot hide an incomplete model group");
  check(policy::Schedule(7, 5, true, false, true) == policy::Execution::REFRESH, "a gap cannot reuse an old anchor");
  check(policy::Schedule(6, 5, false, false, true) == policy::Execution::REFRESH, "invalid anchor forces refresh");
  check(policy::Schedule(6, 5, true, false, false) == policy::Execution::REFRESH, "Every present does not enable x2");
  if (failures != 0) return 1;
  std::cout << "PASS present frame policy evidence=CPU cases=" << cases << '\n';
  return 0;
}
