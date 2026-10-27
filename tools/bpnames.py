"""Print the FName table of chosen assets from the test copy's paks (latest pak wins)."""
import sys, os, struct
sys.path.insert(0, sys.argv[1])  # tools dir with pakv4.py
import pakv4
paks_dir = sys.argv[2]; wanted = sys.argv[3:]
order = sorted(os.listdir(paks_dir), key=lambda f: (f.replace('_0_P', ''), '_0_P' in f))
found = {}
for f in order:
    if not f.endswith('.pak'): continue
    p = pakv4.Pak(os.path.join(paks_dir, f))
    for n in p.entries:
        for w in wanted:
            if n.endswith('/' + w + '.uasset'): found[w] = (p, n)
for w, (p, n) in found.items():
    b = p.read(n)
    # FPackageFileSummary: tag, legacy ver, ... find NameCount/NameOffset by scanning header for folder name
    o = 4; lv, = struct.unpack_from('<i', b, o); o += 4
    o += 4  # legacy ue3 ver
    o += 4 + 4  # file ver ue4, licensee
    nc, = struct.unpack_from('<i', b, o); o += 4 + nc * 20  # custom versions (guid+int)
    o += 4  # total header size
    s, o = pakv4._fstr(b, o)  # folder name
    o += 4  # package flags
    namecount, nameoff = struct.unpack_from('<ii', b, o)
    q = nameoff; names = []
    for _ in range(namecount):
        s, q = pakv4._fstr(b, q); q += 4  # hash (4.18: two uint16)
        names.append(s)
    print('=====', w, n, len(names))
    print('\n'.join(names))
