"""gxNormalBuffer's shader pack for D3D9 and D3D11: every world vs_3_0 / ps_3_0 shader with the view-space
normal written to oC1 (Tools/VulkanShaders/bls/normals.py), as ps_3_0 / vs_3_0 (D3D9) and, through shader-xlate's
compile_one, ps_4_0 / vs_4_0 (D3D11). Written to Tools/CustomMpq/Normals/{Vertex,Pixel}/<profile>/<name>.bls;
build_custom_mpq.py packs them as Shaders\\... above everything else. Vulkan needs nothing from here: its
translator injects the same rewrite and lists both md5s (GBUF-STATUS.md).

    python3 -I Tools/CustomMpq/ShaderSrc/terrain_fade.py          # first: its terrain shaders are the input here
    python3 -I Tools/CustomMpq/ShaderSrc/normal_pack.py [--xlate /root/d3d11-port/shader-xlate]

Input: shader-xlate's stock/{vs,ps}_3_0 (the client's files, checked against stock/SHA1SUMS), with a file of the
same name in Tools/CustomMpq/Shaders/<Kind>/<profile>/ (ours: terrain_fade's Terrain2/3) taken instead. The output
is derived from the client's shaders: gitignored, never committed. Deterministic: same input, same bytes.
"""
import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CUSTOM = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(os.path.dirname(CUSTOM), 'VulkanShaders'))
from bls import normals  # noqa: E402

OURS = os.path.join(CUSTOM, 'Shaders')
OUT = os.path.join(CUSTOM, 'Normals')
KIND = {'vs': 'Vertex', 'ps': 'Pixel'}


def ours(kind, prof, name):
    d = os.path.join(OURS, kind, prof)
    for f in os.listdir(d) if os.path.isdir(d) else ():
        if f.lower() == name.lower() + '.bls':
            return os.path.join(d, f)
    return None


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--xlate', default='/root/d3d11-port/shader-xlate')
    a = ap.parse_args(argv)
    sys.path.insert(0, a.xlate)
    import xlate
    stock = os.path.join(a.xlate, 'stock')
    sums = {}
    for line in open(os.path.join(stock, 'SHA1SUMS')):
        h, p = line.split()
        sums[os.path.normpath(p.lstrip('*'))] = h
    counts = {}
    for prof in ('vs_3_0', 'ps_3_0'):
        kind = KIND[prof[:2]]
        for f in sorted(os.listdir(os.path.join(stock, prof))):
            name = f[:-4]
            if not f.endswith('.bls') or not normals.wanted(name, prof[:2]):
                continue
            rel = os.path.join(prof, f)
            src = ours(kind, prof, name)
            data = open(src or os.path.join(stock, rel), 'rb').read()
            if not src and sums.get(rel) != hashlib.sha1(data).hexdigest():
                sys.exit('%s: not the stock file (SHA1SUMS)' % rel)
            try:
                perms = xlate.read_bls(data)
            except ValueError:      # a legacy GXVS / GXPS file: Wow.exe never loads it
                continue
            sm3, sm4 = [], []
            for k, (ra, rb, rc, code) in enumerate(perms):
                if code:
                    code = normals.rewrite_record(name, prof[:2], code)
                    st, why, dx = xlate.compile_one(code, None, xlate.pcf_slots(rel, k))[:3]
                    if st != 'ok':
                        sys.exit('%s:%d: xlate failed: %s %s' % (rel, k, st, why))
                else:
                    dx = code
                sm3.append((ra, rb, rc, code))
                sm4.append((ra, rb, rc, dx))
            for p, recs in ((prof, sm3), (prof.replace('_3_0', '_4_0'), sm4)):
                os.makedirs(os.path.join(OUT, kind, p), exist_ok=True)
                with open(os.path.join(OUT, kind, p, f), 'wb') as fh:
                    fh.write(xlate.write_bls(recs))
            counts[prof] = counts.get(prof, 0) + 1
            print('%-28s %s%d permutations' % (rel, '(ours) ' if src else '', len(sm3)))
    print('%s: %s' % (OUT, ', '.join('%d %s files (+ SM4)' % (n, p) for p, n in sorted(counts.items()))))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
