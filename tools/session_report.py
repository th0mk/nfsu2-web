#!/usr/bin/env python3
"""Analyse a recording of a real game session (PERFLOG) -> <dir>/report.md

    python3 tools/session_report.py <dir> [--window 5]   (window length, s)

Input: the files runtime/perflog.c writes (+ the powermetrics output power.txt,
if present).  Standard library + `nm` only.
"""
import bisect, collections, csv, datetime, glob, os, re, statistics as st, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline='') as f:
        r = csv.reader(f)
        header = next(r, None)
        if not header:
            return []
        out = []
        for row in r:
            if len(row) != len(header):
                continue
            d = {}
            for k, v in zip(header, row):
                try:
                    d[k] = float(v)
                except ValueError:
                    d[k] = v
            out.append(d)
        return out


def read_meta(d):
    m = {}
    for l in open(os.path.join(d, 'meta.txt')):
        k, _, v = l.strip().partition(' ')
        m.setdefault(k, v)
    return m


# ---- symbols ----
def source_map():
    """runtime/*.c|m function definitions -> file name (for the category)"""
    h = {}
    defn = re.compile(r'^[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$')
    for path in glob.glob(os.path.join(ROOT, 'runtime', '*.[cm]')):
        name = os.path.basename(path)
        for l in open(path, errors='replace'):
            if l.startswith((' ', '\t', '#', '/', '}', '{')):
                continue
            m = defn.match(l)
            if m and m.group(1) not in ('if', 'for', 'while', 'switch', 'return', 'sizeof'):
                h.setdefault(m.group(1), name)
    return h


class Symbols:
    def __init__(self, d, m):
        self.slide = int(m.get('slide', '0'))
        lo, hi = m.get('text', '0 0').split()
        self.lo, self.hi = int(lo, 16), int(hi, 16)
        binary = m['binary']
        out = subprocess.run(['nm', '-n', binary], capture_output=True, text=True).stdout
        self.addr, self.name = [], []
        for l in out.splitlines():
            p = l.split()
            if len(p) >= 3 and p[1] in 'tT':
                self.addr.append(int(p[0], 16) + self.slide)
                self.name.append(p[2][1:] if p[2].startswith('_') else p[2])
        self.sys_names = {}
        sy = os.path.join(d, 'symbols.txt')
        if os.path.exists(sy):
            for l in open(sy):
                p = l.split()
                if len(p) >= 2:
                    self.sys_names[int(p[0], 16)] = p[1] + ('' if len(p) < 3 else '')
        self.cache = {}

    def __call__(self, pc):
        r = self.cache.get(pc)
        if r is not None:
            return r
        if self.lo <= pc < self.hi:
            i = bisect.bisect_right(self.addr, pc) - 1
            r = self.name[i] if i >= 0 else '?'
        else:
            r = self.sys_names.get(pc, '?system')
        self.cache[pc] = r
        return r


WAITING = re.compile(r'kevent|psynch|mach_msg|semwait|nanosleep|__ulock|swtch|__select|workq|__wait4|cond_wait')


def category(name, sources):
    m = re.match(r'sub_00([0-9a-f]{6})', name)
    if m:
        a = int(m.group(1), 16)
        return 'D3DX (lift)' if 0x650000 <= a < 0x690000 else 'game (lift)'
    if WAITING.search(name):
        return 'wait/sleep'
    if re.match(r'(n[4-8]_|dxn|nat_|pl_plan|efx_)', name):
        return 'D3DX (native)'
    k = sources.get(name)
    if k:
        return {'com.c': 'D3D9 layer (com.c)', 'metal.m': 'Metal (metal.m)', 'd3dx_hle.c': 'D3DX (native)',
                'win32.c': 'Win32', 'win32_ext.c': 'Win32', 'win32_misc.c': 'Win32', 'vsemu.c': 'vsemu', 'dispatch.c': 'dispatch',
                'genexec.c': 'dispatch', 'mem.c': 'memory', 'enhance.c': 'HD (enhance)',
                'audio.m': 'sound', 'perflog.c': 'perflog'}.get(k, k)
    if name.startswith(('x87_', 'fcompare', 'fsw_', 'FLAGS', 'parity')):
        return 'game (lift)'
    if name in ('?', '?system'):
        return 'system (?)'
    if name.startswith(('objc_', '-[', '+[', 'MTL', 'IOGPU', 'AGX', 'CA', 'CF', '_dispatch', 'dispatch')):
        return 'system: Metal/ObjC'
    if name.startswith(('_platform_mem', 'memcpy', 'memmove', 'memset', 'bzero', '_platform_strcmp', 'strcmp')):
        return 'system: mem/str'
    return 'system: other'


