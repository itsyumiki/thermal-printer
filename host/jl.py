"""JieLi CRC16 and the flash scrambler, in pure Python.

Replaces jl-misctools' jltech.crc / jltech.cipher (and its crcmod dependency) for
the two things the image builder needs. Same results: patch_app.py checks every
entry-header CRC and the app_area_head name of a real dump, so a wrong CRC or
cipher here fails loudly instead of producing a bad image.
"""


def _make_table():
    table = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        table.append(c)
    return table


_TABLE = _make_table()


def jl_crc16(data, crc=0):
    """CRC-16/XMODEM (poly 0x1021, init 0, no reflection); `crc` continues a run."""
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _TABLE[(crc >> 8) ^ b]
    return crc


def jl_enc_cipher(buf, off, size, key=0xFFFF):
    """JieLi "ENC" cipher: XOR with a 16-bit LFSR stream, in place."""
    for i in range(size):
        buf[off + i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF
    return key


def jl_sfc_cipher(buf, off, size, base, key, blocksize=32):
    """Flash scrambler: ENC per 32-byte block, key ^ ((offset - base) >> 2). In place."""
    for i in range(0, size, blocksize):
        jl_enc_cipher(buf, off + i, min(size - i, blocksize), key ^ ((off + i - base) >> 2))
