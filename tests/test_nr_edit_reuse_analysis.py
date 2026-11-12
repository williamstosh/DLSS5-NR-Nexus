"""Small synthetic checks for NR history comparison math and readback identity."""

import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "offline"))
import nr_edit_reuse as history


class HistoryAnalysisTests(unittest.TestCase):
    def test_known_srgb_and_stop_error(self):
        linear = history.srgb_decode(np.array([0, .04045, .5, 1.0]))
        np.testing.assert_allclose(linear, [0, .003130805, .21404114, 1.0], atol=1e-7)
        error = np.log2(np.array([.25, .5])) - np.log2(np.array([.125, .25]))
        self.assertEqual(history.statistics(error)["mae"], 1.0)

    def test_current_to_previous_motion_with_scale(self):
        previous = np.arange(12, dtype=np.float32).reshape(2, 6, 1)
        motion = np.zeros((2, 3, 2), dtype=np.float32)
        motion[..., 0] = -.5
        valid = np.full((2, 3, 1), 3, dtype=np.uint8)
        warped, mask = history.reproject(previous, motion, valid, [-1, 1])
        np.testing.assert_array_equal(warped[:, :-1], previous[:, 1:])
        self.assertFalse(mask[:, -1].any())
        self.assertTrue(mask[:, :-1].all())

    def test_two_step_validity_excludes_disocclusion(self):
        image = np.ones((2, 2, 3), dtype=np.float32)
        motion = np.zeros((2, 2, 2), dtype=np.float32)
        valid = np.array([[[3], [1]], [[0], [3]]], dtype=np.uint8)
        warped, mask = history.reproject(image, motion, valid, [1, 1])
        np.testing.assert_array_equal(warped, image)
        np.testing.assert_array_equal(mask, [[True, False], [False, True]])

    def test_half_readback_identity_and_finite_values(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1] / "tmp") as directory:
            root = Path(directory)
            raw = struct.pack("<eeee", .25, .5, 1, 1)
            (root / "frame.rgba16f").write_bytes(raw)
            descriptor = {"file": "frame.rgba16f", "format": "RGBA16F", "width": 1, "height": 1,
                          "bytes": len(raw), "crc32": zlib.crc32(raw)}
            values, _ = history.read_plane(root, descriptor)
            np.testing.assert_array_equal(values, [[[.25, .5, 1, 1]]])
            (root / "frame.rgba16f").write_bytes(struct.pack("<eeee", .5, .5, 1, 1))
            with self.assertRaisesRegex(history.ContractError, "identity"):
                history.read_plane(root, descriptor)
            raw = struct.pack("<eeee", float("nan"), .5, 1, 1)
            (root / "frame.rgba16f").write_bytes(raw)
            descriptor["crc32"] = zlib.crc32(raw)
            with self.assertRaisesRegex(history.ContractError, "Non-finite"):
                history.read_plane(root, descriptor)

    def test_empty_valid_region_is_unmeasured(self):
        result = history.statistics(np.empty((0, 3)))
        self.assertIsNone(result["mae"])
        self.assertEqual(result["components"], 0)
        json.dumps(result, allow_nan=False)

    def test_four_condition_pairing_and_sr_mismatch(self):
        with tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1] / "tmp") as directory:
            root = Path(directory)
            for condition_index, condition in enumerate(history.CONDITIONS):
                target = root / condition
                target.mkdir()
                report = {"schema": "nr-nexus-nr-edit-reuse-history-v1", "condition": condition,
                          "process_id": condition_index + 1, "runtime": "synthetic", "nr_sha256": "synthetic",
                          "driver_x100": 0, "requested_model": "B", "internal_model": "unknown",
                          "detail_stability": 0, "feed_mode": 1, "norm_governor": 0,
                          "prefix_host_sr_evaluates": 2, "prefix_nr_evaluates": 1,
                          "dimensions": {"render": [4, 4], "output": [4, 4]}, "frames": []}
                for index in range(4):
                    def save(name, values, format):
                        raw = values.tobytes()
                        filename = f"{index}-{name}.raw"
                        (target / filename).write_bytes(raw)
                        return {"file": filename, "format": format, "width": 4, "height": 4,
                                "bytes": len(raw), "crc32": zlib.crc32(raw)}

                    image = np.full((4, 4, 4), .5, dtype="<f2")
                    image[..., 3] = 1
                    inputs = {
                        "color": save("input", image, "RGBA16F"),
                        "depth": save("depth", np.ones((4, 4, 1), dtype="<f4"), "R32F"),
                        "motion": save("motion", np.zeros((4, 4, 2), dtype="<f2"), "RG16F"),
                        "nr_motion": save("nr-motion", np.zeros((4, 4, 2), dtype="<f2"), "RG16F"),
                        "surface": save("surface", np.zeros((4, 4, 1), dtype="u1"), "R8_UINT"),
                        "valid": save("valid", np.full((4, 4, 1), 3, dtype="u1"), "R8_UINT")}
                    refresh = not condition.startswith("x2") or index % 2 == 0
                    planes = {"sr": save("sr", image, "RGBA16F"), "proxy": None, "neural": None, "final": None}
                    if refresh:
                        planes["proxy"] = save("proxy", image, "RGBA16F")
                        if condition == "x2-compose" and index == 2:
                            image[..., :3] = .75
                        planes["neural"] = save("neural", image, "RGBA16F")
                        planes["final"] = save("final", image, "RGBA16F")
                    report["frames"].append({"frame": index, "scene": "synthetic", "refresh": refresh,
                        "fence": index + 1, "completed_fence": index + 1, "inputs": inputs, "planes": planes,
                        "sr": {"calls": 1, "reset": int(index == 0), "gpu_ms": 1},
                        "nr": {"calls": int(refresh), "reset": int(index == 0 or condition == "x2-reset") if refresh else None,
                               "gpu_ms": 1 if refresh else None,
                               "parameters": {"DLSSNR.MVecScale" + axis: {"value": 1} for axis in ("X", "Y")}}})
                (target / "nr-edit-reuse-history.json").write_text(json.dumps(report), encoding="utf-8")
            result = history.analyze(root)
            self.assertEqual(result["sr_and_proxy_identity"], "passed")
            self.assertEqual(result["conditions"]["x2-compose"]["nr_calls"], 2)
            self.assertEqual(result["comparisons"]["baseline-repeat"][2]["edit_error_stops"]["mae"], 0)
            self.assertGreater(result["comparisons"]["x2-compose"][1]["temporal_error_stops"]["mae"], 1)
            report_path = root / "x2-reset" / "nr-edit-reuse-history.json"
            report = json.loads(report_path.read_text(encoding="utf-8"))
            descriptor = report["frames"][0]["planes"]["sr"]
            raw = np.ones((4, 4, 4), dtype="<f2").tobytes()
            (root / "x2-reset" / descriptor["file"]).write_bytes(raw)
            descriptor["crc32"] = zlib.crc32(raw)
            report_path.write_text(json.dumps(report), encoding="utf-8")
            with self.assertRaisesRegex(history.ContractError, "sr mismatch"):
                history.analyze(root)
            self.assertEqual(len(list((root / "failure-sample").iterdir())), 2)


if __name__ == "__main__":
    unittest.main()
