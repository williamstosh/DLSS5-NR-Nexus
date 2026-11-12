"""Compare the fixed x2 cache candidate with its independent per-frame baseline.

The caller runs this before Standard raw cleanup. Quality differences remain
measurements; exact current-color fallback and actual cadence are contracts.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import sys

import numpy as np
from PIL import Image

from contracts import ContractError, artifact_path, digest, read_json, sample_statistics
from nr_edit_reuse import read_plane, save_preview, statistics

CONDITIONS = ("cache-baseline", "cache")


def luma_edit(final: np.ndarray, source: np.ndarray) -> np.ndarray:
    weights = np.array([.2126, .7152, .0722])
    return np.log2(np.maximum(final[..., :3] @ weights, 1 / 1024)) - np.log2(np.maximum(source[..., :3] @ weights, 1 / 1024))


def preserve(root: Path, planes: list[tuple[str, dict]]) -> None:
    output = root / "cache-failure-sample"
    output.mkdir(exist_ok=True)
    for condition, descriptor in planes:
        shutil.copyfile(artifact_path(root / condition, descriptor["file"]),
                        output / (condition + "-" + Path(descriptor["file"]).name))


def analyze(root: Path) -> dict:
    reports = {name: read_json(root / name / "nr-edit-reuse-cache.json") for name in CONDITIONS}
    reference = reports["cache-baseline"]
    frames = reference["frames"]
    if not frames or [frame["frame"] for frame in frames] != list(range(len(frames))):
        raise ContractError("Cache baseline frame indices must be consecutive from zero")
    result = {"schema": "nr-nexus-nr-edit-cache-analysis/1", "status": "measured",
              "evidence": ["NATIVE_GPU", "NATIVE_NGX"], "visual_acceptance": "not_performed",
              "conditions": {}, "comparisons": [], "dimensions": reference["dimensions"],
              "route": reference.get("route", "inline"), "xefg_scope": reference.get("xefg_scope"),
              "measurement_notes": [
                  "Independent Features use identical SR input and byte-identical SR output.",
                  "Whole-chain intervals include cache storage/reprojection, codec and output copies, excluding test readbacks.",
                  "Model intervals include only real NR Evaluate; reuse has no model sample.",
                  "Invalid reuse pixels must equal current SR bit-for-bit, including alpha.",
                  "Refresh/reuse costs are separate; no FPS or dynamic visual acceptance is inferred."]}
    for name, report in reports.items():
        if report["schema"] != "nr-nexus-nr-edit-reuse-cache-v1" or len(report["frames"]) != len(frames):
            raise ContractError(f"Incompatible cache sequence: {name}")
        for key in ("dimensions", "runtime", "nr_sha256", "driver_x100", "requested_model", "internal_model",
                    "detail_stability", "prefix_host_sr_evaluates", "prefix_nr_evaluates"):
            if report[key] != reference[key]:
                raise ContractError(f"Mismatched {key}: {name}")
        if report["detail_stability"] != 1:
            raise ContractError(f"Cache experiment requires Detail stability Off (1): {name}")
        if any(report.get(key) != reference.get(key) for key in ("scene_id", "raster_sign")):
            raise ContractError(f"Mismatched jitter scene: {name}")
        if any(report.get(key) != reference.get(key) for key in ("route", "present_white_nits", "xefg_scope")):
            raise ContractError(f"Mismatched recording route: {name}")
        times, kinds, model_times = [], {}, []
        previous_fence, previous_reuse = 0, False
        for index, frame in enumerate(report["frames"]):
            if (frame["frame"] != index or frame["scene"] != frames[index]["scene"]
                    or frame["sr"]["calls"] != 1 or frame["sr"]["result"] != 1
                    or frame["fence"] <= previous_fence or frame["completed_fence"] < frame["fence"]
                    or frame["completed_fence"] == 2 ** 64 - 1):
                raise ContractError(f"Incomplete SR or GPU completion: {name} frame {index}")
            previous_fence = frame["fence"]
            if frame["sr"].get("jitter") != frames[index]["sr"].get("jitter"):
                raise ContractError(f"Mismatched SR jitter: {name} frame {index}")
            reuse = frame["frame_kind"] == "reuse"
            if frame["nr"]["calls"] != int(not reuse) or (reuse and previous_reuse):
                raise ContractError(f"Unexpected x2 cadence: {name} frame {index}")
            if reuse:
                if name == "cache-baseline" or frame["nr"]["gpu_ms"] is not None:
                    raise ContractError(f"Reuse invented a model sample: {name} frame {index}")
            else:
                if frame["nr"]["result"] != 1:
                    raise ContractError(f"NR Evaluate failed: {name} frame {index}")
                model_times.append(frame["nr"]["gpu_ms"])
            previous_reuse = reuse
            elapsed = frame["nr"]["whole_chain_gpu_ms"]
            if not np.isfinite(elapsed) or elapsed <= 0:
                raise ContractError(f"Invalid whole-chain timing: {name} frame {index}")
            times.append(elapsed)
            kinds.setdefault(frame["frame_kind"], []).append(elapsed)
        result["conditions"][name] = {
            "report_sha256": digest(root / name / "nr-edit-reuse-cache.json"),
            "sr_calls": len(frames), "nr_calls": sum(frame["nr"]["calls"] for frame in report["frames"]),
            "whole_chain_gpu_ms": sample_statistics(times),
            "whole_chain_gpu_ms_by_kind": {kind: sample_statistics(values) for kind, values in kinds.items()},
            "model_gpu_ms_when_invoked": sample_statistics(model_times), "raw_sha256": {}}

    previous_lighting = None
    lighting_previews = []
    for index, baseline_frame in enumerate(frames):
        loaded = {}
        for name, report in reports.items():
            frame = report["frames"][index]
            loaded[name] = {"inputs": {}, "planes": {}}
            for group in ("inputs", "planes"):
                for plane, descriptor in frame[group].items():
                    if descriptor is None:
                        continue
                    values, sha = read_plane(root / name, descriptor)
                    loaded[name][group][plane] = values
                    result["conditions"][name]["raw_sha256"][descriptor["file"]] = sha
            if name == "cache-baseline":
                continue
            for group, planes in (("inputs", baseline_frame["inputs"]), ("planes", {"sr": baseline_frame["planes"]["sr"]})):
                for plane, first in planes.items():
                    second = frame[group][plane]
                    if (result["conditions"]["cache-baseline"]["raw_sha256"][first["file"]]
                            != result["conditions"][name]["raw_sha256"][second["file"]]):
                        preserve(root, [("cache-baseline", first), (name, second)])
                        raise ContractError(f"{plane} mismatch: {name} frame {index}")
        baseline = loaded["cache-baseline"]["planes"]
        cached = loaded["cache"]["planes"]
        frame = reports["cache"]["frames"][index]
        final_bits = cached["final"].astype("<f2").view("<u2")
        source_bits = cached["sr"].astype("<f2").view("<u2")
        if not np.array_equal(final_bits[..., 3], source_bits[..., 3]):
            raise ContractError(f"Cache changed source alpha: frame {index}")
        row = {"frame": index, "scene": frame["scene"], "frame_kind": frame["frame_kind"],
               "final_rgb": statistics(cached["final"][..., :3] - baseline["final"][..., :3]),
               "alpha_identical": True, "invalid_pixels": None, "invalid_pixels_identical": None}
        if frame["frame_kind"] == "reuse":
            invalid = cached["validity"][..., 0] == 0
            if not np.array_equal(final_bits[invalid], source_bits[invalid]):
                preserve(root, [("cache", frame["planes"]["sr"]), ("cache", frame["planes"]["final"])])
                raise ContractError(f"Invalid cache pixels differ from current SR: frame {index}")
            row.update(invalid_pixels=int(invalid.sum()), invalid_pixels_identical=True,
                       valid_pixels=int((~invalid).sum()))
        # The six lighting frames stop the background camera. This fixed left
        # ROI stays away from the independently moving foreground and borders.
        # Compare changes in NR's edit to the per-frame baseline, separating
        # source illumination from the edit disappearing/reappearing.
        if frame["scene"] == "lighting":
            height, width = cached["sr"].shape[:2]
            roi = (slice(height // 4, height * 3 // 4), slice(width // 16, width // 4))
            base_edit = luma_edit(baseline["final"], baseline["sr"])[roi]
            cache_edit = luma_edit(cached["final"], cached["sr"])[roi]
            row["lighting_roi"] = {"xywh": [width // 16, height // 4, width // 4 - width // 16, height * 3 // 4 - height // 4],
                                   "baseline_edit_stops": statistics(base_edit), "cache_edit_stops": statistics(cache_edit),
                                   "edit_error_stops": statistics(cache_edit - base_edit), "temporal_error_stops": None}
            if previous_lighting is not None:
                row["lighting_roi"]["baseline_delta_stops"] = statistics(base_edit - previous_lighting[0])
                row["lighting_roi"]["cache_delta_stops"] = statistics(cache_edit - previous_lighting[1])
                row["lighting_roi"]["temporal_error_stops"] = statistics(
                    (cache_edit - previous_lighting[1]) - (base_edit - previous_lighting[0]))
            previous_lighting = (base_edit, cache_edit)
        result["comparisons"].append(row)
        save_preview(root / f"cache-frame-{index:03d}.png", index, frame["scene"],
                     [("SR", cached["sr"]), ("baseline", baseline["final"]),
                      (frame["frame_kind"], cached["final"])])
        if frame["scene"] == "lighting":
            with Image.open(root / f"cache-frame-{index:03d}.png") as preview:
                lighting_previews.append(preview.copy())
    if not any(row["frame_kind"] == "reuse" for row in result["comparisons"]):
        raise ContractError("Cache candidate never reused an edit")
    if not any(row.get("valid_pixels", 0) for row in result["comparisons"]):
        raise ContractError("Cache candidate rejected every cached edit")
    result["input_and_sr_identity"] = "passed"
    result["invalid_pixel_identity"] = "passed"
    result["quality_decision"] = "requires sequence review; no automatic quality pass"
    if lighting_previews:
        first = lighting_previews[0]
        first.save(root / "cache-lighting-preview.png", save_all=True, append_images=lighting_previews[1:], duration=250, loop=0)
        strip = Image.new("RGB", (first.width, first.height * len(lighting_previews)))
        for index, preview in enumerate(lighting_previews):
            strip.paste(preview, (0, index * first.height))
        strip.save(root / "cache-lighting-strip.png")
        result["lighting_observation"] = {"preview": "cache-lighting-preview.png", "strip": "cache-lighting-strip.png",
                                          "preview_frame_ms": 250,
                                          "notes": "Slow diagnostic loop, not game pacing. ROI uses final/SR log2 luma gain; temporal error subtracts the baseline edit change."}
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = analyze(args.input)
        code = 0
    except (ContractError, OSError, KeyError, ValueError) as error:
        result = {"schema": "nr-nexus-nr-edit-cache-analysis/1", "status": "failed", "error": str(error)}
        code = 1
    (args.input / "nr-edit-cache-analysis.json").write_text(
        json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("status", "error") if key in result}))
    return code


if __name__ == "__main__":
    sys.exit(main())
