import subprocess
import sys
import unittest
from pathlib import Path


class LiveGateTest(unittest.TestCase):
    def test_junit_verdict(self):
        tests_dir = Path(__file__).resolve().parent
        checker = tests_dir.parent / "scripts" / "check-live-junit.py"
        fixtures = tests_dir / "fixtures" / "live-junit"
        for fixture, ctest_status, status, summary in (
            ("pass", 0, 0, "2 passed, 0 skipped, 0 failed, 0 missing"),
            ("skip", 0, 1, "1 passed, 1 skipped, 0 failed, 0 missing"),
            ("fail", 0, 1, "1 passed, 0 skipped, 1 failed, 0 missing"),
            ("missing", 0, 1, "1 passed, 0 skipped, 0 failed, 1 missing"),
            ("pass", 8, 1, "CTest exited 8"),
            ("absent", 0, 1, "live gate failed"),
        ):
            with self.subTest(fixture=fixture, ctest_status=ctest_status):
                result = subprocess.run(
                    [sys.executable, checker, fixtures / f"{fixture}.xml", str(ctest_status)],
                    capture_output=True, text=True,
                )
                output = result.stdout + result.stderr
                self.assertEqual(result.returncode, status, output)
                self.assertIn(summary, output)
                self.assertEqual(len(output.splitlines()), 1, output)


if __name__ == "__main__":
    unittest.main()
