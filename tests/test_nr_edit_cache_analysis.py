import json
from pathlib import Path
import sys
import tempfile
import unittest
import zlib

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/offline"))
import nr_edit_cache as cache


class CacheAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1] / "tmp")
        self.root = Path(self.directory.name)
        for name in cache.CONDITIONS:
            target = self.root / name
            target.mkdir()
            report = {"schema": "nr-nexus-nr-edit-reuse-cache-v1", "condition": name,
                      "dimensions": {"render": [4, 2], "output": [4, 2]}, "runtime": "synthetic",
                      "nr_sha256": "synthetic", "driver_x100": 0, "requested_model": "B",
                      "internal_model": "unknown", "detail_stability": 1,
                      "prefix_host_sr_evaluates": 1, "prefix_nr_evaluates": 1, "frames": []}
            for index in range(4):
                reuse = name == "cache" and index % 2 == 1
                image = np.full((2, 4, 4), .25 + index * .1, dtype="<f2")
                image[..., 3] = .75
                validity = np.ones((2, 4, 1), dtype="<u4")
                validity[:, :2] = 0
                final = image.copy()
                final[:, 2:, :3] *= 1.25
                planes = {
                    "sr": self.write_plane(target, index, "sr", image, "RGBA16F"),
                    "final": self.write_plane(target, index, "final", final, "RGBA16F"),
                    "validity": self.write_plane(target, index, "validity", validity, "R32_UINT") if reuse else None}
                report["frames"].append({"frame": index, "scene": "synthetic", "frame_kind": "reuse" if reuse else "refresh",
                    "fence": index + 1, "completed_fence": index + 1,
                    "inputs": {"color": self.write_plane(target, index, "color", image, "RGBA16F")},
                    "planes": planes,
                    "sr": {"calls": 1, "result": 1, "gpu_ms": .5},
                    "nr": {"calls": int(not reuse), "result": None if reuse else 1,
                           "gpu_ms": None if reuse else 2, "whole_chain_gpu_ms": .5 if reuse else 3}})
            (target / "nr-edit-reuse-cache.json").write_text(json.dumps(report), encoding="utf-8")

    def tearDown(self):
        self.directory.cleanup()

    def write_plane(self, target, index, name, values, format):
        path = target / f"frame-{index:03d}-{name}.raw"
        raw = values.tobytes()
        path.write_bytes(raw)
        return {"file": path.name, "format": format, "encoding": "synthetic", "bytes": len(raw),
                "width": values.shape[1], "height": values.shape[0], "crc32": zlib.crc32(raw)}

    def test_actual_cadence_cost_and_current_color_fallback(self):
        result = cache.analyze(self.root)
        self.assertEqual(result["input_and_sr_identity"], "passed")
        self.assertEqual(result["invalid_pixel_identity"], "passed")
        self.assertEqual(result["conditions"]["cache"]["nr_calls"], 2)
        self.assertEqual(result["conditions"]["cache"]["whole_chain_gpu_ms"]["mean"], 1.75)
        self.assertEqual(result["comparisons"][1]["invalid_pixels"], 4)
        self.assertEqual(result["comparisons"][1]["valid_pixels"], 4)
        self.assertEqual(len(list(self.root.glob("cache-frame-*.png"))), 4)

    def test_rejects_wrong_invalid_pixel_and_preserves_sample(self):
        report_path = self.root / "cache/nr-edit-reuse-cache.json"
        report = json.loads(report_path.read_text())
        descriptor = report["frames"][1]["planes"]["final"]
        path = self.root / "cache" / descriptor["file"]
        image = np.frombuffer(path.read_bytes(), dtype="<f2").reshape(2, 4, 4).copy()
        image[0, 0, 0] += .25
        raw = image.tobytes()
        path.write_bytes(raw)
        descriptor["crc32"] = zlib.crc32(raw)
        report_path.write_text(json.dumps(report))
        with self.assertRaisesRegex(cache.ContractError, "Invalid cache pixels"):
            cache.analyze(self.root)
        self.assertEqual(len(list((self.root / "cache-failure-sample").iterdir())), 2)

    def test_rejects_invented_reuse_model_measurement(self):
        report_path = self.root / "cache/nr-edit-reuse-cache.json"
        report = json.loads(report_path.read_text())
        report["frames"][1]["nr"]["gpu_ms"] = 0
        report_path.write_text(json.dumps(report))
        with self.assertRaisesRegex(cache.ContractError, "invented a model sample"):
            cache.analyze(self.root)


if __name__ == "__main__":
    unittest.main()
