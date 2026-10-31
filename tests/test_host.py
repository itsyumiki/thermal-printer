"""Tests for host/br23t.py against a fake Transporter (no hardware).

Needs the printer's original dump and any built app.bin, which are not in this repo:

    BR23_DUMP=/path/to/dump.bin BR23_APP=/path/to/app.bin python3 -m unittest discover -s tests -v
"""
import argparse
import contextlib
import io
import os
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE), str(HERE.parent / "host")]

import br23t  # noqa: E402
from fake_transporter import GOOD_INFO, SECTOR, WRITE_END, WRITE_MIN, FakeSerial  # noqa: E402

DUMP, APP = os.environ.get("BR23_DUMP"), os.environ.get("BR23_APP")


def quiet(fn, *a):
    with contextlib.redirect_stdout(io.StringIO()) as buf:
        fn(*a)
    return buf.getvalue()


@unittest.skipUnless(DUMP and APP, "set BR23_DUMP and BR23_APP")
class HostFlows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dump = Path(DUMP).read_bytes()
        cls.app = Path(APP).read_bytes()
        with contextlib.redirect_stdout(io.StringIO()):
            cls.img, cls.changed = br23t.plan_image(cls.dump, cls.app)
        # the image build is slow in pure Python; the flows below reuse it
        cls._real_plan = staticmethod(br23t.plan_image)
        br23t.plan_image = lambda dump, app, key=br23t.CHIP_KEY: (cls.img, cls.changed)

    @classmethod
    def tearDownClass(cls):
        br23t.plan_image = cls._real_plan

    def setUp(self):
        flash = bytearray(self.dump)
        flash[0x70000] ^= 0xFF           # device data the printer rewrote since the dump
        self.fs = FakeSerial(flash)
        self.original = bytes(flash)
        self.args = argparse.Namespace(app=Path(APP), dump=Path(DUMP), key=0x3496,
                                       write=False, run=False, wait=0)

    def flash(self, **kw):
        vars(self.args).update(kw)
        return quiet(br23t.cmd_flash, self.fs, self.args)

    # --- the image itself
    def test_plan_stays_inside_the_window(self):
        self.assertTrue(self.changed)
        self.assertGreaterEqual(min(self.changed), WRITE_MIN)
        self.assertLess(max(self.changed), WRITE_END)
        outside = [a for a in range(0, len(self.dump), SECTOR)
                   if not WRITE_MIN <= a < WRITE_END and self.img[a:a + SECTOR] != self.dump[a:a + SECTOR]]
        self.assertEqual(outside, [])

    # --- flash
    def test_dry_run_writes_nothing(self):
        out = self.flash()
        self.assertIn("dry run", out)
        self.assertEqual(self.fs.writes, [])
        self.assertEqual(bytes(self.fs.flash), self.original)

    def test_write_touches_only_the_window_and_header_sector_last(self):
        self.flash(write=True)
        self.assertTrue(self.fs.writes)
        self.assertTrue(all(WRITE_MIN <= a < WRITE_END for a in self.fs.writes))
        self.assertEqual(len(set(self.fs.writes)), len(self.fs.writes))
        self.assertEqual(self.fs.writes[-1], WRITE_MIN)
        self.assertEqual(bytes(self.fs.flash[WRITE_MIN:WRITE_END]), self.img[WRITE_MIN:WRITE_END])
        self.assertEqual(bytes(self.fs.flash[:WRITE_MIN]), self.original[:WRITE_MIN])
        self.assertEqual(bytes(self.fs.flash[WRITE_END:]), self.original[WRITE_END:])   # incl. device data

    def test_second_flash_has_nothing_to_write(self):
        self.flash(write=True)
        n = len(self.fs.writes)
        self.assertIn("already holds", self.flash(write=True))
        self.assertEqual(len(self.fs.writes), n)

    def test_run_sends_runapp(self):
        self.assertIn("OK", self.flash(write=True, run=True).splitlines()[-1])

    def test_wrong_chip_is_refused(self):
        self.fs.info = "OK key=980F type=3 id=856014"
        with self.assertRaises(SystemExit):
            self.flash(write=True)
        self.assertEqual(self.fs.writes, [])

    def test_changed_boot_area_is_refused(self):
        self.fs.flash[0x1000] ^= 1
        with self.assertRaises(SystemExit):
            self.flash(write=True)
        self.assertEqual(self.fs.writes, [])

    def test_write_failure_stops_before_the_header_sector(self):
        self.fs.fail_write_at = 0x10000
        with self.assertRaises(SystemExit):
            self.flash(write=True)
        self.assertNotIn(WRITE_MIN, self.fs.writes)

    def test_missing_loader_is_explained(self):
        self.fs.loader = False
        with self.assertRaises(SystemExit) as cm:
            self.flash(write=True)
        self.assertIn("br23loader", str(cm.exception))

    # --- restore / selftest
    def test_restore_puts_the_factory_app_back(self):
        self.flash(write=True)
        quiet(br23t.cmd_restore, self.fs, argparse.Namespace(dump=Path(DUMP), write=True, run=False, wait=0))
        self.assertEqual(bytes(self.fs.flash[:WRITE_END]), self.dump[:WRITE_END])

    def test_selftest_write_changes_nothing(self):
        args = argparse.Namespace(ref=Path(DUMP), sector=0x50000, write=True, wait=0)
        quiet(br23t.cmd_selftest_write, self.fs, args)
        self.assertEqual(self.fs.writes, [0x50000])
        self.assertEqual(bytes(self.fs.flash), self.original)

    def test_selftest_refuses_outside_the_window(self):
        args = argparse.Namespace(ref=Path(DUMP), sector=0x70000, write=True, wait=0)
        with self.assertRaises(SystemExit):
            quiet(br23t.cmd_selftest_write, self.fs, args)
        self.assertEqual(self.fs.writes, [])


class Offline(unittest.TestCase):
    def test_crc16_known_value(self):
        from jl import jl_crc16
        self.assertEqual(jl_crc16(b"123456789"), 0x31C3)     # CRC-16/XMODEM check value

    def test_fake_matches_the_profile(self):
        self.assertEqual(GOOD_INFO, br23t.EXPECT_INFO)
        self.assertEqual((WRITE_MIN, WRITE_END), (br23t.WRITE_MIN, br23t.WRITE_END))


if __name__ == "__main__":
    unittest.main()