def read_samples(d):
    path = os.path.join(d, 'samples.bin')
    data = open(path, 'rb').read() if os.path.exists(path) else b''
    i, out = 0, []
    while i + 16 <= len(data):
        t, gf, role, n, _ = struct.unpack_from('<QIBBH', data, i)
        i += 16
        if i + 8 * n > len(data):
            break
        pcs = struct.unpack_from('<%dQ' % n, data, i)
        i += 8 * n
        out.append((t / 1e3, gf, role, pcs))
    return out


# ---- powermetrics ----
def read_power(path):
    if not os.path.exists(path):
        return []
    samples, cur = [], None
    for l in open(path, errors='replace'):
        m = re.match(r'\*\*\* Sampled system activity \((.+?) [+-]\d{4}\) \(([\d.]+)ms elapsed\)', l)
        if m:
            t = datetime.datetime.strptime(m.group(1), '%a %b %d %H:%M:%S %Y').timestamp()
            cur = {'t': t, 'dt': float(m.group(2)) / 1000}
            samples.append(cur)
            continue
        if cur is None:
            continue
        for k, nm in (('cpu', 'CPU Power'), ('gpu', 'GPU Power'), ('top', 'Combined Power (CPU + GPU + ANE)')):
            if l.startswith(nm + ':') and k not in cur:
                cur[k] = float(l.split(':')[1].split()[0]) / 1000
        m = re.match(r'GPU HW active frequency: (\d+) MHz', l)
        if m and 'gf' not in cur:
            cur['gf'] = float(m.group(1))
        m = re.match(r'P0-Cluster HW active frequency: (\d+) MHz', l)
        if m and 'pf' not in cur:
            cur['pf'] = float(m.group(1))
        m = re.match(r'Current pressure level: (\w+)', l)
        if m:
            cur['pressure'] = m.group(1)
    return [o for o in samples if 'top' in o]


def mean(xs):
    xs = [x for x in xs if x is not None]
    return st.mean(xs) if xs else float('nan')


