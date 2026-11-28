from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest


class LayeringTest(unittest.TestCase):
    def check(self, files):
        checker = Path(__file__).resolve().parents[1] / "scripts" / "check-layering.py"
        with TemporaryDirectory() as directory:
            root = Path(directory)
            for name, text in files.items():
                (root / name).parent.mkdir(parents=True, exist_ok=True)
                (root / name).write_text(text)
            return subprocess.run([sys.executable, checker, root], capture_output=True, text=True)

    def test_layers_meet_only_at_public_headers(self):
        tree = {
            "include/flock/store.hpp": "",
            "src/database.hpp": '#include "flock/store.hpp"\n',
            "src/store.cpp": '#include "database.hpp"\n',
            "app/main.cpp": '#include "flock/store.hpp"\n#include "bots.hpp"\n#include <vector>\n',
            "app/bots.hpp": "",
            "app/tests/store.cpp": '#include "../../src/database.hpp"\n',
            "web/server.cpp": '#include "embedded_assets.hpp"\n',
        }
        result = self.check(tree)
        self.assertEqual(result.returncode, 0, result.stderr)

        for name, text, diagnostic in (
            ("app/access.cpp", '#include "../src/database.hpp"\n', 'app/ may not include "../src/database.hpp" from src/'),
            ("app/model.cpp", '#include "database.hpp"\n', 'app/ may not include "database.hpp" from src/'),
            ("web/server.cpp", '#include "../app/bots.hpp"\n', 'web/ may not include "../app/bots.hpp" from app/'),
            ("src/store.cpp", '#include "../app/bots.hpp"\n', 'src/ may not include "../app/bots.hpp" from app/'),
            ("include/flock/store.hpp", '#include "../../src/database.hpp"\n', 'public headers include project headers as "flock/..."'),
        ):
            with self.subTest(name=name):
                result = self.check(tree | {name: text})
                self.assertEqual(result.returncode, 1)
                self.assertIn(diagnostic, result.stderr)


if __name__ == "__main__":
    unittest.main()
