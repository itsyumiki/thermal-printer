"""Regression tests for the 384-dot printhead direction demo."""

import importlib.util
import pathlib
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "examples" / "print_width_ramp_ws_demo.py"
SPEC = importlib.util.spec_from_file_location("print_width_ramp_ws_demo", SCRIPT)
DEMO = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(DEMO)


class PrintWidthRampTests(unittest.TestCase):
    def test_bands_alternate_and_fill_exactly_384_dots(self):
        bands = DEMO.band_pattern()
        self.assertEqual(sum(width for _, width in bands), 384)
        self.assertEqual(bands[:6], [(False, 1), (True, 1), (False, 2), (True, 2), (False, 3), (True, 3)])
        self.assertEqual(bands[-1], (False, 4))
        self.assertEqual(len(bands), 39)

    def test_mask_is_48_bytes_lsb_first_and_black_count_is_190(self):
        mask = DEMO.make_mask()
        self.assertEqual(len(mask), 48)
        bits = [(mask[i // 8] >> (i % 8)) & 1 for i in range(384)]
        self.assertEqual(sum(bits), 190)
        self.assertEqual(bits[:10], [0, 1, 0, 0, 1, 1, 0, 0, 0, 1])
        self.assertEqual(bits[380:], [0, 0, 0, 0])

    def test_only_384_dot_rows_are_supported(self):
        with self.assertRaises(ValueError):
            DEMO.make_mask(383)


if __name__ == "__main__":
    unittest.main()
