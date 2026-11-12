"""Compare the independent native NR history experiments before raw cleanup.

Requires NumPy and Pillow. Numerical differences are measurements, not a visual
pass/fail threshold. The test entry owns raw retention and cleanup.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw

from contracts import ContractError, artifact_path, digest, read_json, sample_statistics

CONDITIONS = ("baseline", "baseline-repeat", "x2-compose", "x2-reset")
FORMATS = {"RGBA16F": ("<f2", 4), "RG16F": ("<f2", 2),
           "R32F": ("<f4", 1), "R8_UINT": ("u1", 1)}
FLOOR = 1.0 / 1024.0


def read_plane(directory: Path, descriptor: dict) -> tuple[np.ndarray, str]:
    path = artifact_path(directory, descriptor["file"])
    raw = path.read_bytes()
    if len(raw) != descriptor["bytes"] or zlib.crc32(raw) != descriptor["crc32"]:
        raise ContractError(f"Readback identity mismatch: {path}")
    dtype, channels = FORMATS[descriptor["format"]]
    values = np.frombuffer(raw, dtype=dtype).reshape(descriptor["height"], descriptor["width"], channels)
    if not np.isfinite(values).all():
        raise ContractError(f"Non-finite readback: {path}")
    return values.astype(np.float32), hashlib.sha256(raw).hexdigest()


def srgb_decode(values: np.ndarray) -> np.ndarray:
    return np.where(values <= 0.04045, values / 12.92,
                    np.maximum((values + 0.055) / 1.055, 0) ** 2.4)


def statistics(values: np.ndarray) -> dict:
    values = np.asarray(values, dtype=np.float64)
    if values.size == 0:
        return {"components": 0, "mae": None, "rmse": None, "max": None, "p95": None, "signed_mean": None}
    absolute = np.abs(values)
    return {"components": int(values.size), "mae": float(absolute.mean()),
            "rmse": float(np.sqrt(np.mean(values * values))), "max": float(absolute.max()),
            "p95": float(np.quantile(absolute, .95)), "signed_mean": float(values.mean())}


def preserve_pair(root: Path, name: str, first: dict, second: dict) -> None:
    destination = root / "failure-sample"
    destination.mkdir(exist_ok=True)
    for condition, descriptor in (("baseline", first), (name, second)):
        shutil.copyfile(artifact_path(root / condition, descriptor["file"]),
                        destination / (condition + "-" + Path(descriptor["file"]).name))


def reproject(previous: np.ndarray, motion: np.ndarray, valid: np.ndarray,
              scale: list[float]) -> tuple[np.ndarray, np.ndarray]:
    """Two-step fixture motion in render pixels -> bilinear NR-grid samples."""
    height, width, _ = previous.shape
    render_height, render_width, _ = motion.shape
    y, x = np.mgrid[:height, :width]
    mx = np.minimum(((x + .5) * render_width / width).astype(int), render_width - 1)
    my = np.minimum(((y + .5) * render_height / height).astype(int), render_height - 1)
    px = x + motion[my, mx, 0] * scale[0] * width / render_width
    py = y + motion[my, mx, 1] * scale[1] * height / render_height
    mask = ((valid[my, mx, 0].astype(np.uint8) & 2) != 0)
    mask &= (px >= 0) & (px <= width - 1) & (py >= 0) & (py <= height - 1)
    px, py = np.clip(px, 0, width - 1), np.clip(py, 0, height - 1)
    x0, y0 = px.astype(int), py.astype(int)
    x1, y1 = np.minimum(x0 + 1, width - 1), np.minimum(y0 + 1, height - 1)
    ax, ay = (px - x0)[..., None], (py - y0)[..., None]
    warped = ((previous[y0, x0] * (1 - ax) + previous[y0, x1] * ax) * (1 - ay)
              + (previous[y1, x0] * (1 - ax) + previous[y1, x1] * ax) * ay)
    return warped, mask


def save_preview(path: Path, frame: int, scene: str, planes: list[tuple[str, np.ndarray]]) -> None:
    image = Image.new("RGB", (320 * len(planes), 212), "#171b20")
    draw = ImageDraw.Draw(image)
    for index, (name, values) in enumerate(planes):
        # One fixed display transform for all linear-HDR final planes.
        linear = np.maximum(values[..., :3], 0)
        linear = linear / (1 + linear)
        encoded = np.where(linear <= .0031308, linear * 12.92,
                           1.055 * linear ** (1 / 2.4) - .055)
        tile = Image.fromarray(np.uint8(np.clip(encoded * 255, 0, 255))).resize((320, 180))
        image.paste(tile, (320 * index, 32))
        draw.text((320 * index + 8, 8), f"{frame:02d} {scene}: {name}", fill="white")
    image.save(path)


def analyze(root: Path) -> dict:
    reports = {name: read_json(root / name / "nr-edit-reuse-history.json") for name in CONDITIONS}
    reference = reports["baseline"]
    frames = reference["frames"]
    if not frames or [item["frame"] for item in frames] != list(range(len(frames))):
        raise ContractError("Baseline frame indices must be consecutive from zero")
    result = {"schema": "nr-nexus-nr-edit-reuse-analysis/1", "status": "measured",
              "evidence": ["NATIVE_GPU", "NATIVE_NGX"], "visual_acceptance": "not_performed",
              "dimensions": reference["dimensions"], "conditions": {}, "comparisons": {},
              "measurement_notes": [
                  "SR and encoded proxy must match byte-for-byte across conditions.",
                  "Baseline repeat estimates run-to-run output differences; no visual quality threshold is assumed.",
                  "Edit differences use decoded proxy/neural log2 gain with floor 1/1024.",
                  "Temporal error compares successive even refresh errors after two-step fixture motion reprojection.",
                  "GPU timing covers real Evaluate intervals only; no cache replay or whole-chain savings measured.",
                  "Previews use fixed linear/(1+linear), then sRGB; raw numerical comparisons are untonemapped."]}
    for name, report in reports.items():
        if report["schema"] != "nr-nexus-nr-edit-reuse-history-v1" or report["condition"] != name:
            raise ContractError(f"Wrong condition or schema: {name}")
        if report["dimensions"] != reference["dimensions"] or len(report["frames"]) != len(frames):
            raise ContractError(f"Incompatible sequence: {name}")
        if report["detail_stability"] != 1:
            raise ContractError(f"History experiment requires Detail stability Off (1): {name}")
        for key in ("runtime", "nr_sha256", "driver_x100", "requested_model", "internal_model",
                    "detail_stability", "feed_mode", "norm_governor", "prefix_host_sr_evaluates", "prefix_nr_evaluates"):
            if report[key] != reference[key]:
                raise ContractError(f"Mismatched {key}: {name}")
        nr_times, sr_times = [], []
        previous_fence = 0
        for index, frame in enumerate(report["frames"]):
            refresh = not name.startswith("x2") or index % 2 == 0
            if (frame["frame"] != index or frame["scene"] != frames[index]["scene"]
                    or frame["refresh"] != refresh or frame["sr"]["calls"] != 1
                    or frame["nr"]["calls"] != int(refresh)
                    or frame["fence"] <= previous_fence or frame["completed_fence"] < frame["fence"]
                    or frame["completed_fence"] == 2 ** 64 - 1):
                raise ContractError(f"Incomplete cadence or GPU completion: {name} frame {index}")
            previous_fence = frame["fence"]
            expected_reset = int(index == 0 or name == "x2-reset")
            if frame["sr"]["reset"] != int(index == 0) or (refresh and frame["nr"]["reset"] != expected_reset):
                raise ContractError(f"Unexpected Reset schedule: {name} frame {index}")
            sr_times.append(frame["sr"]["gpu_ms"])
            nr_times.append(frame["nr"]["gpu_ms"] if refresh else 0.0)
        result["conditions"][name] = {"report_sha256": digest(root / name / "nr-edit-reuse-history.json"),
            "process_id": report["process_id"], "runtime": report["runtime"],
            "nr_calls": sum(item["nr"]["calls"] for item in report["frames"]),
            "sr_calls": len(frames), "sr_gpu_ms": sample_statistics(sr_times),
            "nr_gpu_ms_per_base_frame": sample_statistics(nr_times),
            "nr_gpu_ms_refresh_only": sample_statistics([value for value in nr_times if value > 0]),
            "raw_sha256": {}}

    previous_errors = {}
    for index, baseline_frame in enumerate(frames):
        loaded = {}
        for name, report in reports.items():
            frame = report["frames"][index]
            loaded[name] = {"inputs": {}, "planes": {}}
            for group in ("inputs", "planes"):
                for plane, descriptor in frame[group].items():
                    if descriptor is None:
                        continue
                    array, sha = read_plane(root / name, descriptor)
                    loaded[name][group][plane] = array
                    result["conditions"][name]["raw_sha256"][descriptor["file"]] = sha
            for plane in ("color", "depth", "motion", "surface", "valid"):
                if name != "baseline":
                    first, second = baseline_frame["inputs"][plane], frame["inputs"][plane]
                    if result["conditions"]["baseline"]["raw_sha256"][first["file"]] != result["conditions"][name]["raw_sha256"][second["file"]]:
                        preserve_pair(root, name, first, second)
                        raise ContractError(f"Input mismatch: {name} frame {index} {plane}")
            for plane in ("sr", "proxy"):
                if name != "baseline" and plane in loaded[name]["planes"]:
                    first = baseline_frame["planes"][plane]["file"]
                    second = frame["planes"][plane]["file"]
                    if result["conditions"]["baseline"]["raw_sha256"][first] != result["conditions"][name]["raw_sha256"][second]:
                        preserve_pair(root, name, baseline_frame["planes"][plane], frame["planes"][plane])
                        raise ContractError(f"{plane} mismatch: {name} frame {index}")
        baseline = loaded["baseline"]["planes"]
        for name in CONDITIONS[1:]:
            comparison = loaded[name]["planes"]
            if "neural" not in comparison:
                continue
            row = {"frame": index, "scene": baseline_frame["scene"], "sr_identical": True, "proxy_identical": True}
            for plane in ("neural", "final"):
                difference = comparison[plane][..., :3] - baseline[plane][..., :3]
                row[plane + "_rgb"] = statistics(difference)
                row[plane + "_alpha_equal"] = bool(np.array_equal(comparison[plane][..., 3], baseline[plane][..., 3]))
            base_neural = srgb_decode(baseline["neural"][..., :3])
            variant_neural = srgb_decode(comparison["neural"][..., :3])
            row["neural_decoded_linear_rgb"] = statistics(variant_neural - base_neural)
            # Proxy is identical, so it cancels in the difference of log edits.
            error = np.log2(np.maximum(variant_neural, FLOOR)) - np.log2(np.maximum(base_neural, FLOOR))
            row["edit_error_stops"] = statistics(error)
            if index % 2 == 0:
                if name in previous_errors:
                    motion = loaded["x2-compose"]["inputs"]["nr_motion"]
                    valid = loaded["x2-compose"]["inputs"]["valid"]
                    params = reports["x2-compose"]["frames"][index]["nr"]["parameters"]
                    scale = [params["DLSSNR.MVecScale" + axis]["value"] for axis in ("X", "Y")]
                    warped, mask = reproject(previous_errors[name], motion, valid, scale)
                    row["temporal_error_stops"] = statistics((error - warped)[mask])
                    row["temporal_valid_pixels"] = int(mask.sum())
                previous_errors[name] = error
            result["comparisons"].setdefault(name, []).append(row)
        if index % 2 == 0:
            save_preview(root / f"frame-{index:03d}-comparison.png", index, baseline_frame["scene"],
                         [("SR", baseline["sr"])]
                         + [(name, loaded[name]["planes"]["final"]) for name in ("baseline", "x2-compose", "x2-reset")])
    result["sr_and_proxy_identity"] = "passed"
    result["quality_decision"] = "requires numerical and sequence review; no automatic quality pass"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="Parent of the four condition directories")
    args = parser.parse_args()
    try:
        result = analyze(args.input)
        code = 0
    except (ContractError, OSError, KeyError, ValueError) as error:
        result = {"schema": "nr-nexus-nr-edit-reuse-analysis/1", "status": "failed", "error": str(error)}
        code = 1
    (args.input / "nr-edit-reuse-analysis.json").write_text(
        json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("status", "error") if key in result}))
    return code


if __name__ == "__main__":
    sys.exit(main())
