"""Static contracts for SDK-bound firmware adapter choices not host-buildable here."""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
APP = ROOT / "firmware/apps/pinbridge/src/app.c"
WEB_APP = ROOT / "host/pinbridge/bun/public/app.js"
SERVER = ROOT / "host/pinbridge/bun/server.ts"
BUILD_SCRIPT = ROOT / "scripts/build-app.sh"
WORKFLOW = ROOT / ".github/workflows/pinbridge.yml"
WEB_HTML = ROOT / "host/pinbridge/bun/public/index.html"
TUNING = ROOT / "firmware/apps/pinbridge/src/pb_tuning.h"
TBP = ROOT / "host/pinbridge/bun/tbp.ts"
UNITS = ROOT / "host/pinbridge/bun/units.ts"


class FirmwareAdapterContractTests(unittest.TestCase):
    def test_step_timer_is_awake_only_during_motion(self):
        source = APP.read_text(encoding="utf-8")
        self.assertIn("sys_hi_timer_add(NULL, pb_step_timer, 1)", source)
        self.assertIn("sys_hi_timer_del(step_timer_id)", source)
        self.assertIn("if (core.step_active)", source)
        self.assertNotIn("sys_s_hi_timer_add(NULL, pb_step_timer, 1)", source)

    def test_heater_timer_does_not_claim_sdk_usr_timer_hardware_timer(self):
        # usr_timer (sys_hi_timer_add) runs on TIMER1; claiming it stopped the stepper tick
        # (bench PDIAG 2026-11-03: timer registered, ticks=0, no boot wiggle).
        source = APP.read_text(encoding="utf-8")
        code = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
        self.assertNotIn("JL_TIMER1", code)
        self.assertNotIn("IRQ_TIME1_IDX", code)
        self.assertIn("#define PB_HEATER_TIMER JL_TIMER3", source)
        self.assertIn("#define PB_HEATER_TIMER_IRQ IRQ_TIME3_IDX", source)

    def test_motor_phase_callback_uses_irq_safe_port_writer(self):
        printer = (ROOT / "firmware/apps/pinbridge/src/pb_printer.h").read_text(encoding="utf-8")
        phase = printer[printer.index("static void pb_drive_motor_phase"):printer.index("/* A head port") if "/* A head port" in printer else printer.index("/* Shift one fully validated")]
        self.assertIn("c->h.write_port_fast", phase)
        self.assertIn("pb_printer_pin_i(c, k, level)", phase)
        self.assertIn("unconditionally disables then enables IRQs", phase)

    def test_stepper_runtime_diagnostics_expose_timer_progress_and_wiggle_gates(self):
        app = APP.read_text(encoding="utf-8")
        core = (ROOT / "firmware/apps/pinbridge/src/pb_core.h").read_text(encoding="utf-8")
        printer = (ROOT / "firmware/apps/pinbridge/src/pb_printer.h").read_text(encoding="utf-8")
        self.assertIn("core.step_timer_callbacks++", app)
        self.assertIn("core.step_timer_reg_failures++", app)
        self.assertIn('pb_eq(v[0], "PDIAG")', core)
        for field in ("step=", "phase=", "done=", "rem=", "timer=", "ticks=", "regfail=", "wiggle=", "request=", "rowpaper=", "power=", "abort="):
            self.assertIn(field, printer)

    def test_ci_embeds_source_sha_in_hello_build_id(self):
        core = (ROOT / "firmware/apps/pinbridge/src/pb_core.h").read_text(encoding="utf-8")
        script = BUILD_SCRIPT.read_text(encoding="utf-8")
        workflow = WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("info[1] = (uint8_t)(PB_BUILD_ID >> 24)", core)
        self.assertIn("PINBRIDGE_BUILD_SHA", script)
        self.assertIn("#define PB_BUILD_ID 0x${BUILD_ID}u", script)
        self.assertIn("PINBRIDGE_BUILD_SHA: ${{ github.sha }}", workflow)

    def test_hello_recovery_retries_without_port_drop_and_discards_partial_lines(self):
        server = SERVER.read_text(encoding="utf-8")
        uart = (ROOT / "host/pinbridge/bun/uart.ts").read_text(encoding="utf-8")
        self.assertIn("const HELLO_TIMEOUT_MS = 400", server)
        self.assertIn("HELLO failed; retrying on open UART", server)
        self.assertIn("this.serial.discardPartialLine()", server)
        self.assertIn("discardPartialLine()", uart)

    def test_rx_overflow_resynchronizes_at_the_next_newline(self):
        app = APP.read_text(encoding="utf-8")
        overflow = app[app.index("if (rx_overflow)"):app.index("while ((n = rx_ring_pop", app.index("if (rx_overflow)"))]
        self.assertIn("core.discard = 1", overflow)
        self.assertIn("fast_line_discard = 1", overflow)

    def test_host_and_firmware_share_the_same_half_steps_per_mm_assumption(self):
        tuning = TUNING.read_text(encoding="utf-8")
        units = UNITS.read_text(encoding="utf-8")
        firmware_match = re.search(r"#define PB_TUNE_STEPS_PER_MM (\d+)u", tuning)
        host_match = re.search(r"export const STEPS_PER_MM = (\d+);", units)
        self.assertIsNotNone(firmware_match)
        self.assertIsNotNone(host_match)
        self.assertEqual(int(firmware_match.group(1)), int(host_match.group(1)))

    def test_row_shift_finishes_in_one_executor_poll(self):
        core = (ROOT / "firmware/apps/pinbridge/src/pb_core.h").read_text(encoding="utf-8")
        self.assertIn("for (i = 0; i < 384u && c->tbp_exec_bit < 384u; i++)", core)
        self.assertIn("if (c->tbp_exec_bit < 384u) return;", core)

    def test_host_heater_slice_limit_matches_firmware_tuning(self):
        tuning = TUNING.read_text(encoding="utf-8")
        tbp = TBP.read_text(encoding="utf-8")
        app = WEB_APP.read_text(encoding="utf-8")
        firmware_match = re.search(r"#define PB_TUNE_HEATER_SLICE_MAX_US (\d+)u", tuning)
        host_match = re.search(r"MAX_HEATER_SLICE_US = (\d+);", tbp)
        ui_match = re.search(r"const MAX_HEATER_SLICE_US = (\d+);", app)
        self.assertIsNotNone(firmware_match)
        self.assertIsNotNone(host_match)
        self.assertIsNotNone(ui_match)
        self.assertEqual(int(firmware_match.group(1)), int(host_match.group(1)))
        self.assertEqual(int(firmware_match.group(1)), int(ui_match.group(1)))
        self.assertIn("one shift/latch per row", app)
        self.assertIn("four synchronized events", app)
        self.assertIn("four feed edges", app)
        self.assertIn("thermal and duty safeguards remain active", app)

    def test_webui_shows_celsius_raw_tooltip_and_safety_limits(self):
        script = WEB_APP.read_text(encoding="utf-8")
        self.assertIn('Raw ADC: ${t.headTempRaw}', script)
        self.assertIn('°C', script)
        self.assertIn('Cutoff 70 °C · abort 75 °C · resume 50 °C', script)

    def test_webui_javascript_only_references_existing_dom_ids(self):
        script = WEB_APP.read_text(encoding="utf-8")
        page = WEB_HTML.read_text(encoding="utf-8")
        ids = set(re.findall(r'\bid="([^"]+)"', page))
        refs = set(re.findall(r'\$\("([^"]+)"\)', script))
        self.assertEqual(refs - ids, set())
        self.assertIn("function connectSocket()", script)
        self.assertIn("connectSocket();", script)
        self.assertIn("scheduleSocketReconnect();", script)

    def test_webui_shows_tick_quantized_effective_speed(self):
        script = WEB_APP.read_text(encoding="utf-8")
        page = WEB_HTML.read_text(encoding="utf-8")
        self.assertIn('Math.ceil(periodUs / 1000) * 1000', script)
        self.assertNotIn('id="rowSpeedEffective"', page)
        self.assertNotIn('id="rowMm"', page)
        self.assertNotIn('id="rowSpeed"', page)
        self.assertIn('id="moveSpeedEffective"', page)
        self.assertIn("15 ms", page)
        self.assertIn("step tick", page)


if __name__ == "__main__":
    unittest.main()
