import json
from pathlib import Path
import sys
import tempfile
import unittest
import zlib

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/offline"))
import sr_jitter


class JitterCalibrationTests(unittest.TestCase):
    def test_correct_and_inverted_contracts_are_distinguished(self):
        for correct_sign in (-1, 1):
            with self.subTest(correct_sign=correct_sign), tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[1] / "tmp") as output:
                root = Path(output)
                directory = root / "jitter-calibration"
                directory.mkdir()
                phases = [[.375, .25], [-.375, .25], [.375, -.25], [-.375, -.25]]
                report = {"schema": "nr-nexus-sr-jitter-calibration-v1", "dimensions": {"render": [32, 24], "output": [48, 36]},
                          "edge_render": [13.25, 9.75], "samples": []}
                for index, (sign, jitter) in enumerate([(0, [0, 0])] + [(s, j) for s in (-1, 1) for j in phases]):
                    # Synthetic SR oracle shifts wrong-sign edges by twice the
                    # requested jitter. Neither analyzer nor fixture assumes
                    # which sign is correct before measuring its output.
                    residual = np.array(jitter) * (0 if sign in (0, correct_sign) else 2)
                    y, x = np.mgrid[:36, :48]
                    pixels = np.ones((36, 48, 4), dtype="<f2")
                    pixels[..., 0] = .2 + .6 * np.clip((x + .5) / 1.5 - 13.25 - residual[0] + .5, 0, 1)
                    pixels[..., 1] = .2 + .6 * np.clip((y + .5) / 1.5 - 9.75 - residual[1] + .5, 0, 1)
                    path = directory / f"{index}.rgba16f"
                    raw = pixels.tobytes()
                    path.write_bytes(raw)
                    descriptor = {"file": path.name, "format": "RGBA16F", "width": 48, "height": 36,
                                  "bytes": len(raw), "crc32": zlib.crc32(raw)}
                    report["samples"].append({"index": index, "raster_sign": sign, "jitter": jitter,
                        "sr": {"calls": 1, "result": 1, "reset": 1}, "fence": index + 1, "completed_fence": index + 1,
                        "planes": {"sr": descriptor}})
                (directory / "sr-jitter-calibration.json").write_text(json.dumps(report))
                result = sr_jitter.analyze(root)
                self.assertEqual(result["status"], "aligned")
                self.assertEqual(result["raster_sampling_sign"], correct_sign)
                report["samples"][2]["sr"]["reset"] = 0
                (directory / "sr-jitter-calibration.json").write_text(json.dumps(report))
                with self.assertRaisesRegex(sr_jitter.ContractError, "Incomplete SR"):
                    sr_jitter.analyze(root)


if __name__ == "__main__":
    unittest.main()
