// Copyright (C) 2026
// SPDX-License-Identifier: MIT

// DEV-only diagnostic after-cache marker in the final scRGB or PQ surface.
// Dispatch covers only the ROI; no anchor or image outside it is accessed.
RWTexture2D<float4> Target : register(u0);

cbuffer MarkerConstants : register(b0) {
  uint2 Origin;
  uint Reused;
  uint PatternBits;
  uint HdrMode;
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
  const float gray = (bit != 0u ? 4.0 : 0.125);
  // ST 2084 values for the same 320-nit / 10-nit diagnostic levels.
  const float pq_gray = (bit != 0u ? 0.6287278400 : 0.2996990924);
  value.rgb = (HdrMode == 2u ? pq_gray : gray);
  Target[pixel] = value;
}
