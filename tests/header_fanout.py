import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest


class HeaderFanoutTest(unittest.TestCase):
    def test_project_header_limits(self):
        checker = Path(__file__).resolve().parents[1] / "scripts" / "check-header-fanout.py"
        for measured, recorded, status, diagnostic in (
            (30, None, 0, ""),
            (31, None, 1, "project.hpp: measured 31, limit 30"),
            (31, 31, 0, ""),
            (32, 31, 1, "project.hpp: measured 32, recorded 31"),
            (30, 31, 1, "project.hpp: measured 30, recorded 31"),
            (0, 31, 1, "project.hpp: measured 0, recorded 31"),
        ):
            with self.subTest(measured=measured, recorded=recorded), TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / "source"
                source.mkdir()
                build = root / "build"
                target = build / "CMakeFiles" / "flock.dir"
                target.mkdir(parents=True)
                (target / "support.o.d").write_text("support.o: ../source/support.hpp\n")
                for index in range(measured):
                    (target / f"{index}.o.d").write_text(f"{index}.o: ../source/project.hpp\n")
                allowlist = root / "allowlist.json"
                allowlist.write_text(json.dumps({} if recorded is None else {"project.hpp": recorded}))

                result = subprocess.run(
                    [sys.executable, checker, source, build, allowlist], capture_output=True, text=True,
                )
                self.assertEqual(result.returncode, status, result.stdout + result.stderr)
                if diagnostic:
                    self.assertIn(diagnostic, result.stderr)
                else:
                    self.assertIn("Header fan-out passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
