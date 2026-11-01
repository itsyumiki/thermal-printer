import time
import unittest

from pinbridge.board import Board
from pinbridge.fake import FakeSerial
from pinbridge.protocol import BoardReset, PinError


class BoardBehaviourTests(unittest.TestCase):
    def test_error_mapping_and_event_queue(self):
        serial = FakeSerial()
        board = Board(serial_obj=serial).open()
        try:
            with self.assertRaises(PinError):
                board.mod("NO_SUCH_PIN", "in")
            serial.q.append(b"!PB3=0 @123\n")
            deadline = time.monotonic() + 0.5
            while (
                not any(k == "pin" for k, _ in board._events)
                and time.monotonic() < deadline
            ):
                time.sleep(0.01)
            self.assertIn(("pin", "!PB3=0 @123"), board._events)
        finally:
            board.close()

    def test_reset_detection_until_ack(self):
        serial = FakeSerial()
        board = Board(serial_obj=serial).open()
        try:
            serial.up = -100
            with self.assertRaises(BoardReset):
                board.command("HEL")
            with self.assertRaises(BoardReset):
                board.command("STA")
            board.ack_reset()
            self.assertEqual(board.command("HEL")[0], "jlpin")
        finally:
            board.close()

    def test_keepalive_feeds_fake(self):
        serial = FakeSerial()
        board = Board(serial_obj=serial).open()
        initial = board.up
        try:
            time.sleep(1.7)
            self.assertGreater(board.up, initial)
        finally:
            board.close()

    def test_context_manager_closes_serial(self):
        serial = FakeSerial()
        with Board(serial_obj=serial) as board:
            board.mod("PC7", "lo")
        self.assertTrue(serial.closed)
        self.assertEqual(serial.modes["PC7"], "off")


if __name__ == "__main__":
    unittest.main()
