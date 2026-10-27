"""Byte-search every .uasset/.umap (latest pak wins) for the given keywords. Read-only."""
import sys, os
sys.path.insert(0, sys.argv[1])
import pakv4
paks = sys.argv[2]; keys = [k.encode() for k in sys.argv[3:]]
order = sorted([f for f in os.listdir(paks) if f.endswith('.pak')], key=lambda f: (f.replace('_0_P', '').replace('_0_p',''), '_0_p' in f.lower()))
hits = {}
for f in order:
    p = pakv4.Pak(os.path.join(paks, f))
    for n in p.entries:
        if not n.endswith(('.uasset', '.umap')): continue
        try: b = p.read(n)
        except Exception: continue
        h = [k.decode() for k in keys if k in b]
        path = n.split('Content/', 1)[-1]
        if h: hits[path] = h
        elif path in hits: del hits[path]
for path, h in sorted(hits.items()): print(path, h)
