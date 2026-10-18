# Resolve samples.bin from sampler.c against the exe symbol table.
# usage: python prof.py exe samples.bin [top]
import bisect, collections, struct, subprocess, sys

exe, path = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = []
for line in subprocess.run(['C:/msys64/mingw64/bin/nm.exe', '-n', '-C', exe], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in 'tT':
        syms.append((int(p[0], 16), p[2]))
addrs = [a for a, _ in syms]
# PE image base used by nm
imgbase = 0x140000000

data = open(path, 'rb').read()
base = struct.unpack_from('<Q', data, 0)[0]
off = 8
self_c, incl_c = collections.Counter(), collections.Counter()
total = 0
samples = []
mods = []
while off < len(data):
    n = struct.unpack_from('<i', data, off)[0]; off += 4
    if n < 0:
        for i in range(-n):
            b, e = struct.unpack_from('<QQ', data, off); off += 16
            name = data[off:off + 64].split(bytes(1))[0].decode(); off += 64
            mods.append((b, e, name))
        break
    samples.append(struct.unpack_from('<%dQ' % n, data, off)); off += 8 * n
exe_mod = [m for m in mods if m[0] == base]
exe_end = exe_mod[0][1] if exe_mod else 1 << 64


def name_of(pc):
    if pc < base or pc >= exe_end:
        for b, e, nm in mods:
            if b <= pc < e:
                return '[' + nm + ']'
        return '[?]'
    a = pc - base + imgbase
    j = bisect.bisect_right(addrs, a) - 1
    return syms[j][1] if j >= 0 else '?'


for pcs in samples:
    total += 1
    seen = set()
    for i, pc in enumerate(pcs):
        name = name_of(pc)
        if i == 0:
            self_c[name] += 1
        if name not in seen:
            seen.add(name)
            incl_c[name] += 1
print('samples', total)
print('--- self')
for k, v in self_c.most_common(top):
    print('%6.2f%%  %s' % (100.0 * v / total, k))
print('--- inclusive')
for k, v in incl_c.most_common(top):
    print('%6.2f%%  %s' % (100.0 * v / total, k))
