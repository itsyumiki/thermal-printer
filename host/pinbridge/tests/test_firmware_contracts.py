"""Static contracts for SDK-bound firmware adapter choices not host-buildable here."""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
APP = ROOT / "firmware/apps/pinbridge/src/app.c"


class FirmwareAdapterContractTests(unittest.TestCase):
    def test_step_timer_is_awake_only_during_motion(self):
        source = APP.read_text(encoding="utf-8")
        self.assertIn("sys_hi_timer_add(NULL, pb_step_timer, 1)", source)
        self.assertIn("sys_hi_timer_del(step_timer_id)", source)
        self.assertIn("if (core.step_active || core.wiggle_stage)", source)
        self.assertNotIn("sys_s_hi_timer_add(NULL, pb_step_timer, 1)", source)


if __name__ == "__main__":
    unittest.main()
