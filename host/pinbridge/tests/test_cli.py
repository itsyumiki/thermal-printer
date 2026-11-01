import pathlib
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
ENV = {"PYTHONPATH": str(ROOT / "host/pinbridge")}


class CliTests(unittest.TestCase):
    def test_gui_help_is_lazy_and_headless(self):
        p = subprocess.run(
            [sys.executable, "-m", "pinbridge", "gui", "--help"],
            cwd=ROOT,
            env=ENV,
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("--mock", p.stdout)
        self.assertIn("--port", p.stdout)

    def test_webui_help_is_headless(self):
        p = subprocess.run(
            [sys.executable, "-m", "pinbridge", "webui", "--help"],
            cwd=ROOT,
            env=ENV,
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("--http-port", p.stdout)
        self.assertIn("--no-browser", p.stdout)

    def test_repl_help(self):
        p = subprocess.run(
            [sys.executable, "-m", "pinbridge", "repl", "--help"],
            cwd=ROOT,
            env=ENV,
            capture_output=True,
            text=True,
            timeout=3,
        )
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("--port", p.stdout)


if __name__ == "__main__":
    unittest.main()
