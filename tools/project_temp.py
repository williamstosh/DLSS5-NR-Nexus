"""Isolated test fixtures inside this checkout, independent of system tempfile defaults."""
import os
import stat
from pathlib import Path
from tempfile import TemporaryDirectory


def _reject_links(path):
    for item in (*reversed(path.parents), path):
        try:
            info = item.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError(f"Python fixture root crosses a link: {item}")


def project_temporary_directory():
    root = Path(__file__).absolute().parents[1] / "tmp"
    _reject_links(root)
    # Outside or absent caller defaults are ignored; an in-project link is rejected.
    base = root / "python-fixtures"
    for key in ("TMPDIR", "TEMP", "TMP"):
        value = os.environ.get(key)
        if not value:
            continue
        candidate = Path(os.path.abspath(value))
        if Path(value).is_absolute() and candidate.is_relative_to(root):
            _reject_links(candidate)
            if candidate.is_dir():
                base = candidate
                break
    _reject_links(base)
    base.mkdir(parents=True, exist_ok=True)
    return TemporaryDirectory(prefix="nrn-test-", dir=base)
