import queue
import time
import unittest

from pinbridge.board import Board
from pinbridge.fake import FakeSerial
from pinbridge.gui.adapter import (
    TaskAdapter,
    modes_for_pin,
    output_confirmation_required,
)


class GuiAdapterTests(unittest.TestCase):
    def test_mode_menu_caps_and_danger_gate(self):
        self.assertIn("hi", modes_for_pin("PC7"))
        self.assertNotIn("up", modes_for_pin("PB3"))
        self.assertNotIn("lo", modes_for_pin("PB3"))
        self.assertTrue(output_confirmation_required("PB11", "hi", set()))
        self.assertFalse(output_confirmation_required("PB11", "hi", {"PB11"}))
        self.assertFalse(output_confirmation_required("PC7", "hi", set()))

    def test_adapter_task_round_trip(self):
        q = queue.Queue()
        adapter = TaskAdapter(q, mock=True)
        try:
            got = []
            q.put(
                {
                    "window": True,
                    "action": "set_direction",
                    "pin": "PC7",
                    "value": "OUT",
                    "callback": got.append,
                }
            )
            deadline = time.monotonic() + 1
            while not got and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(got)
            self.assertEqual(got[0]["type"], "ack")
            read = []
            q.put(
                {
                    "window": True,
                    "action": "get_value",
                    "pin": "PC7",
                    "callback": read.append,
                }
            )
            deadline = time.monotonic() + 1
            while not read and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(read)
            self.assertEqual(read[0]["type"], "data")
        finally:
            adapter.close()

    def test_close_sends_reset(self):
        serial = FakeSerial()
        board = Board(serial_obj=serial).open()
        board.mod("PC7", "lo")
        board.close()
        self.assertTrue(serial.closed)


if __name__ == "__main__":
    unittest.main()
