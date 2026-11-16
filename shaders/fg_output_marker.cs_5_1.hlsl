// Copyright (C) 2026
// SPDX-License-Identifier: MIT

// DEV-only diagnostic after-cache marker in the final linear FP16 surface.
// Dispatch covers only the ROI; no anchor or image outside it is accessed.
RWTexture2D<float4> Target : register(u0);

cbuffer MarkerConstants : register(b0) {
  uint2 Origin;
  uint Reused;
  uint PatternBits;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  const uint2 local = dispatch_id.xy;
  if (local.x >= 128u || local.y >= 64u) return;
  const uint cell = (local.y / 16u) * 8u + local.x / 16u;
  uint bit = (PatternBits >> cell) & 1u;
  // Last two cells are complementary, preserving a phase in both directions.
  if (cell == 30u) bit = Reused;
  if (cell == 31u) bit = 1u - Reused;
  const uint2 pixel = Origin + local;
  float4 value = Target[pixel];
  value.rgb = bit != 0u ? 4.0 : 0.125;
  Target[pixel] = value;
}
