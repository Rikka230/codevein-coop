"""Minimal read-only UE4 pak v4 reader (UE 4.18). Lists entries and reads files (zlib or stored)."""
import struct, zlib, os, sys

def _fstr(b, o):
    n, = struct.unpack_from('<i', b, o); o += 4
    if n == 0: return '', o
    if n < 0:
        s = b[o:o - n * 2].decode('utf-16-le').rstrip('\0'); return s, o - n * 2
    s = b[o:o + n].decode('latin-1').rstrip('\0'); return s, o + n

class Pak:
    def __init__(self, path):
        self.path = path; self.f = open(path, 'rb')
        size = os.path.getsize(path)
        self.f.seek(size - 44); t = self.f.read(44)
        magic, ver, ioff, isz = struct.unpack_from('<IiQQ', t, 0)
        assert magic == 0x5A6F12E1, 'bad magic'
        self.version = ver
        self.f.seek(ioff); b = self.f.read(isz)
        mount, o = _fstr(b, 0)
        n, = struct.unpack_from('<i', b, o); o += 4
        self.mount = mount; self.entries = {}
        for _ in range(n):
            name, o = _fstr(b, o)
            off, sz, usz, comp = struct.unpack_from('<qqqi', b, o); o += 28 + 20
            blocks = []
            if comp:
                nb, = struct.unpack_from('<i', b, o); o += 4
                for _ in range(nb):
                    s, e = struct.unpack_from('<qq', b, o); o += 16; blocks.append((s, e))
            enc = b[o]; o += 1
            bs, = struct.unpack_from('<I', b, o); o += 4
            self.entries[mount + name] = (off, sz, usz, comp, blocks, enc, bs)

    def read(self, name):
        off, sz, usz, comp, blocks, enc, bs = self.entries[name]
        if enc: raise ValueError('encrypted entry')
        if not comp:
            # stored: data follows the serialized entry header (48 + 4 + 1 + 4 bytes for v4 uncompressed)
            hdr = 8 + 8 + 8 + 4 + 20 + 1 + 4
            self.f.seek(off + hdr); return self.f.read(usz)
        out = bytearray()
        for s, e in blocks:
            self.f.seek(s); out += zlib.decompress(self.f.read(e - s))
        return bytes(out)

if __name__ == '__main__':
    p = Pak(sys.argv[1])
    print(p.version, p.mount, len(p.entries))
