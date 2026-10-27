"""Find the primary vtable of an MSVC x64 class via RTTI in a PE file (read-only).
Usage: python -I rttivtable.py <exe> <ClassName> [vtable index ...]  -> prints vtable address and entries."""
import struct, sys, mmap
path, cls = sys.argv[1], sys.argv[2]
f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]
nsec = struct.unpack_from('<H', m, pe + 6)[0]; optsz = struct.unpack_from('<H', m, pe + 20)[0]
base = struct.unpack_from('<Q', m, pe + 24 + 24)[0]
secs = []
for i in range(nsec):
    nm, vsz, va, rsz, rp = struct.unpack_from('<8sIIII', m, pe + 24 + optsz + i * 40)
    secs.append((nm.rstrip(b'\0').decode(), va, vsz, rsz, rp))
def rva2off(rva):
    for n, va, vsz, rsz, rp in secs:
        if va <= rva < va + rsz: return rva - va + rp
def off2rva(o):
    for n, va, vsz, rsz, rp in secs:
        if rp <= o < rp + rsz: return o - rp + va
name = b'.?AV' + cls.encode() + b'@@\0'
so = m.find(name)
if so < 0: sys.exit('type descriptor not found for ' + cls)
td_rva = off2rva(so) - 16
print('type descriptor RVA 0x%X' % td_rva)
pat = struct.pack('<I', td_rva); hits = []
pos = 0
while True:
    pos = m.find(pat, pos)
    if pos < 0: break
    col = pos - 12
    sig, offset, cd = struct.unpack_from('<III', m, col)
    if sig == 1 and offset == 0:
        selfrva = struct.unpack_from('<I', m, col + 20)[0]
        if selfrva == off2rva(col): hits.append(col)
    pos += 1
for col in hits:
    colva = base + off2rva(col)
    p = m.find(struct.pack('<Q', colva))
    while p >= 0:
        vt = base + off2rva(p + 8)
        print('COL 0x%X  vtable 0x%X' % (colva, vt))
        for idx in sys.argv[3:]:
            i = int(idx, 0)
            fn = struct.unpack_from('<Q', m, p + 8 + i)[0]
            print('  [+0x%X] -> 0x%X' % (i, fn))
        p = m.find(struct.pack('<Q', colva), p + 1)
