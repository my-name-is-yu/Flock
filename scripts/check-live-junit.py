import sys
from xml.etree import ElementTree


EXPECTED_TESTS = {"flock_live_t3", "flock_live_conversation"}


def check_junit(report, ctest_status):
    suite = ElementTree.parse(report).getroot()
    if suite.tag != "testsuite":
        raise ValueError("expected a CTest JUnit testsuite")

    seen = set()
    passed = 0
    skipped, failed = [], []
    for case in suite.findall("testcase"):
        name = case.get("name")
        if not name or name in seen:
            raise ValueError(f"missing or duplicate testcase name {name!r}")
        seen.add(name)

        status = case.get("status")
        if status == "run":
            passed += 1
        elif status in {"notrun", "disabled"}:
            skipped.append(name)
        else:
            failed.append(name)

    missing = sorted(EXPECTED_TESTS - seen)
    problems = [
        f"{label} {', '.join(names)}"
        for label, names in (("skipped", skipped), ("failed", failed), ("missing", missing))
        if names
    ]
    if ctest_status:
        problems.append(f"CTest exited {ctest_status}")
    verdict = "failed" if problems else "passed"
    counts = f"{passed} passed, {len(skipped)} skipped, {len(failed)} failed, {len(missing)} missing"
    details = f" — {' / '.join(problems)}" if problems else ""
    print(f"live gate {verdict} ({counts}){details}")
    return int(bool(problems))


if __name__ == "__main__":
    try:
        sys.exit(check_junit(sys.argv[1], int(sys.argv[2])))
    except (OSError, ElementTree.ParseError, ValueError) as error:
        print(f"live gate failed ({error})", file=sys.stderr)
        sys.exit(1)
