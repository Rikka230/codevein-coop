"""Search every .uasset/.umap header in the paks for keyword names. Resumable; writes local/bpscan.json."""
import sys, os, json, time
sys.path.insert(0, os.path.dirname(__file__))
import pakv4
PAKS = os.path.expanduser('~/mnt/CODE VEIN/CodeVein/Content/Paks')
OUT = os.path.expanduser('~/mnt/code-vein-coop/local/bpscan.json')
KEYS = [b'SetIsSessionOperationProhibition', b'DestroyOnlineSession', b'CloseOnlineSession', b'OpenOnlineSession',
        b'ExpectsBeOpen', b'RescueSignal', b'NumPublicConnections', b'MaxPlayers', b'OnLeaveOnlineSessionGest',
        b'OnLostConnectionWithHost', b'OnDestroyOnlineSession', b'IsOnlineMultiplay', b'GetOnlineOtherPlayerNum',
        b'TeleportToHost', b'ESessionOpenness', b'CreateOnlineSession', b'JoinOnlineSession', b'SessionManager',
        b'MistWall', b'BossAreaState', b'OnBranchOfJoiningOnlineSession', b'IsOnlineSessionBusy', b'ShouldBeOpen',
        b'SetAppSessionSettings', b'FindAndJoinOnlineSessions', b'OnNewConnectionInOnlineSession', b'Guest', b'Multiplay']
state = json.load(open(OUT)) if os.path.exists(OUT) else {'done': [], 'hits': {}}
deadline = time.time() + float(sys.argv[1] if len(sys.argv) > 1 else 160)
# later paks override earlier ones: process in order, later hit lists replace earlier for the same path
order = sorted(os.listdir(PAKS), key=lambda f: (f.replace('_0_P', ''), '_0_P' in f))
for f in order:
    p = pakv4.Pak(os.path.join(PAKS, f))
    for n in p.entries:
        if not n.endswith(('.uasset', '.umap')): continue
        key = f + '|' + n
        if key in state['done']: continue
        if time.time() > deadline:
            json.dump(state, open(OUT, 'w')); print('PARTIAL', len(state['done'])); sys.exit(0)
        try:
            b = p.read(n)
        except Exception as e:
            b = b''
        hits = [k.decode() for k in KEYS if k in b]
        path = n.split('Content/', 1)[-1]
        if hits: state['hits'][path] = hits
        elif path in state['hits']: del state['hits'][path]
        state['done'].append(key)
json.dump(state, open(OUT, 'w')); print('COMPLETE', len(state['done']), 'hits', len(state['hits']))
