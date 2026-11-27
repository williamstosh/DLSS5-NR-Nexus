// Colour protection is identical for adapters with valid, same-domain samples.
// Resource binding, coordinate mapping and late depth proof stay with the caller.
float ControlMaskDepthKeep(float depth_value, bool inverted, float fade_start, float fade_end) {
  if (inverted) {
    depth_value = 1.0 - depth_value;
  }
  float keep = 1.0;
  if (depth_value >= 0.0 && depth_value <= 1.0) {
    keep = 1.0 - smoothstep(fade_start, fade_end, depth_value);
  }
  return keep;
}

float ControlMaskMotionKeep(float2 motion_vector, float2 pixel_scale, float fade_start, float fade_end) {
  const float speed = length(motion_vector * pixel_scale);
  float keep = 1.0;
  if (all(isfinite(motion_vector)) && isfinite(speed)) {
    keep = 1.0 - smoothstep(fade_start, fade_end, speed);
  }
  return keep;
}

float ControlMaskRed(float red, float depth_keep, float motion_keep) {
  return red * min(depth_keep, motion_keep);
}
