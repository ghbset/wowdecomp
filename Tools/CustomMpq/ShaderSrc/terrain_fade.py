"""gfxShadowFade on the terrain: the stock Terrain2 / Terrain2_pcf / Terrain3 / Terrain3_pcf pixel shaders (every
dynamic-shadow terrain permutation, CMap::GetTerrainPixelShader) patched to fade their sun shadow, written as
Tools/CustomMpq/Shaders/Pixel/ps_3_0/<name>.bls (D3D9, and the source of the Vulkan translation) and ps_4_0 (D3D11,
shader-xlate's compile_one with its pcf_slots, as the SM4 pack). They replace the stock files by name in custom.MPQ.

    python3 -I Tools/CustomMpq/ShaderSrc/terrain_fade.py [--xlate /root/d3d11-port/shader-xlate]

Every stock permutation ends its shadow with `mad rD, rS, k.0.3, k.0.7` (a def'd constant: lit = 0.3 S + 0.7, S the
min of the shadow-map term and the chunk's baked shadow, also the specular's mask). Before it one instruction is
inserted, `mad rS.c, rS.c, c15.x, c15.y`, so S' = S f + (1 - f) with c15 = (f, 1 - f) from
CShadowCache::SetShadowMapTerrain (c15 is free in every terrain permutation: c0-c13 at most). f = 1 is S exactly.

The input is shader-xlate's stock/ps_3_0 (the client's files, byte-identical, stock/SHA1SUMS). The outputs are
derived from the client's shaders, so they are not committed (.gitignore); build_custom_mpq.py refuses to build
without them.
"""
import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'Shaders', 'Pixel')
FAMILIES = ('terrain2', 'terrain2_pcf', 'terrain3', 'terrain3_pcf')
FADE_REG = 15
K03, K07 = struct.unpack('<2I', struct.pack('<2f', 0.3, 0.7))


def reg(tok):
    return ((tok >> 28) & 7) | ((tok >> 8) & 0x18), tok & 0x7FF


def patch(code):
    """-> code with the fade inserted before the 0.3 / 0.7 mad; exactly one per permutation."""
    w = list(struct.unpack('<%dI' % (len(code) // 4), code))
    assert w[0] == 0xFFFF0300, 'not ps_3_0'
    defs, hits, i = {}, [], 1
    while w[i] != 0x0000FFFF:
        op = w[i] & 0xFFFF
        if op == 0xFFFE:  # comment
            i += 1 + ((w[i] >> 16) & 0x7FFF)
            continue
        n = (w[i] >> 24) & 0xF
        if op == 0x51:  # def
            defs[reg(w[i + 1])[1]] = w[i + 2:i + 6]
        elif op == 0x04:  # mad dst, s0, s1, s2
            dst, s0, s1, s2 = w[i + 1:i + 5]

            def const_val(s):
                t, r = reg(s)
                sw = (s >> 16) & 0xFF
                if t != 2 or r not in defs or s & 0x2000 or (s >> 24) & 0xF or sw not in (0x00, 0x55, 0xAA, 0xFF):
                    return None
                return defs[r][sw & 3]
            if const_val(s1) == K03 and const_val(s2) == K07:
                hits.append((i, s0))
        i += 1 + n
    assert len(hits) == 1, 'expected one 0.3/0.7 mad, found %d' % len(hits)
    at, s0 = hits[0]
    t, r = reg(s0)
    sw = (s0 >> 16) & 0xFF
    dmask = (w[at + 1] >> 16) & 0xF
    comps = {(sw >> (2 * k)) & 3 for k in range(4) if dmask >> k & 1}
    assert t == 0 and not s0 & 0x2000 and not (s0 >> 24) & 0xF and len(comps) == 1, 'S is not one plain temp component'
    c = comps.pop()
    rep = (c | c << 2 | c << 4 | c << 6) << 16
    ins = [0x04 | 4 << 24,
           0x80000000 | (1 << c) << 16 | r,                 # rS.c
           0x80000000 | rep | r,                            # rS.cccc
           0x80000000 | 2 << 28 | 0x00 << 16 | FADE_REG,    # c15.xxxx
           0x80000000 | 2 << 28 | 0x55 << 16 | FADE_REG]    # c15.yyyy
    assert FADE_REG not in defs
    return struct.pack('<%dI' % (len(w) + 5), *(w[:at] + ins + w[at:]))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--xlate', default='/root/d3d11-port/shader-xlate')
    a = ap.parse_args(argv)
    sys.path.insert(0, a.xlate)
    import xlate
    sums = {}
    for line in open(os.path.join(a.xlate, 'stock', 'SHA1SUMS')):
        h, p = line.split()
        sums[os.path.normpath(p.lstrip('*'))] = h
    for fam in FAMILIES:
        rel = os.path.join('ps_3_0', fam + '.bls')
        data = open(os.path.join(a.xlate, 'stock', rel), 'rb').read()
        if sums.get(rel) != hashlib.sha1(data).hexdigest():
            sys.exit('%s: not the stock file (SHA1SUMS)' % rel)
        sm3, sm4 = [], []
        for k, (ra, rb, rc, code) in enumerate(xlate.read_bls(data)):
            if not code:
                sm3.append((ra, rb, rc, code))
                sm4.append((ra, rb, rc, code))
                continue
            p = patch(code)
            st, why, dx = xlate.compile_one(p, None, xlate.pcf_slots(rel, k), rel)[:3]   # rel: lit families (modern lighting)
            if st != 'ok':
                sys.exit('%s:%d: xlate failed: %s %s' % (fam, k, st, why))
            sm3.append((ra, rb, rc, p))
            sm4.append((ra, rb, rc, dx))
        for prof, recs in (('ps_3_0', sm3), ('ps_4_0', sm4)):
            os.makedirs(os.path.join(OUT, prof), exist_ok=True)
            with open(os.path.join(OUT, prof, fam + '.bls'), 'wb') as f:
                f.write(xlate.write_bls(recs))
        print('%s: %d permutations patched' % (fam, len(sm3)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
