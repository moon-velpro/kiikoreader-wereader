#!/usr/bin/env python3
"""Validate one-pass ZIP stored entries made by the host C regression binary."""
from __future__ import annotations

import struct
import sys
import zipfile
from pathlib import Path

p = Path(sys.argv[1])
with zipfile.ZipFile(p, "r") as zf:
    assert zf.testzip() is None
    assert zf.namelist() == ["c0.xhtml", "nav.xhtml"]
    assert zf.read("c0.xhtml") == b"<html>\nHello EPUB!\n</html>\n"
    assert zf.read("nav.xhtml") == b""
    for entry in zf.infolist():
        assert entry.compress_type == zipfile.ZIP_STORED
        assert entry.flag_bits & 0x8  # data descriptor
        assert entry.file_size == entry.compress_size
        assert entry.CRC == zipfile.crc32(zf.read(entry.filename))
        with p.open("rb") as source:
            source.seek(entry.header_offset)
            local = source.read(30)
            assert len(local) == 30
            assert struct.unpack_from("<I", local)[0] == 0x04034B50
            assert struct.unpack_from("<H", local, 6)[0] & 0x8
            assert struct.unpack_from("<III", local, 14) == (0, 0, 0)
            name_len, extra_len = struct.unpack_from("<HH", local, 26)
            source.seek(name_len + extra_len + entry.file_size, 1)
            descriptor = source.read(16)
            assert struct.unpack_from("<IIII", descriptor) == (
                0x08074B50, entry.CRC, entry.file_size, entry.file_size
            )
print("PASS: Python zipfile CRC, stored members, descriptor, local header, empty member")
