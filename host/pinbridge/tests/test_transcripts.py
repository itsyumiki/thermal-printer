"""Run shared golden transcripts through the strict C runner and Python fake."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest

from pinbridge.fake import FakeSerial

ROOT = pathlib.Path(__file__).resolve().parents[3]
TRANSCRIPTS = ROOT / "firmware/apps/pinbridge/tests/transcripts"
C_RUNNER = ROOT / "firmware/apps/pinbridge/tests/c/pb_test.c"


class TranscriptTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="pinbridge-tests-")
        cls.binary = pathlib.Path(cls.tmp.name) / "pb_test"
        cls.real_binary = pathlib.Path(cls.tmp.name) / "pb_test_real"
        cc = shutil.which("gcc")
        if not cc:
            raise unittest.SkipTest(
                "gcc unavailable; skipping compiled C transcript runner"
            )
        common = [
            cc,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I" + str(C_RUNNER.parent),
            str(C_RUNNER),
        ]
        subprocess.run(common + ["-o", str(cls.binary)], check=True)
        subprocess.run(
            common[:1]
            + [
                '-DPB_PINS_HEADER="pb_pins.h"',
                "-I" + str(ROOT / "firmware/apps/pinbridge/src"),
            ]
            + common[1:]
            + ["-o", str(cls.real_binary)],
            check=True,
        )

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, "tmp"):
            cls.tmp.cleanup()

    def test_all_golden_transcripts_in_c(self):
        cases = sorted(TRANSCRIPTS.glob("*.txt"))
        self.assertGreaterEqual(len(cases), 5)
        for case in cases:
            with self.subTest(transcript=case.name):
                runner = self.real_binary if case.name == "real-table.txt" else self.binary
                subprocess.run(
                    [str(runner), str(case)],
                    check=True,
                    capture_output=True,
                    text=True,
                )

    def test_c_runner_rejects_wrong_reply(self):
        original = (TRANSCRIPTS / "smoke.txt").read_text()
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write(original.replace("< ok\n", "< WRONG\n", 1))
            path = f.name
        try:
            result = subprocess.run(
                [str(self.binary), path], capture_output=True, text=True
            )
            self.assertNotEqual(result.returncode, 0)
        finally:
            pathlib.Path(path).unlink(missing_ok=True)

    def _run_python_fake_transcript(self, content):
        serial = FakeSerial()
        for number, raw in enumerate(content.splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith("#") or line.startswith("@"):
                continue
            if line.startswith(">r "):
                serial.write((line[3:] + "\r\n").encode())
            elif line.startswith("> "):
                serial.write((line[2:] + "\n").encode())
            elif line.startswith("< "):
                expected = line[2:].replace("{cfg}", "546c66a8").replace("{int}", "10")
                actual = serial.readline().decode().strip()
                if actual != expected:
                    return False, (number, expected, actual)
        return True, None

    def test_python_fake_handshake_and_gpio_transcript_subset(self):
        content = (TRANSCRIPTS / "smoke.txt").read_text()
        ok, detail = self._run_python_fake_transcript(content)
        self.assertTrue(ok, detail)

    def test_python_runner_rejects_wrong_transcript(self):
        content = (TRANSCRIPTS / "smoke.txt").read_text().replace("< ok\n", "< WRONG\n", 1)
        ok, detail = self._run_python_fake_transcript(content)
        self.assertFalse(ok)
        self.assertIsNotNone(detail)


if __name__ == "__main__":
    unittest.main()
