#
# This file is distributed under the MIT License. See LICENSE.md for details.
#
# RUN: python3 %s %root/bin/revng %t

"""Exercise CLR support diagnostics and checked header reads through the CLI.

These complete synthetic PE files test importer structure and bounds. Genuine
source-built CLR assemblies are also needed to validate the managed support
boundary; these structural fixtures do not claim to contain managed programs.
"""

from pathlib import Path
import struct
import subprocess
import sys


def make_pe(path, clr=False, flags=0, directory_size=72, header_size=72, rva=0x2000):
    data = bytearray(0x600)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x14C, 2, 0, 0, 0, 224, 0x2102)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x10B)
    struct.pack_into("<III", data, optional + 16, 0x1000, 0x1000, 0x2000)
    struct.pack_into("<III", data, optional + 28, 0x400000, 0x1000, 0x200)
    struct.pack_into("<HH", data, optional + 40, 6, 0)
    struct.pack_into("<II", data, optional + 56, 0x3000, 0x200)
    struct.pack_into("<H", data, optional + 68, 3)
    struct.pack_into("<I", data, optional + 92, 16)
    if clr:
        struct.pack_into("<II", data, optional + 96 + 14 * 8, rva, directory_size)
    for offset, name, address, raw, attributes in [
        (0x178, b".text", 0x1000, 0x200, 0x60000020),
        (0x1A0, b".rdata", 0x2000, 0x400, 0x40000040),
    ]:
        data[offset:offset + len(name)] = name
        struct.pack_into("<IIIIIIHHI", data, offset + 8,
                         0x200, address, 0x200, raw, 0, 0, 0, 0, attributes)
    data[0x200:0x206] = b"\xb8\x01\x00\x00\x00\xc3"
    struct.pack_into("<IHHIII", data, 0x400, header_size, 2, 5, 0, 0, flags)
    path.write_bytes(data)


revng = sys.argv[1]
work = Path(sys.argv[2])
work.mkdir()
cases = [
    ("native", {}, None),
    ("mixed-mode-header", {"clr": True}, None),
    ("pure-il", {"clr": True, "flags": 1}, "COMIMAGE_FLAGS_ILONLY"),
    ("small-directory", {"clr": True, "directory_size": 16}, "directory is too small"),
    ("small-header", {"clr": True, "header_size": 16}, "inconsistent size"),
    ("oversized-header", {"clr": True, "header_size": 128}, "inconsistent size"),
    ("unbacked-header", {"clr": True, "rva": 0x21FC}, ""),
    ("out-of-image-header", {"clr": True, "rva": 0xFFFFFFF0}, ""),
]
for name, options, diagnostic in cases:
    binary = work / (name + ".dll")
    make_pe(binary, **options)
    command = [revng, "quick", "analyze", "--analyses=parse-binary", str(binary),
               "-o", str(work / (name + ".yml"))]
    result = subprocess.run(command, capture_output=True, text=True)
    output = result.stdout + result.stderr
    assert (result.returncode == 0) == (diagnostic is None), (name, result.returncode, output)
    assert result.returncode >= 0, (name, output)
    if diagnostic is not None:
        assert diagnostic in output, (name, output)
    assert "Stack dump:" not in output, (name, output)
    print(name + ": passed")