def main():
    d = sys.argv[1]
    win_s = 5.0
    if '--window' in sys.argv:
        win_s = float(sys.argv[sys.argv.index('--window') + 1])
    m = read_meta(d)
    epoch0 = int(m.get('start_epoch_us', '0')) / 1e6
    guest = read_csv(os.path.join(d, 'guest.csv'))
    render = read_csv(os.path.join(d, 'render.csv'))
    gpu = {int(r['frame']): r['gpu_us'] for r in read_csv(os.path.join(d, 'gpu.csv'))}
    sec = read_csv(os.path.join(d, 'persec.csv'))
    thr = read_csv(os.path.join(d, 'thread.csv'))
    power = read_power(os.path.join(d, 'power.txt'))
    for g in power:
        g['tms'] = (g['t'] - g['dt'] / 2 - epoch0) * 1e3   # sample midpoint, in session time
    thumbs = sorted(glob.glob(os.path.join(d, 'thumbs', '*.jpg')))
    thumb_t = [(float(os.path.basename(e).split('_')[0]), e) for e in thumbs]

    sb = Symbols(d, m)
    sources = source_map()
    samp = read_samples(d)

    R = []
    emit = R.append
    duration = (render[-1]['t_ms'] - render[0]['t_ms']) / 1e3 if len(render) > 1 else 0
    emit('# Session report: %s\n' % os.path.basename(os.path.normpath(d)))
    emit('build **%s**, binary `%s`, duration %.0f s, %d frames (render), %d guest frames, %d samples, %d screenshots\n'
        % (m.get('build', '?'), os.path.basename(m.get('binary', '?')), duration, len(render), len(guest), len(samp), len(thumbs)))

    # ---- overview ----
    ar = [r['interval_us'] / 1e3 for r in render if r['interval_us'] > 0]
    if ar:
        fps = 1000 / st.mean(ar)
        emit('## Overview\n')
        emit('| measure | value |\n|---|---|')
        emit('| average fps | %.1f |' % fps)
        for limit in (9, 12, 20, 34):
            n = sum(1 for x in ar if x > limit)
            emit('| frames > %d ms | %d (%.2f%%) |' % (limit, n, 100 * n / len(ar)))
        emit('| longest frame | %.1f ms |' % max(ar))
        if guest:
            emit('| guest CPU / frame | %.2f ms |' % mean(k['cpu_us'] / 1e3 for k in guest))
            emit('| frame limiter sleep (idle margin) / frame | %.2f ms |' % mean(k['sleep_us'] / 1e3 for k in guest))
        emit('| main thread encoding / frame | %.2f ms |' % mean(r['encode_us'] / 1e3 for r in render))
        if gpu:
            emit('| GPU / frame | %.2f ms |' % mean(v / 1e3 for v in gpu.values()))
        emit('| draws / frame | %.0f |' % mean(r['draws'] for r in render))
        if len(sec) > 1:
            dt = (sec[-1]['t_ms'] - sec[0]['t_ms']) / 1e3
            emit('| process CPU energy (rusage) | %.2f W (P cores %.2f W) |'
                % ((sec[-1]['energy_nj'] - sec[0]['energy_nj']) / 1e9 / dt,
                   (sec[-1]['penergy_nj'] - sec[0]['penergy_nj']) / 1e9 / dt))
            cyc = sec[-1]['cycles'] - sec[0]['cycles']
            ins = sec[-1]['instr'] - sec[0]['instr']
            if cyc > 0:
                emit('| IPC | %.2f |' % (ins / cyc))
        if power:
            emit('| powermetrics total / CPU / GPU | %.2f / %.2f / %.2f W |'
                % (mean(g['top'] for g in power), mean(g['cpu'] for g in power), mean(g['gpu'] for g in power)))
            emit('| GPU frequency / P0 frequency | %.0f / %.0f MHz |' % (mean(g.get('gf') for g in power), mean(g.get('pf') for g in power)))
        emit('')

    # ---- events ----
    if guest:
        top = lambda k: sum(x[k] for x in guest)
        emit('## Events (session total)\n')
        emit('| event | count | time |\n|---|---|---|')
        emit('| VS compiles | %d | |' % top('vs_n'))
        emit('| PS compiles | %d | |' % top('ps_n'))
        emit('| PSO creations | %d | |' % top('pso_n'))
        emit('| compile time in total | | %.1f ms |' % (top('compile_us') / 1e3))
        emit('| texture creations | %d | |' % top('tex_n'))
        emit('| texture uploads | %.1f MB | %.1f ms |' % (top('tex_kb') / 1024, top('tex_us') / 1e3))
        emit('')

    # ---- timeline ----
    t_first = render[0]['t_ms'] if render else 0
    t_last = render[-1]['t_ms'] if render else 0
    windows = []
    t = t_first
    while t < t_last:
        a, b = t, t + win_s * 1000
        rr = [r for r in render if a <= r['t_ms'] < b]
        gw = [k for k in guest if a <= k['t_ms'] < b]
        ss = [s for s in sec if a - 1000 <= s['t_ms'] < b + 1000]
        gg = [g for g in power if a <= g['tms'] < b]
        w = {'a': a, 'b': b, 'n': len(rr)}
        if rr:
            w['fps'] = len(rr) / (win_s)
            w['encode'] = mean(r['encode_us'] / 1e3 for r in rr)
            w['draws'] = mean(r['draws'] for r in rr)
            w['gpu'] = mean(gpu.get(int(r['frame'])) for r in rr) / 1e3 if gpu else float('nan')
            w['longest'] = max(r['interval_us'] for r in rr) / 1e3
        if gw:
            w['gcpu'] = mean(k['cpu_us'] / 1e3 for k in gw)
            w['sleep'] = mean(k['sleep_us'] / 1e3 for k in gw)
            w['compiles'] = sum(k['vs_n'] + k['ps_n'] + k['pso_n'] for k in gw)
            w['tex_mb'] = sum(k['tex_kb'] for k in gw) / 1024
        ss2 = [s for s in sec if a <= s['t_ms'] <= b]
        if len(ss2) >= 2:
            dt = (ss2[-1]['t_ms'] - ss2[0]['t_ms']) / 1e3
            w['cpuw'] = (ss2[-1]['energy_nj'] - ss2[0]['energy_nj']) / 1e9 / dt
            w['thermal'] = max(s['thermal'] for s in ss2)
        if gg:
            w['pm_cpu'] = mean(g['cpu'] for g in gg)
            w['pm_gpu'] = mean(g['gpu'] for g in gg)
            w['pm_gf'] = mean(g.get('gf') for g in gg)
        e = [x for x in thumb_t if a <= x[0] < b]
        w['thumb'] = os.path.relpath(e[len(e) // 2][1], d) if e else ''
        windows.append(w)
        t = b

    def f(w, k, fmt='%.2f'):
        v = w.get(k)
        return '' if v is None or v != v else fmt % v

    emit('## Timeline (%.0f s windows)\n' % win_s)
    emit('Guest CPU = the game thread\'s real CPU time per frame; sleep = the frame limiter\'s '
        'idle margin (high = relaxed).  CPU W = the process\'s own energy counter (no sudo needed); pm = powermetrics.\n')
    emit('| t (s) | fps | guest CPU ms | sleep ms | encoding ms | GPU ms | draws | longest ms | CPU W | pm CPU W | pm GPU W | GPU MHz | thermal | compiles | texture MB | screen |')
    emit('|' + '---|' * 16)
    for w in windows:
        emit('| %.0f | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |' % (
            (w['a'] - t_first) / 1e3, f(w, 'fps', '%.1f'), f(w, 'gcpu'), f(w, 'sleep'), f(w, 'encode'), f(w, 'gpu'),
            f(w, 'draws', '%.0f'), f(w, 'longest', '%.1f'), f(w, 'cpuw'), f(w, 'pm_cpu'), f(w, 'pm_gpu'),
            f(w, 'pm_gf', '%.0f'), f(w, 'thermal', '%.0f'), f(w, 'compiles', '%.0f'), f(w, 'tex_mb', '%.1f'),
            ('![](%s)' % w['thumb']) if w['thumb'] else ''))
    emit('')

    # ---- sample profile ----
    def profile(sel, title, n_self=30, n_incl=25):
        if not sel:
            return
        self_c, incl_c, cat_c = collections.Counter(), collections.Counter(), collections.Counter()
        for _, _, _, pcs in sel:
            names = [sb(p) for p in pcs]
            self_c[names[0]] += 1
            cat_c[category(names[0], sources)] += 1
            for a in set(names):
                if a not in ('?', '?system'):
                    incl_c[a] += 1
        N = len(sel)
        emit('### %s (%d samples)\n' % (title, N))
        emit('| category (self) | share |\n|---|---|')
        for k, v in cat_c.most_common():
            emit('| %s | %.1f%% |' % (k, 100 * v / N))
        emit('\n| function (self) | share | category |\n|---|---|---|')
        for k, v in self_c.most_common(n_self):
            emit('| `%s` | %.2f%% | %s |' % (k, 100 * v / N, category(k, sources)))
        emit('\n| function (inclusive) | share |\n|---|---|')
        for k, v in incl_c.most_common(n_incl):
            emit('| `%s` | %.1f%% |' % (k, 100 * v / N))
        emit('')

    guest_samp = [o for o in samp if o[2] == 0]
    main_samp = [o for o in samp if o[2] == 1]
    emit('## Profile: the game thread (1 ms sampling, wall clock)\n')
    emit('Share = share of the samples; waiting/sleep included.  For ms/frame: share x frame time (~8.33 ms).\n')
    profile(guest_samp, 'Whole session')

    # heaviest windows: by guest CPU
    heavy = sorted([w for w in windows if w.get('gcpu') == w.get('gcpu') and w.get('n', 0) > 50],
                  key=lambda w: -w['gcpu'])[:3]
    for w in heavy:
        win_samp = [o for o in guest_samp if w['a'] <= o[0] < w['b']]
        profile(win_samp, 'Heaviest window: t=%.0f-%.0f s, guest CPU %.2f ms/frame, GPU %s ms %s'
               % ((w['a'] - t_first) / 1e3, (w['b'] - t_first) / 1e3, w['gcpu'], f(w, 'gpu'),
                  ('![](%s)' % w['thumb']) if w['thumb'] else ''), 20, 0)
    emit('## Profile: the main (render) thread (4 ms sampling)\n')
    profile(main_samp, 'Whole session', 20, 15)

    # ---- hitches ----
    emit('## Hitches (frame interval > 12 ms)\n')
    hitches = [r for r in render if r['interval_us'] > 12000]
    if not hitches:
        emit('none\n')
    else:
        emit('| t (s) | frame | interval ms | nearby guest CPU ms (max) | compiles | texture KB | GPU ms | screen |\n|---|---|---|---|---|---|---|---|')
        for r in hitches[:60]:
            gw = [k for k in guest if r['t_ms'] - 60 <= k['t_ms'] <= r['t_ms'] + 20]
            e = min(thumb_t, key=lambda x: abs(x[0] - r['t_ms'])) if thumb_t else None
            emit('| %.2f | %d | %.1f | %s | %d | %d | %s | %s |' % (
                (r['t_ms'] - t_first) / 1e3, r['frame'], r['interval_us'] / 1e3,
                '%.1f' % max(k['cpu_us'] / 1e3 for k in gw) if gw else '',
                sum(k['vs_n'] + k['ps_n'] + k['pso_n'] for k in gw), sum(k['tex_kb'] for k in gw),
                '%.1f' % (gpu[int(r['frame'])] / 1e3) if int(r['frame']) in gpu else '',
                ('![](%s)' % os.path.relpath(e[1], d)) if e and abs(e[0] - r['t_ms']) < 1500 else ''))
        if len(hitches) > 60:
            emit('\n... %d hitches in total' % len(hitches))
        emit('')

    # ---- threads ----
    if thr:
        first, last = {}, {}
        for r in thr:
            tid = int(r['tid'])
            first.setdefault(tid, r)
            last[tid] = r
        dt = (thr[-1]['t_ms'] - thr[0]['t_ms']) / 1e3
        rows = []
        for tid, r in last.items():
            a = first[tid]
            usr = (r['user_ns'] - a['user_ns']) / 1e9 / dt
            sy = (r['sys_ns'] - a['sys_ns']) / 1e9 / dt
            rows.append((usr + sy, usr, sy, r.get('role', ''), r.get('name', ''), tid))
        rows.sort(reverse=True)
        emit('## Threads (cores = CPU s / wall s)\n')
        # samples of the other threads (role 2, 10 ms): most frequent PC < LR per thread
        others = collections.defaultdict(collections.Counter)
        for _, tid32, role, pcs in samp:
            if role == 2:
                others[tid32][' < '.join(sb(p) for p in pcs[:2])] += 1
        emit('| role | name | total | user | system | most frequent (PC < LR, 10 ms samples) |\n|---|---|---|---|---|---|')
        for t_, usr, sy, role, name, tid in rows[:16]:
            c = others.get(tid & 0xFFFFFFFF)
            top_pcs = ''
            if c:
                N = sum(c.values())
                top_pcs = '; '.join('`%s` %.0f%%' % (k, 100 * v / N) for k, v in c.most_common(3))
            emit('| %s | %s | %.3f | %.3f | %.3f | %s |' % (role, name or tid, t_, usr, sy, top_pcs))
        emit('| | **total** | **%.3f** | %.3f | %.3f | |' % (sum(x[0] for x in rows), sum(x[1] for x in rows), sum(x[2] for x in rows)))
        emit('')

    open(os.path.join(d, 'report.md'), 'w').write('\n'.join(R) + '\n')
    print('\n'.join(R[:40]))
    print('...\nreport: %s' % os.path.join(d, 'report.md'))


if __name__ == '__main__':
    main()
