import unittest

from pinbridge.board import Board
from pinbridge.fake import FakeSerial
from pinbridge.protocol import ProtocolError


class Tests(unittest.TestCase):
    def test_handshake_and_alias(self):
        b = Board(serial_obj=FakeSerial()).open()
        self.assertEqual(b.up, 10)
        b.mod("led", "lo")
        b.set(led=1)
        self.assertIn("PC7=1", b.get("PC7")[0])
        b.close()

    def test_hash_mismatch(self):
        s = FakeSerial()
        s._cmd = lambda line: s.q.append(b"ok jlpin 2 deadbeef up=1\n")
        with self.assertRaises(ProtocolError):
            Board(serial_obj=s).open()


if __name__ == "__main__":
    unittest.main()
