"""Run shared golden transcripts through the strict C runner and Python fake."""

import pathlib
import shutil
import subprocess
import tempfile
import unittest

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
                # Transcripts that say "@config real" (or are listed here) need the real pin table.
                runner = (
                    self.real_binary
                    if "@config real" in case.read_text()
                    or case.name == "boot-motor-released.txt"
                    else self.binary
                )
                subprocess.run(
                    [str(runner), str(case)],
                    check=True,
                    capture_output=True,
                    text=True,
                )

    def test_c_runner_rejects_wrong_reply(self):
        original = (TRANSCRIPTS / "tbp-hello.txt").read_text()
        original = original.replace("< 01810100CD2F\n", "< WRONG\n", 1)
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write(original)
            path = f.name
        try:
            result = subprocess.run(
                [str(self.real_binary), path], capture_output=True, text=True
            )
            self.assertNotEqual(result.returncode, 0)
        finally:
            pathlib.Path(path).unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
