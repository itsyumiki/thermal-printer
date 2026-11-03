"""Static contracts for SDK-bound firmware adapter choices not host-buildable here."""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
APP = ROOT / "firmware/apps/pinbridge/src/app.c"
WEB_APP = ROOT / "host/pinbridge/bun/public/app.js"
WEB_HTML = ROOT / "host/pinbridge/bun/public/index.html"


class FirmwareAdapterContractTests(unittest.TestCase):
    def test_step_timer_is_awake_only_during_motion(self):
        source = APP.read_text(encoding="utf-8")
        self.assertIn("sys_hi_timer_add(NULL, pb_step_timer, 1)", source)
        self.assertIn("sys_hi_timer_del(step_timer_id)", source)
        self.assertIn("if (core.step_active || core.wiggle_stage)", source)
        self.assertNotIn("sys_s_hi_timer_add(NULL, pb_step_timer, 1)", source)

    def test_webui_shows_tick_quantized_effective_speed(self):
        script = WEB_APP.read_text(encoding="utf-8")
        page = WEB_HTML.read_text(encoding="utf-8")
        self.assertIn('Math.ceil(periodUs / 1000) * 1000', script)
        self.assertIn('id="rowSpeedEffective"', page)
        self.assertIn('id="moveSpeedEffective"', page)


if __name__ == "__main__":
    unittest.main()
