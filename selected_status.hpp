/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// A completed recording gives execution facts. Only the consumer can attest
// that it accepted that colour for the same output and configuration.
enum class MonitorConsumer : uint8_t { NONE, DXGI, NATIVE_FG, XE_FG };

struct MonitorDelivery {
  output_framerate::OutputIdentity output;
  uint64_t configuration = 0;
  uint64_t reset_epoch = 0;
  present_frame_policy::Frame frame;
  int64_t accepted_ns = 0;
  MonitorConsumer consumer = MonitorConsumer::NONE;
  uint64_t accepted_outputs = 0;
};

// Both FG consumers publish only after their existing successful-return checks.
inline void AcceptMonitorDelivery(MonitorDelivery* current, MonitorDelivery receipt, int64_t now) {
  const bool same_scope = current->output == receipt.output && current->configuration == receipt.configuration
      && current->reset_epoch == receipt.reset_epoch && current->consumer == receipt.consumer
      && present_frame_policy::SameSource(current->frame, receipt.frame);
  receipt.accepted_ns = now;
  receipt.accepted_outputs = (same_scope ? current->accepted_outputs + 1 : 1);
  *current = receipt;
}

inline bool CurrentMonitorDelivery(const MonitorDelivery& receipt,
    const output_framerate::OutputIdentity& output, uint64_t configuration, uint64_t reset_epoch,
    const present_frame_policy::Frame& frame, int64_t now) {
  return output && receipt.output == output && receipt.configuration == configuration
      && receipt.reset_epoch == reset_epoch && receipt.consumer != MonitorConsumer::NONE
      && receipt.accepted_ns > 0 && now >= receipt.accepted_ns
      && now - receipt.accepted_ns <= gpu_timers::internal::kSampleAgeNs
      && present_frame_policy::Known(frame) && present_frame_policy::Known(receipt.frame)
      && present_frame_policy::SameSource(receipt.frame, frame) && receipt.frame.sequence == frame.sequence;
}

inline ui::CardInputs ProjectSelectedCardInputs(ui::CardInputs in, bool has_present,
    const gpu_timers::Stats* execution, const MonitorDelivery& receipt = {},
    const output_framerate::Snapshot& rate = {},
    const std::optional<fg::xefg::StatusSnapshot>& xefg = {}) {
  in.active_features = 0;
  in.fg_submitted_frames.reset();
  in.fg_continuing_submission = false;
  in.present_before_fg = false;
  in.frame_gen_multiplier = 0;
  in.present_gaps = 0;
  in.gap_reason = nullptr;
  in.recent.reset();
  in.input_width = in.input_height = in.output_width = in.output_height = 0;
  in.nr_gpu_ms = in.nr_present_ms = 0.f;
  if (in.verdict.state == NrState::kOff || in.verdict.state == NrState::kUnavailable) return in;
  const auto observed = in.verdict;
  in.verdict = {.api = observed.api, .path = (has_present ? "present" : observed.path)};
  in.starting = true;
  in.dlss_hint = 0;
  in.issues = 0;
  in.present = has_present;
  if (execution != nullptr) {
    in.input_width = execution->passes[0].width;
    in.input_height = execution->passes[0].height;
    in.output_width = execution->width;
    in.output_height = execution->height;
  }
  if (has_present && xefg) {
    const auto outcome = xefg->DisplayOutcome();
    const bool selected_delivery = xefg->owned && receipt.consumer == MonitorConsumer::XE_FG
        && receipt.output == xefg->output && receipt.frame.generation == xefg->epoch;
    const bool rejected = outcome == fg::Result::NOT_SERVED || outcome == fg::Result::FAILED;
    const bool preparing = outcome == fg::Result::PREPARED;
    if (rejected || (preparing && (!selected_delivery || !xefg->continuing_submission))) {
      in.verdict = observed;
      return in;
    }
    if (!selected_delivery) return in;
  }
  if ((has_present && receipt.consumer != MonitorConsumer::NONE) || (!has_present && execution != nullptr)) {
    in.verdict.state = NrState::kEngaged;
    in.active_features = 1;
  }
  if (has_present && (receipt.consumer == MonitorConsumer::NATIVE_FG || receipt.consumer == MonitorConsumer::XE_FG)) {
    in.present_before_fg = true;
    in.fg_continuing_submission = true;
    if (receipt.accepted_outputs != 0) in.fg_submitted_frames = receipt.accepted_outputs;
    if (rate.output == receipt.output && rate.available && std::isfinite(rate.observed_multiplier)
        && rate.observed_multiplier > 1. && rate.observed_multiplier < UINT32_MAX) {
      in.frame_gen_multiplier = static_cast<uint32_t>(std::lround(rate.observed_multiplier));
    }
  }
  return in;
}
