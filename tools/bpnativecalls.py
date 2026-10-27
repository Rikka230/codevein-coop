"""Find Kismet calls to an imported (native) function in a cooked UE 4.18 Blueprint and dump the bytes around each call.
Read-only. Usage: python -I bpnativecalls.py <tools dir> <paks dir> <AssetName> <FunctionName> [bytes after]
Calls to natives are EX_FinalFunction (0x1C) / EX_CallMath (0x68) / EX_LocalFinalFunction (0x46) followed by the
function's package index (int32, negative = import)."""
import sys, os, struct
sys.path.insert(0, sys.argv[1])
import pakv4

paks, asset, func = sys.argv[2], sys.argv[3], sys.argv[4]
after = int(sys.argv[5]) if len(sys.argv) > 5 else 48
order = sorted([f for f in os.listdir(paks) if f.endswith('.pak')],
               key=lambda f: (f.replace('_0_P', '').replace('_0_p', ''), '_0_p' in f.lower()))
src = {}
for f in order:
    p = pakv4.Pak(os.path.join(paks, f))
    for n in p.entries:
        for ext in ('.uasset', '.uexp'):
            if n.endswith('/' + asset + ext): src[ext] = (p, n)
ua = src['.uasset'][0].read(src['.uasset'][1])
o = 8 + 4 + 4 + 4
nc, = struct.unpack_from('<i', ua, o); o += 4 + nc * 20 + 4
_, o = pakv4._fstr(ua, o); o += 4                       # folder name, package flags
name_count, name_off, _gtc, _gto, exp_count, exp_off, imp_count, imp_off = struct.unpack_from('<8i', ua, o)
q = name_off; names = []
for _ in range(name_count):
    s, q = pakv4._fstr(ua, q); q += 4; names.append(s)
imports = []
for i in range(imp_count):
    b = imp_off + i * 28
    cls_pkg, _, cls_name, _, outer, obj_name, _ = struct.unpack_from('<iiiiiii', ua, b)
    imports.append((names[cls_name], names[obj_name], outer))
hits = [i for i, (c, n, _) in enumerate(imports) if n == func and c == 'Function']
print(asset, 'imports', imp_count, '| matches for', func, ':', hits)
data = src['.uexp'][0].read(src['.uexp'][1])
for i in hits:
    pat = struct.pack('<i', -(i + 1))
    pos = 0
    while True:
        pos = data.find(pat, pos)
        if pos < 0: break
        op = data[pos - 1]
        if op in (0x1C, 0x68, 0x46):
            print('at %d op 0x%02X: %s' % (pos - 1, op, data[pos + 4:pos + 4 + after].hex(' ')))
        pos += 1

# helper so the bytes after the call can be read: print the import index of every Function import used nearby
print('function imports (index -> name):')
for i, (c, n, outer) in enumerate(imports):
    if c == 'Function': print('  %d (pkgidx %d) %s' % (i, -(i + 1), n))
