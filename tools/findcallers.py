"""Find E8 (call rel32) / E9 (jmp rel32) instructions whose target lies in [lo, hi]. Read-only.
Usage: python -I findcallers.py <exe> <lo hex> <hi hex>"""
import struct, sys, mmap, re
path, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]; nsec = struct.unpack_from('<H', m, pe + 6)[0]; optsz = struct.unpack_from('<H', m, pe + 20)[0]
base = struct.unpack_from('<Q', m, pe + 24 + 24)[0]
for i in range(nsec):
    nm, vsz, va, rsz, rp = struct.unpack_from('<8sIIII', m, pe + 24 + optsz + i * 40)
    ch = struct.unpack_from('<I', m, pe + 24 + optsz + i * 40 + 36)[0]
    if not ch & 0x20000000: continue           # executable sections only
    sec = m[rp:rp + rsz]; sva = base + va
    for mt in re.finditer(b'[\xe8\xe9]', sec):
        o = mt.start()
        if o + 5 > len(sec): break
        tgt = sva + o + 5 + struct.unpack_from('<i', sec, o + 1)[0]
        if lo <= tgt <= hi:
            print('%s 0x%X -> 0x%X  (%s)' % ('call' if sec[o] == 0xE8 else 'jmp ', sva + o, tgt, nm.rstrip(b'\0').decode()))
