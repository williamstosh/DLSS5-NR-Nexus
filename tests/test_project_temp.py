import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from project_temp import project_temporary_directory


class ProjectTemporaryDirectoryTests(unittest.TestCase):
    def test_external_environment_uses_local_fixture_root(self):
        with patch.dict(os.environ, {"TMPDIR": str(Path(__file__).parent.parent.parent / "outside"),
                                     "TEMP": "relative", "TMP": "relative"}):
            with project_temporary_directory() as folder:
                self.assertEqual(Path(folder).parent.name, "python-fixtures")
                self.assertEqual(Path(folder).parents[1].name, "tmp")

    def test_existing_caller_temp_and_junction(self):
        with project_temporary_directory() as scratch:
            folder = Path(scratch)
            with patch.dict(os.environ, {"TMPDIR": str(folder)}):
                with project_temporary_directory() as actual:
                    self.assertEqual(Path(actual).parent, folder)
            if os.name == "nt":
                link = folder / "junction"
                target = folder / "target"
                target.mkdir()
                subprocess.run([os.environ["ComSpec"], "/d", "/c", "mklink", "/J", str(link), str(target)],
                               check=True, capture_output=True)
                try:
                    for preferred in (link, link / "uncreated"):
                        with patch.dict(os.environ, {"TMPDIR": str(preferred)}):
                            with self.assertRaisesRegex(ValueError, "crosses a link"):
                                project_temporary_directory()
                    target.rmdir()
                    with patch.dict(os.environ, {"TMPDIR": str(link)}):
                        with self.assertRaisesRegex(ValueError, "crosses a link"):
                            project_temporary_directory()
                finally:
                    link.rmdir()


if __name__ == "__main__":
    unittest.main()
