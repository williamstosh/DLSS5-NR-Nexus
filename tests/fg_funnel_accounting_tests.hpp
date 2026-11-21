#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <vector>

namespace funnel_accounting_tests {
namespace nr = renodx::addons::dlss5::internal;
namespace dlss5 = renodx::addons::dlss5;
static NVSDK_NGX_Result result = NVSDK_NGX_Result_Success;
static bool delegate = false;

NVSDK_NGX_Result NVSDK_CONV Evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
  return result;
}
NVSDK_NGX_Result NVSDK_CONV EvaluateC(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback_C) {
  if (delegate) return nr::HookedEvaluateFeatureSlot<0>(list, handle, parameters, nullptr);
  return result;
}

// Restore test-owned substitutions and observations before the native FG matrix.
struct Restore {
  std::vector<std::function<void()>> actions;
  template <typename T> void Atomic(std::atomic<T>* value) {
    const T saved = value->load();
    actions.emplace_back([value, saved] { value->store(saved); });
  }
  template <typename T> void Value(T* value) {
    const T saved = *value;
    actions.emplace_back([value, saved] { *value = saved; });
  }
  ~Restore() {
    for (auto it = actions.rbegin(); it != actions.rend(); ++it) (*it)();
  }
};

void Run(Gpu* gpu) {
  Restore restore;
  restore.Value(&result);
  restore.Value(&delegate);
  restore.Value(&nr::ngx_slot_real[0]);
  restore.Value(&nr::created_feature_ids);
  restore.Atomic(&nr::logged_skip_ids);
  restore.Value(&renodx::addons::dlss5::evaluate_chain);
  restore.Value(&nr::inside_game_evaluate_wrapper);
  restore.Value(&nr::telemetry_window_nr_calls);
  restore.Value(&nr::telemetry_window_nr_ns);
  restore.Value(&nr::telemetry_window_nr_max_ns);
  restore.Value(&captured_logs);
  restore.Atomic(&nr::ngx_loader_published);
  restore.Atomic(&nr::enabled);
  restore.Atomic(&nr::hook_point);
  restore.Atomic(&nr::d3d12_device_seen);
  restore.Atomic(&nr::cmd_hooks_ever_installed);
  restore.Atomic(&nr::ngx_ever_hooked);
  restore.Atomic(&nr::last_present_steady_ns);
  restore.Atomic(&nr::lifecycle_tick_owed_ns);
  restore.Atomic(&nr::evaluates_since_present);
  restore.Atomic(&nr::intercepted_evaluations);
  restore.Atomic(&nr::own_evaluations);
  restore.Atomic(&nr::evaluations_in_flight);
  restore.Atomic(&nr::successful_evaluations);
  restore.Atomic(&nr::successful_pre_sr_evaluations);
  restore.Atomic(&nr::present_frames_seen);
  restore.Atomic(&nr::present_prefg_attempts);
  restore.Atomic(&nr::native_fg_owns);
  restore.Atomic(&nr::ngx_entry_evaluate[0]);
  restore.Atomic(&nr::ngx_entry_evaluate_c[0]);
  restore.Atomic(&nr::logged_first_evaluate);
  restore.Atomic(&nr::ngx_dlss_evaluate_seen);
  restore.Atomic(&nr::gate_ever_opened);
  restore.Atomic(&nr::logged_state_target_armed);
  restore.Atomic(&nr::motion_checked_evaluates);
  restore.Atomic(&nr::motion_missing_evaluates);
  for (auto& count : dlss5::nr_decline_counts) restore.Atomic(&count);
  nr::NgxLoaderSymbols loader;
  loader.nr_ready = true;
  loader.core_ready = true;
  nr::ngx_loader_published.store(&loader);
  nr::last_present_steady_ns.store(nr::SteadyNowNs());
  nr::lifecycle_tick_owed_ns.store(0);
  nr::d3d12_device_seen.store(true);
  nr::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(Evaluate);
  nr::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(EvaluateC);
  uint32_t sr_key = 0, fg_key = 0;
  const auto* sr = reinterpret_cast<const NVSDK_NGX_Handle*>(&sr_key);
  const auto* fg = reinterpret_cast<const NVSDK_NGX_Handle*>(&fg_key);
  nr::created_feature_ids[sr] = nr::kFeatureDlss;
  nr::created_feature_ids[fg] = NVSDK_NGX_Feature_FrameGeneration;
  renodx::addons::dlss5::bridge::OwnedParameters parameters;
  auto color = gpu->Texture();
  parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
  parameters.Set(NVSDK_NGX_Parameter_Output, color.Get());
  parameters.Set(NVSDK_NGX_Parameter_MotionVectors, color.Get());
  parameters.Set(NVSDK_NGX_Parameter_Depth, color.Get());
  unsigned failures = 0;
  const auto check = [&](bool ok, const char* message) {
    std::cout << "FUNNEL " << (ok ? "PASS " : "FAIL ") << message << '\n';
    if (!ok) ++failures;
  };
  for (const auto hook : {nr::kHookUpscaled, nr::kHookRender}) {
    nr::hook_point.store(hook);
    for (int entry = 0; entry < 3; ++entry) {
      delegate = entry == 2;
      std::cout << "FUNNEL hook=" << hook << " entry=" << entry << '\n';
      const auto call = [&](const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* block) {
        if (entry == 0) return nr::HookedEvaluateFeatureSlot<0>(gpu->list.Get(), handle, block, nullptr);
        return nr::HookedEvaluateFeatureCSlot<0>(gpu->list.Get(), handle, block, nullptr);
      };
      const auto excluded = [&](const char* name, const NVSDK_NGX_Handle* handle,
                                const NVSDK_NGX_Parameter* block, dlss5::NrDeclineReason reason, bool own) {
        const auto before = nr::CollectFunnelInputs();
        const auto count = dlss5::nr_decline_counts[static_cast<size_t>(reason)].load();
        const auto own_count = nr::own_evaluations.load();
        NVSDK_NGX_Result returned;
        if (own) {
          dlss5::DirectCallScope scope;
          returned = call(handle, block);
        } else {
          returned = call(handle, block);
        }
        const auto after = nr::CollectFunnelInputs();
        check(returned == result && after.seen == before.seen + 1
                  && after.eligible == before.eligible && after.injected == before.injected
                  && after.unaccounted == before.unaccounted
                  && nr::own_evaluations.load() == own_count + (own ? 1 : 0)
                  && dlss5::nr_decline_counts[static_cast<size_t>(reason)].load() == count + (own ? 0 : 1), name);
      };
      nr::enabled.store(true);
      result = NVSDK_NGX_Result_Success;
      excluded("FG excluded exactly once", fg, &parameters, dlss5::NrDeclineReason::kNgxNotDlssEvaluation, false);
      excluded("own excluded exactly once", fg, &parameters, dlss5::NrDeclineReason::kNgxNotDlssEvaluation, true);
      // A known FG handle avoids any pre-SR injection before the real failure/off terminal.
      result = NVSDK_NGX_Result_FAIL_InvalidParameter;
      excluded("game_failed excluded", fg, &parameters, dlss5::NrDeclineReason::kGameEvaluateFailed, false);
      result = NVSDK_NGX_Result_Success;
      nr::enabled.store(false);
      excluded("nr_off excluded", fg, &parameters, dlss5::NrDeclineReason::kNrDisabledEvaluation, false);
      nr::enabled.store(true);
      excluded("null handle malformed excluded", nullptr, &parameters, dlss5::NrDeclineReason::kMalformedEvaluate, false);
      excluded("null parameters malformed excluded", sr, nullptr, dlss5::NrDeclineReason::kMalformedEvaluate, false);
      if (hook != nr::kHookRender) continue;
      for (const char* missing : {NVSDK_NGX_Parameter_Depth, NVSDK_NGX_Parameter_MotionVectors}) {
        parameters.Set(missing, static_cast<ID3D12Resource*>(nullptr));
        const auto before = nr::CollectFunnelInputs();
        const auto count = dlss5::nr_decline_counts[static_cast<size_t>(dlss5::NrDeclineReason::kNgxMissingGuides)].load();
        call(sr, &parameters);
        const auto after = nr::CollectFunnelInputs();
        check(after.eligible == before.eligible + 1 && after.injected == before.injected
                  && after.unaccounted == before.unaccounted
                  && dlss5::nr_decline_counts[static_cast<size_t>(dlss5::NrDeclineReason::kNgxMissingGuides)].load() == count + 1,
              "missing guide remains eligible with one reason");
        parameters.Set(missing, color.Get());
      }
    }
  }
  for (const auto hook : {nr::kHookUpscaled, nr::kHookRender}) {
    for (auto& count : dlss5::nr_decline_counts) count.store(0);
    nr::intercepted_evaluations.store(400);
    nr::own_evaluations.store(0);
    nr::evaluations_in_flight.store(0);
    nr::successful_evaluations.store(400);
    nr::successful_pre_sr_evaluations.store(0);
    nr::present_frames_seen.store(0);
    nr::present_prefg_attempts.store(0);
    nr::native_fg_owns.store(false);
    nr::hook_point.store(hook);
    const auto card = [&] {
      auto in = nr::CollectFunnelInputs();
      in.hooks_enabled = in.nr_enabled = in.runtime_available = true;
      in.ngx_hooked = in.list_hooks_live = in.dlss_feature_seen = true;
      in.nr_feature_ready = in.gate_ever_opened = true;
      return dlss5::ui::ComputeStatusCard(
          {.verdict = dlss5::ComputeNrVerdict(in), .active_features = 1,
              .pre_sr = hook == nr::kHookRender});
    };
    check(card().id == dlss5::ui::CardId::kActive, "inline mode initially active");
    nr::hook_point.store(nr::kHookPresent);
    nr::native_fg_owns.store(true);
    bool yielded = true;
    for (uint32_t present = 0; present < 500; ++present) {
      nr::present_frames_seen.fetch_add(1);
      yielded &= nr::YieldPresentToNativeFg();
    }
    check(yielded && dlss5::nr_decline_counts[
              static_cast<size_t>(dlss5::NrDeclineReason::kPresentPreFg)].load() == 500,
          "native FG normal Present handoff names one ineligible terminal");
    nr::hook_point.store(hook);
    nr::intercepted_evaluations.fetch_add(100);
    nr::successful_evaluations.fetch_add(100);
    const auto after = nr::CollectFunnelInputs();
    check(!nr::YieldPresentToNativeFg() && after.seen == 1000 && after.eligible == 500
              && after.injected == 500 && after.unaccounted == 0
              && card().id == dlss5::ui::CardId::kActive,
          "inline to native Present and back remains active without resetting history");
    for (uint32_t skipped = 0; skipped < 50; ++skipped) {
      nr::intercepted_evaluations.fetch_add(1);
      dlss5::CountNrDecline(dlss5::NrDeclineReason::kNgxMissingGuides);
    }
    check(card().id == dlss5::ui::CardId::kPartlyActive,
          "real eligible skips still report partial activity after mode switching");
  }
  {
    namespace xefg = dlss5::fg::xefg;
    Restore present_restore;
    present_restore.Value(&nr::ngx_slot_real[0]);
    present_restore.Value(&nr::ngx_slot_real[1]);
    present_restore.Atomic(&nr::ngx_entry_evaluate[1]);
    present_restore.Atomic(&nr::ngx_entry_evaluate_c[1]);
    present_restore.Atomic(&nr::present_guides);
    present_restore.Atomic(&nr::debug_guide_mode);
    present_restore.Atomic(&nr::present_unusable);
    present_restore.Atomic(&nr::present_unpresented_evaluates);
    present_restore.Atomic(&nr::present_prefg_upscaled_active);
    present_restore.Atomic(&nr::synthetic_tick_active);
    present_restore.Atomic(&nr::dedupe_mode);
    present_restore.Atomic(&xefg::internal::owns_stream);
    nr::enabled.store(true);
    nr::hook_point.store(nr::kHookPresent);
    nr::native_fg_owns.store(false);
    nr::present_guides.store(nr::kPresentGuidesRequired);
    nr::debug_guide_mode.store(0);
    nr::present_unusable.store(false);
    nr::present_unpresented_evaluates.store(0);
    nr::present_prefg_upscaled_active.store(false);
    nr::synthetic_tick_active.store(false);
    nr::dedupe_mode.store(dlss5::kDedupePerEvaluate);
    xefg::internal::owns_stream.store(false);

    static uint32_t game_evaluates = 0;
    present_restore.Value(&game_evaluates);
    const auto original = +[](ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                             PFN_NVSDK_NGX_ProgressCallback) -> NVSDK_NGX_Result {
      ++game_evaluates;
      return NVSDK_NGX_Result_Success;
    };
    const auto nested = +[](ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                           const NVSDK_NGX_Parameter* block, PFN_NVSDK_NGX_ProgressCallback callback) -> NVSDK_NGX_Result {
      return nr::HookedEvaluateFeatureSlot<1>(list, handle, block, callback);
    };
    const auto nested_c = +[](ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                             const NVSDK_NGX_Parameter* block, PFN_NVSDK_NGX_ProgressCallback) -> NVSDK_NGX_Result {
      return nr::HookedEvaluateFeatureCSlot<1>(list, handle, block, nullptr);
    };
    nr::ngx_slot_real[1].evaluate_c = reinterpret_cast<void*>(+[](ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
        const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C) -> NVSDK_NGX_Result {
      ++game_evaluates;
      return NVSDK_NGX_Result_Success;
    });
    auto motion = gpu->Texture();
    auto depth = gpu->Texture();
    gpu->Transition(motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu->Transition(depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    dlss5::bridge::OwnedParameters present_parameters;
    present_parameters.Set(NVSDK_NGX_Parameter_Color, color.Get());
    present_parameters.Set(NVSDK_NGX_Parameter_Output, color.Get());
    present_parameters.Set(NVSDK_NGX_Parameter_MotionVectors, motion.Get());
    present_parameters.Set(NVSDK_NGX_Parameter_Depth, depth.Get());
    enum class Entry { CPP_TO_CPP, C_TO_CPP, CPP_TO_C };
    for (const auto entry : {Entry::CPP_TO_CPP, Entry::C_TO_CPP, Entry::CPP_TO_C}) {
      const bool c_export = entry == Entry::C_TO_CPP;
      const bool c_delegate = entry == Entry::CPP_TO_C;
      nr::ngx_slot_real[0].evaluate = reinterpret_cast<void*>(c_export ? original : (c_delegate ? nested_c : nested));
      nr::ngx_slot_real[0].evaluate_c = reinterpret_cast<void*>(EvaluateC);
      nr::ngx_slot_real[1].evaluate = reinterpret_cast<void*>(original);
      delegate = true;
      const auto before = nr::CollectFunnelInputs();
      const auto calls = game_evaluates;
      const auto cpp0 = nr::ngx_entry_evaluate[0].load();
      const auto cpp1 = nr::ngx_entry_evaluate[1].load();
      const auto c0 = nr::ngx_entry_evaluate_c[0].load();
      const auto c1 = nr::ngx_entry_evaluate_c[1].load();
      const auto mirrors = dlss5::nr_decline_counts[static_cast<size_t>(dlss5::NrDeclineReason::kNestedMirror)].load();
      const auto presents = dlss5::nr_decline_counts[static_cast<size_t>(dlss5::NrDeclineReason::kPresentHook)].load();
      const auto captures = nr::present_capture_serial;
      const auto returned = (c_export
          ? nr::HookedEvaluateFeatureCSlot<0>(gpu->list.Get(), sr, &present_parameters, nullptr)
          : nr::HookedEvaluateFeatureSlot<0>(gpu->list.Get(), sr, &present_parameters, nullptr));
      const auto after = nr::CollectFunnelInputs();
      const auto mirror_delta = dlss5::nr_decline_counts[static_cast<size_t>(dlss5::NrDeclineReason::kNestedMirror)].load() - mirrors;
      const auto present_delta = dlss5::nr_decline_counts[static_cast<size_t>(dlss5::NrDeclineReason::kPresentHook)].load() - presents;
      check(returned == NVSDK_NGX_Result_Success && game_evaluates == calls + 1
                && nr::ngx_entry_evaluate[0].load() == cpp0 + 1
                && nr::ngx_entry_evaluate[1].load() == cpp1 + (entry == Entry::CPP_TO_CPP ? 1 : 0)
                && nr::ngx_entry_evaluate_c[0].load() == c0 + (c_export ? 1 : 0)
                && nr::ngx_entry_evaluate_c[1].load() == c1 + (c_delegate ? 1 : 0),
            "nested Present invokes the real wrapper chain and only one original SR call");
      check(nr::present_capture_serial == captures + 1 && present_delta == 1,
            "nested Present captures the actual depth and motion only once");
      check(after.seen == before.seen + (c_export ? 1 : 2) && mirror_delta == (c_export ? 0 : 1)
                && after.eligible == before.eligible && after.injected == before.injected
                && after.unaccounted == before.unaccounted,
            "nested Present gives every counted wrapper one terminal without inventing a C delegate mirror");
      std::cout << "FUNNEL_PRESENT_NESTED entry=" << (c_export ? "C>Cpp" : (c_delegate ? "Cpp0>C1" : "Cpp0>Cpp1"))
                << " seen=" << after.seen - before.seen << " source_calls=" << game_evaluates - calls
                << " captures=" << nr::present_capture_serial - captures << " present_terminal=" << present_delta
                << " mirrors=" << mirror_delta << " unaccounted=" << after.unaccounted - before.unaccounted << '\n';
    }
    gpu->Submit();
    const dlss5::RuntimeLock lock(nr::runtime_mutex);
    for (auto& set : nr::present_guide_sets) {
      if (set.source_handle != sr) continue;
      nr::ClearPresentSource(&set);
      nr::RetirePresentResource(&set.motion);
      nr::RetirePresentResource(&set.depth);
      set = {};
    }
    nr::DrainPresentRetired();
  }
  {
    namespace health = dlss5::health;
    namespace xefg = dlss5::fg::xefg;
    Restore health_restore;
    health_restore.Atomic(&nr::hooks_enabled);
    health_restore.Atomic(&nr::d3d12_present_seen);
    health_restore.Atomic(&nr::nr_feature_ever_ready);
    health_restore.Atomic(&nr::gate_ever_opened);
    health_restore.Atomic(&nr::native_fg_result);
    health_restore.Atomic(&nr::edit_reuse_reuses);
    health_restore.Atomic(&dlss5::mixed_stage_group_successes);
    health_restore.Atomic(&dlss5::mixed_stage_source_successes);
    health_restore.Atomic(&xefg::internal::requested);
    health_restore.Atomic(&xefg::internal::owns_stream);
    health_restore.Atomic(&xefg::internal::result);
    nr::hooks_enabled.store(true);
    nr::enabled.store(true);
    nr::hook_point.store(nr::kHookPresent);
    nr::d3d12_present_seen.store(true);
    nr::nr_feature_ever_ready.store(true);
    nr::gate_ever_opened.store(true);
    nr::intercepted_evaluations.store(0);
    nr::own_evaluations.store(0);
    nr::evaluations_in_flight.store(0);
    nr::successful_evaluations.store(0);
    nr::successful_pre_sr_evaluations.store(0);
    nr::present_frames_seen.store(0);
    nr::present_prefg_attempts.store(0);
    nr::edit_reuse_reuses.store(0);
    dlss5::mixed_stage_group_successes.store(0);
    dlss5::mixed_stage_source_successes.store(0);
    for (auto& count : dlss5::nr_decline_counts) count.store(0);
    xefg::internal::requested.store(+[]() noexcept { return true; });

    const auto row = std::find_if(std::begin(health::kRows), std::end(health::kRows), [](const auto& candidate) {
      return std::strcmp(candidate.slug, "frames_unaccounted") == 0;
    });
    Check(row != std::end(health::kRows), "the production ledger registers frames_unaccounted");
    const auto row_index = static_cast<size_t>(row - std::begin(health::kRows));
    check(row->limit == 0.01 && row->min_volume == 60 && row->enter_s == 5 && row->exit_s == 15,
          "unaccounted health retains its 1 percent, 5 second enter and 15 second exit policy");
    health::Ledger ledger;
    std::array<health::Event, health::kRowCount> events{};
    double now_s = 0;
    uint64_t ticks = 0;
    uint64_t expected_eligible = 0;
    uint64_t expected_nr = 0;
    uint64_t expected_gaps = 0;
    uint32_t entered = 0;
    uint32_t exited = 0;
    bool cumulative_samples = true;
    bool current_handoffs = true;
    ledger.Tick(nr::CollectHealthSample(), now_s, false, true, events.data(), events.size());
    const auto window = [&](uint32_t gaps_per_hundred) {
      // Synthetic session counters exercise the collector and ledger. The
      // integration fixture separately proves the private Record producers.
      for (uint32_t frame = 0; frame < 8; ++frame) {
        constexpr dlss5::fg::Result phases[]{dlss5::fg::Result::PREPARED, dlss5::fg::Result::SUBMITTED,
            dlss5::fg::Result::FAILED, dlss5::fg::Result::NOT_SERVED, dlss5::fg::Result::ORDINARY_SUBMITTED};
        const auto route = ticks % 3;
        const auto phase = phases[(ticks / 3) % std::size(phases)];
        xefg::internal::owns_stream.store(route == 1);
        nr::native_fg_owns.store(route == 2);
        xefg::internal::result.store(phase);
        nr::native_fg_result.store(phase);
        nr::intercepted_evaluations.fetch_add(100);
        nr::successful_evaluations.fetch_add(99 - gaps_per_hundred);
        dlss5::CountNrDecline(dlss5::NrDeclineReason::kNgxMissingGuides);
        expected_eligible += 100;
        expected_nr += 99 - gaps_per_hundred;
        expected_gaps += gaps_per_hundred;
        const auto session = nr::CollectSessionFunnelInputs();
        const auto presentation = nr::CollectFunnelInputs();
        auto sample = nr::CollectHealthSample();
        const bool cumulative_matches = session.seen == expected_eligible && session.eligible == expected_eligible
            && session.injected == expected_nr && session.unaccounted == expected_gaps
            && sample[health::Signal::kEligible] == static_cast<double>(expected_eligible)
            && sample[health::Signal::kNrFrames] == static_cast<double>(expected_nr)
            && sample[health::Signal::kUnaccounted] == static_cast<double>(expected_gaps)
            && sample[dlss5::NrDeclineReason::kNgxMissingGuides] == static_cast<double>(ticks + 1);
        if (!cumulative_matches && cumulative_samples) {
          std::cout << "FUNNEL_HEALTH_SAMPLE_FAILURE tick=" << ticks << " route=" << route
                    << " phase=" << static_cast<unsigned>(phase)
                    << " expected_eligible=" << expected_eligible << " expected_nr=" << expected_nr
                    << " expected_gaps=" << expected_gaps << " expected_named=" << ticks + 1
                    << " session_seen=" << session.seen << " session_eligible=" << session.eligible
                    << " session_nr=" << session.injected << " session_gaps=" << session.unaccounted
                    << " sample_eligible=" << sample[health::Signal::kEligible]
                    << " sample_nr=" << sample[health::Signal::kNrFrames]
                    << " sample_gaps=" << sample[health::Signal::kUnaccounted]
                    << " sample_named=" << sample[dlss5::NrDeclineReason::kNgxMissingGuides] << '\n';
        }
        cumulative_samples &= cumulative_matches;
        const auto verdict = dlss5::ComputeNrVerdict(presentation);
        const auto card = dlss5::ui::ComputeStatusCard({.verdict = verdict, .active_features = 1, .present = true});
        auto expected_card = card.id;
        bool current_matches = false;
        if (route == 0) {
          current_matches = presentation.seen == session.seen && presentation.eligible == session.eligible
              && presentation.injected == session.injected && presentation.unaccounted == session.unaccounted;
        } else {
          const bool submitted = dlss5::fg::Submitted(phase);
          expected_card = (submitted ? dlss5::ui::CardId::kActive
              : phase == dlss5::fg::Result::PREPARED ? dlss5::ui::CardId::kFgPreparing : dlss5::ui::CardId::kNotRunning);
          current_matches = presentation.seen == 1 && presentation.eligible == 1 && presentation.unaccounted == 0
              && presentation.injected == static_cast<uint64_t>(submitted) && card.id == expected_card;
        }
        if (!current_matches && current_handoffs) {
          std::cout << "FUNNEL_HEALTH_CURRENT_FAILURE tick=" << ticks << " route=" << route
                    << " phase=" << static_cast<unsigned>(phase) << " card=" << static_cast<unsigned>(card.id)
                    << " expected_card=" << static_cast<unsigned>(expected_card)
                    << " verdict_state=" << static_cast<unsigned>(verdict.state) << " verdict_stage=" << verdict.StageName()
                    << " verdict_reason=" << verdict.ReasonName() << " verdict_path=" << verdict.path
                    << " current_seen=" << presentation.seen << " current_eligible=" << presentation.eligible
                    << " current_nr=" << presentation.injected << " current_gaps=" << presentation.unaccounted
                    << " session_seen=" << session.seen << " session_eligible=" << session.eligible
                    << " session_nr=" << session.injected << " session_gaps=" << session.unaccounted
                    << " gate_open=" << presentation.gate_ever_opened << '\n';
        }
        current_handoffs &= current_matches;
        ++ticks;
        now_s += 0.125;
        const auto count = ledger.Tick(sample, now_s, true, true, events.data(), events.size());
        for (size_t index = 0; index < count; ++index) {
          if (events[index].row != row_index) continue;
          entered += (events[index].transition == health::Transition::kEnter);
          exited += (events[index].transition == health::Transition::kExit);
        }
      }
    };
    for (uint32_t second = 0; second < 6; ++second) window(0);
    check(!ledger.State(row_index).active && entered == 0,
          "healthy ordinary, XeFG and native FG phase changes do not enter an accounting warning");
    for (uint32_t second = 0; second < 6; ++second) window(1);
    check(!ledger.State(row_index).active && entered == 0 && ledger.State(row_index).value == 0.01,
          "exactly 1 percent genuinely unnamed frames remain at the production boundary");
    for (uint32_t second = 0; second < 4; ++second) window(2);
    check(!ledger.State(row_index).active && entered == 0 && ledger.State(row_index).breach_s == 4,
          "a real unaccounted breach waits through four seconds");
    window(2);
    check(ledger.State(row_index).active && entered == 1 && exited == 0
              && ledger.State(row_index).breach_s == 5 && ledger.State(row_index).num == 16
              && ledger.State(row_index).den == 800 && ledger.State(row_index).value == 0.02,
          "a 2 percent unaccounted share enters once at five seconds despite current FG success or preparation");
    for (uint32_t second = 0; second < 14; ++second) window(0);
    check(ledger.State(row_index).active && entered == 1 && exited == 0 && ledger.State(row_index).clean_s == 14,
          "healthy handoffs retain the existing warning until fifteen clean seconds");
    window(0);
    check(!ledger.State(row_index).active && entered == 1 && exited == 1,
          "fifteen seconds of actual clean cumulative deltas clear the accounting warning once");
    for (uint32_t second = 0; second < 12; ++second) window(0);
    check(!ledger.State(row_index).active && entered == 1 && exited == 1 && expected_gaps != 0
              && ledger.State(row_index).num == 0 && ledger.State(row_index).den == 800,
          "historical unaccounted frames do not reenter after repeated healthy SDK On and Off transitions");
    check(cumulative_samples, "health collects monotonic session totals across ordinary and FG handoffs");
    check(current_handoffs, "FG cards keep current preparation, failure and delivery truth independently of health totals");
    std::cout << "FUNNEL_HEALTH_SESSION evidence=CPU samples=" << ticks << " enters=" << entered << " exits=" << exited
              << " historical_gaps=" << expected_gaps << " current_gap_share=" << ledger.State(row_index).value << '\n';
  }
  Check(failures == 0, "funnel wrapper accounting contracts");
}
}  // namespace funnel_accounting_tests
