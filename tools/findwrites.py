"""Scan a PE for 'mov byte ptr [reg+DISP32], IMM8' (C6 /0, mod=10) with a given displacement. Read-only.
Usage: python -I findwrites.py <exe> <disp hex> [imm]   (imm omitted = any value)"""
import struct, sys, mmap
path, disp = sys.argv[1], int(sys.argv[2], 16)
want = int(sys.argv[3], 0) if len(sys.argv) > 3 else None
f = open(path, 'rb'); m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
pe = struct.unpack_from('<I', m, 0x3C)[0]; nsec = struct.unpack_from('<H', m, pe + 6)[0]
optsz = struct.unpack_from('<H', m, pe + 20)[0]; base = struct.unpack_from('<Q', m, pe + 24 + 24)[0]
secs = [struct.unpack_from('<8sIIII', m, pe + 24 + optsz + i * 40) for i in range(nsec)]
d = struct.pack('<I', disp); pos = 0; hits = 0
while True:
    pos = m.find(d, pos)
    if pos < 0: break
    modrm, op = m[pos - 1], m[pos - 2]
    if op == 0xC6 and (modrm & 0xC0) == 0x80 and (modrm & 0x38) == 0 and (modrm & 7) != 4:
        imm = m[pos + 4]
        if want is None or imm == want:
            rex = m[pos - 3] if 0x40 <= m[pos - 3] <= 0x4F else None
            start = pos - (3 if rex else 2)
            for nm, vsz, va, rsz, rp in secs:
                if rp <= start < rp + rsz:
                    print('0x%X  %s  imm=%d  bytes %s' % (base + va + start - rp, nm.rstrip(b'\0').decode(), imm, m[start:pos + 5].hex(' ')))
            hits += 1
    pos += 1
print('hits:', hits)
