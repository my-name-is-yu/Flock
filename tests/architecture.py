from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest


class ArchitectureTest(unittest.TestCase):
    def check(self, files):
        checker = Path(__file__).resolve().parents[1] / "scripts" / "check-architecture.py"
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

    def test_json_responses_have_one_writer(self):
        send = 'res.set_content(body.dump(), "application/json");\n'
        self.assertEqual(self.check({"web/http.cpp": send, "web/tests/http.cpp": send}).returncode, 0)
        result = self.check({"web/http.cpp": send, "web/git_routes.cpp": send})
        self.assertEqual(result.returncode, 1)
        self.assertIn("web/git_routes.cpp: send JSON responses with web::reply or web::error", result.stderr)

    def test_hex_encoding_has_one_implementation(self):
        loop = 'constexpr std::string_view digits = "0123456789abcdef";\n'
        self.assertEqual(self.check({"src/digest.cpp": loop}).returncode, 0)
        result = self.check({"src/digest.cpp": loop, "app/model.cpp": loop})
        self.assertEqual(result.returncode, 1)
        self.assertIn("app/model.cpp: encode hex with encodeHex and hash with sha256Hex", result.stderr)

    def test_environment_variables_are_named_in_one_header(self):
        name = 'std::getenv("FLOCK_HOME");\n'
        self.assertEqual(self.check({"include/flock/environment.hpp": name, "app/tests/home.cpp": name}).returncode, 0)
        result = self.check({"include/flock/environment.hpp": name, "app/main.cpp": name})
        self.assertEqual(result.returncode, 1)
        self.assertIn("app/main.cpp: name environment variables in flock/environment.hpp", result.stderr)

    def test_pages_share_one_element_helper(self):
        define = "export function node(tag, className, text) {}\n"
        self.assertEqual(self.check({"web/assets/dom.js": define}).returncode, 0)
        for copy in ("function node(tag, className, text) {}\n", "function element(tag, text, className) {}\n"):
            with self.subTest(copy=copy):
                result = self.check({"web/assets/dom.js": define, "web/assets/settings.js": copy})
                self.assertEqual(result.returncode, 1)
                self.assertIn("web/assets/settings.js: create elements with node from dom.js", result.stderr)
        self.assertEqual(self.check({"web/assets/app.js": "import { node } from './dom.js';\n"}).returncode, 0)
        result = self.check({"web/assets/app.js": "import { node } from '/dom.js';\n"})
        self.assertEqual(result.returncode, 1)
        self.assertIn("web/assets/app.js: import page scripts by relative path", result.stderr)

    def test_storage_components_do_not_refer_to_each_other_in_a_cycle(self):
        def wiring(journal):
            return {
                "src/storage.hpp": "struct Store::Impl {\n"
                "    TurnRunner turnRunner{database, conversationTurns, conversationTools};\n"
                "    ConversationTools conversationTools{database, conversationTurns};\n"
                "    ConversationTurns conversationTurns{database, journalStorage};\n"
                "    JournalStorage journalStorage" + journal + ";\n"
                "};\n"
            }

        self.assertEqual(self.check(wiring("{database}")).returncode, 0)
        result = self.check(wiring("{database, conversationTools}"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("conversationTools, conversationTurns, journalStorage refer to each other in a cycle", result.stderr)

    def test_private_file_check_has_one_implementation(self):
        check = "if (info.st_mode & 0077) return false;\n"
        self.assertEqual(self.check({"src/private_files.cpp": check, "src/workspace.cpp": "mode = info.st_mode & 0777;\n"}).returncode, 0)
        for copy in (check, "if ((info.st_mode & 07777) != 0600) return false;\n"):
            with self.subTest(copy=copy):
                result = self.check({"src/private_files.cpp": check, "app/settings_file.cpp": copy})
                self.assertEqual(result.returncode, 1)
                self.assertIn("app/settings_file.cpp: check that a file is private with ownerOnly", result.stderr)

    def test_page_state_imports_no_other_page_script(self):
        self.assertEqual(self.check({"web/assets/app-state.js": "import { x } from './composer-drafts.mjs';\n"}).returncode, 0)
        result = self.check({"web/assets/app-state.js": "import { render } from './app.js';\n"})
        self.assertEqual(result.returncode, 1)
        self.assertIn("web/assets/app-state.js: keep the page state free of imports", result.stderr)

    def test_tests_share_one_check_function(self):
        define = "void check(bool condition, std::string_view message) {}\n"
        self.assertEqual(self.check({"tests/support.hpp": define}).returncode, 0)
        for name in ("tests/core.cpp", "web/tests/http.cpp", "t3/tests.cpp"):
            with self.subTest(name=name):
                result = self.check({"tests/support.hpp": define, name: define})
                self.assertEqual(result.returncode, 1)
                self.assertIn(f"{name}: use check from tests/support.hpp", result.stderr)


if __name__ == "__main__":
    unittest.main()
