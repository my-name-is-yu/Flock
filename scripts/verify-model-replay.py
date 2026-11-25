import subprocess
import sys


def main():
    model, variant, expected = sys.argv[1:]
    replay = subprocess.run([sys.executable, model, variant], text=True, capture_output=True)
    print(replay.stdout, end="")
    print(replay.stderr, end="", file=sys.stderr)
    lines = replay.stdout.splitlines()
    if replay.returncode != 1 or replay.stderr or len(lines) < 2 or lines[0] != f"FAIL: {expected}":
        print(f"Expected {variant} to exit 1 and show a counterexample for: {expected}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
