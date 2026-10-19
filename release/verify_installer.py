#!/usr/bin/env python3
"""Check that the self-contained installer embeds the exact built payloads."""
import hashlib
from pathlib import Path
import struct
import sys

def verify(build):
    data = (build / "fan-addon-setup.exe").read_bytes()
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    assert data[pe:pe+4] == b"PE\0\0"
    count, optional_size = struct.unpack_from("<H", data, pe+6)[0], struct.unpack_from("<H", data, pe+20)[0]
    optional = pe+24
    assert struct.unpack_from("<H", data, optional)[0] == 0x10b, "Expected PE32 installer"
    sections = []
    for i in range(count):
        sections.append(struct.unpack_from("<IIII", data, optional+optional_size+i*40+8))
    def offset(rva):
        for virtual_size, start, raw_size, raw_start in sections:
            if start <= rva < start+max(virtual_size, raw_size):
                return raw_start+rva-start
        raise ValueError("Unmapped resource address")
    root = offset(struct.unpack_from("<I", data, optional+96+16)[0])
    def entries(relative):
        pos = root+relative
        named, ids = struct.unpack_from("<HH", data, pos+12)
        return [struct.unpack_from("<II", data, pos+16+i*8) for i in range(named+ids)]
    def child(relative, resource_id):
        return next(target for name, target in entries(relative) if name == resource_id)
    rcdata = child(0, 10)
    assert rcdata & 0x80000000
    for resource_id, name in ((201, "scmhd.sdp"), (202, "minesim.exe"), (203, "ScmDraft-Fan-Addon-NOTICES.txt")):
        languages = child(rcdata & 0x7fffffff, resource_id)
        assert languages & 0x80000000
        leaf = entries(languages & 0x7fffffff)[0][1]
        assert not leaf & 0x80000000
        rva, size = struct.unpack_from("<II", data, root+leaf)
        payload = data[offset(rva):offset(rva)+size]
        expected = (build / name).read_bytes()
        assert payload == expected, "Embedded payload differs: "+name
        print(name+": embedded bytes verified; SHA256 "+hashlib.sha256(payload).hexdigest())

if __name__ == "__main__":
    verify(Path(sys.argv[1] if len(sys.argv)>1 else "build/release"))
