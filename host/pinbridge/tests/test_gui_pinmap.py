import unittest

from pinbridge.gui.app import ALL_KNOWN, DENIED, PIN_INFO, _caps, _description


class GuiPinMapTests(unittest.TestCase):
    def test_all_documented_pins_are_visible(self):
        self.assertEqual(set(ALL_KNOWN), set(PIN_INFO) | set(DENIED))
        self.assertEqual(len(ALL_KNOWN), len(PIN_INFO) + len(DENIED))
        self.assertIn("PA1", ALL_KNOWN)
        self.assertIn("PB5", ALL_KNOWN)
        self.assertIn("DP", ALL_KNOWN)
        self.assertIn("DM", ALL_KNOWN)

    def test_pa0_and_pa1_are_controllable(self):
        for pin in ("PA0", "PA1"):
            self.assertNotEqual(_caps(pin), "RESERVED")
            self.assertTrue(_description(pin))
        self.assertIn("power-control", _description("PA1"))

    def test_system_and_usb_entries_are_not_mapped_as_gpio(self):
        for pin in ("PA3", "PB5", "DP", "DM"):
            self.assertEqual(_caps(pin), "RESERVED")
            self.assertTrue(_description(pin))

    def test_descriptions_cover_all_enabled_pins(self):
        for pin in PIN_INFO:
            with self.subTest(pin=pin):
                self.assertTrue(_description(pin))
                self.assertNotEqual(_caps(pin), "RESERVED")


if __name__ == "__main__":
    unittest.main()
