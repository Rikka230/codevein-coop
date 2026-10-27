"""Find Kismet calls to a function name inside a cooked Blueprint (uasset name table + uexp bytecode) and dump the bytes after each call."""
import sys, os, struct
sys.path.insert(0, sys.argv[1])
import pakv4
paks, asset, func = sys.argv[2], sys.argv[3], sys.argv[4]
order = sorted([f for f in os.listdir(paks) if f.endswith('.pak')], key=lambda f: (f.replace('_0_P', '').replace('_0_p',''), '_0_p' in f.lower()))
src = {}
for f in order:
    p = pakv4.Pak(os.path.join(paks, f))
    for n in p.entries:
        for ext in ('.uasset', '.uexp'):
            if n.endswith('/' + asset + ext): src[ext] = (p, n)
print({k: v[1] for k, v in src.items()})
ua = src['.uasset'][0].read(src['.uasset'][1])
o = 8 + 4 + 4 + 4
nc, = struct.unpack_from('<i', ua, o); o += 4 + nc * 20 + 4
_, o = pakv4._fstr(ua, o); o += 4
namecount, nameoff = struct.unpack_from('<ii', ua, o)
q = nameoff; names = []
for _ in range(namecount):
    s, q = pakv4._fstr(ua, q); q += 4; names.append(s)
idx = names.index(func); print(func, 'name index', idx)
data = src['.uexp'][0].read(src['.uexp'][1]) if '.uexp' in src else ua
pat = struct.pack('<ii', idx, 0)
pos = 0
while True:
    pos = data.find(pat, pos)
    if pos < 0: break
    before = data[pos-1]
    after = data[pos+8:pos+8+48]
    print('at', pos, 'opcode before = 0x%02X' % before, 'next bytes:', after.hex(' '))
    pos += 1
