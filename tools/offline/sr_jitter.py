"""Measure which analytic raster sampling sign the real SR jitter contract aligns.

Nine reset samples share one static two-axis edge scene: zero jitter, then four
phases for each raster sign. This resolves the fixture's coordinate convention;
it does not establish every game's guide or projection convention.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import numpy as np

from contracts import ContractError, digest, read_json
from nr_edit_reuse import read_plane, save_preview, statistics


def edge_position(image: np.ndarray, axis: int, center: float, radius: int) -> float:
    # R contains a vertical edge, G a horizontal edge. Signed gradients retain
    # the SR sharpening lobes instead of silently counting them as new edges.
    channel = image[..., axis]
    profile = (channel[channel.shape[0] // 4:channel.shape[0] * 3 // 4].mean(axis=0)
               if axis == 0 else channel[:, channel.shape[1] // 4:channel.shape[1] * 3 // 4].mean(axis=1))
    positions = np.arange(len(profile) - 1, dtype=np.float64) + .5
    band = np.abs(positions - center) <= radius
    derivative = np.diff(profile.astype(np.float64))[band]
    if derivative.sum() < .05:
        raise ContractError("Calibration edge has insufficient positive contrast")
    return float(np.dot(positions[band], derivative) / derivative.sum())


def analyze(root: Path) -> dict:
    directory = root / "jitter-calibration"
    report = read_json(directory / "sr-jitter-calibration.json")
    if report["schema"] != "nr-nexus-sr-jitter-calibration-v1" or len(report["samples"]) != 9:
        raise ContractError("Expected zero plus four phases for each raster sign")
    dimensions = report["dimensions"]
    scale = np.array(dimensions["output"], dtype=float) / dimensions["render"]
    center = np.array(report["edge_render"]) * scale - .5
    rows, raw_hashes = [], {}
    previous_fence = 0
    for index, sample in enumerate(report["samples"]):
        if (sample["index"] != index or sample["sr"]["calls"] != 1 or sample["sr"]["result"] != 1
                or sample["sr"]["reset"] != 1 or sample["fence"] <= previous_fence
                or sample["completed_fence"] < sample["fence"] or sample["completed_fence"] == 2 ** 64 - 1):
            raise ContractError(f"Incomplete SR calibration sample {index}")
        previous_fence = sample["fence"]
        plane = sample["planes"]["sr"]
        pixels, sha = read_plane(directory, plane)
        raw_hashes[plane["file"]] = sha
        save_preview(root / f"sr-jitter-{index:02d}.png", index,
                     f"raster {sample['raster_sign']}, jitter {sample['jitter']}", [("SR", pixels)])
        position = [edge_position(pixels, axis, center[axis], int(np.ceil(8 * scale[axis]))) for axis in (0, 1)]
        rows.append({"index": index, "raster_sign": sample["raster_sign"], "jitter": sample["jitter"],
                     "edge_output_pixels": position})
    if rows[0]["raster_sign"] != 0 or rows[0]["jitter"] != [0, 0]:
        raise ContractError("First calibration sample must be zero jitter")
    reference = np.array(rows[0]["edge_output_pixels"])
    groups = {}
    for sign in (-1, 1):
        selected = [row for row in rows if row["raster_sign"] == sign]
        if len(selected) != 4:
            raise ContractError("Each raster sign needs the same four jitter phases")
        shifts = np.array([(np.array(row["edge_output_pixels"]) - reference) / scale for row in selected])
        phases = np.array([row["jitter"] for row in selected])
        groups[str(sign)] = {"render_pixel_error": statistics(shifts),
                            "shift_per_jitter": (np.sum(shifts * phases, axis=0) / np.sum(phases * phases, axis=0)).tolist()}
        for row, shift in zip(selected, shifts):
            row["shift_render_pixels"] = shift.tolist()
    if [row["jitter"] for row in rows if row["raster_sign"] == -1] != [row["jitter"] for row in rows if row["raster_sign"] == 1]:
        raise ContractError("Raster signs used different jitter phases")
    ordered = sorted(groups, key=lambda key: groups[key]["render_pixel_error"]["rmse"])
    best, worst = [groups[key]["render_pixel_error"]["rmse"] for key in ordered]
    resolved = best < .15 and worst > max(.25, 3 * best)
    return {"schema": "nr-nexus-sr-jitter-analysis/1", "status": "aligned" if resolved else "inconclusive",
            "raster_sampling_sign": int(ordered[0]) if resolved else None,
            "sampling_equation": "world sample = pixel center + sign * API jitter",
            "measurement": "Signed-gradient edge centroid relative to zero-jitter SR, in render pixels",
            "decision_bounds": "best RMS < 0.15 render pixels; opposite sign > max(0.25, 3*best)",
            "scope": "This deterministic SR fixture and recorded runtime; no game or NR-history acceptance",
            "report_sha256": digest(directory / "sr-jitter-calibration.json"),
            "dimensions": dimensions, "groups": groups, "samples": rows, "raw_sha256": raw_hashes}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = analyze(args.input)
        code = 0 if result["status"] == "aligned" else 2
    except (ContractError, OSError, KeyError, ValueError) as error:
        result = {"schema": "nr-nexus-sr-jitter-analysis/1", "status": "failed", "error": str(error)}
        code = 1
    (args.input / "sr-jitter-analysis.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in ("status", "raster_sampling_sign", "error") if key in result}))
    return code


if __name__ == "__main__":
    sys.exit(main())
