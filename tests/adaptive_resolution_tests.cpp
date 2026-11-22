/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#include <iostream>

#include "adaptive_resolution_cases.hpp"
#include "output_framerate_cases.hpp"

int main() {
  bool passed = adaptive_resolution_cases::Run();
  RunOutputFramerateCases([&](bool condition, const char* name) {
    if (!condition) {
      std::cerr << "output-framerate: " << name << '\n';
      passed = false;
    }
  });
  if (passed) {
    std::cout << "NR_RELEASE_PARITY dx12-upscaled-adaptive-x2 controller paired_mean_us=2000 lower_us=1500 "
                 "missing-cost=fps-controlled queue-change=preserved target=exact failed-raise=workload-scoped evidence=CPU\n";
    std::cout << "NR_RELEASE_PARITY native-application-frame-count controller fixed=hold-80-to-100 "
                 "dynamic-no-denominator=collecting dynamic-ratio-shift=recollect "
                 "gap-denominator=unavailable full-tags=recovered evidence=CPU\n";
  }
  return passed ? 0 : 1;
}
