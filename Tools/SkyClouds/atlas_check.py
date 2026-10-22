"""Check of the cloud noise atlas (WorldClient/SkyClouds.cpp BuildAtlas + ShaderSrc/SkyClouds.hlsl NoiseLevel):
the atlas lookup, bilinear inside a tile + the slice lerp, equals wrapped trilinear sampling of each mip level of
the 64^3 volume, as wxl-light's tex3Dlod read it.

    python3 -I Tools/SkyClouds/atlas_check.py [Tools/CustomMpq/VFog/cloudnoise.bin]
"""
import os
import random
import sys

W, H, N, LEVELS = 528, 720, 64, 6


def mips(vol):
    out = [vol]
    n = N
    while len(out) < LEVELS:
        h = n // 2
        prev = out[-1]
        cur = bytearray(h * h * h * 4)
        for z in range(h):
            for y in range(h):
                for x in range(h):
                    for ch in range(4):
                        s = 0
                        for k in range(8):
                            s += prev[(((2 * z + (k >> 2)) * n + 2 * y + ((k >> 1) & 1)) * n + 2 * x + (k & 1)) * 4 + ch]
                        cur[((z * h + y) * h + x) * 4 + ch] = (s + 4) // 8
        out.append(bytes(cur))
        n = h
    return out


def atlas(levels):            # SkyClouds.cpp BuildAtlas
    px = [None] * (W * H)
    y0, n = 0, N
    for lv in levels:
        cols, tile = min(n, 8), n + 2
        for z in range(n):
            ox, oy = (z % cols) * tile, y0 + (z // cols) * tile
            for j in range(tile):
                y = (j + n - 1) % n
                for i in range(tile):
                    x = (i + n - 1) % n
                    o = ((z * n + y) * n + x) * 4
                    px[(oy + j) * W + ox + i] = lv[o:o + 4]
        y0 += (n // cols) * tile
        n //= 2
    assert y0 == H, y0
    return px


def bilinear(px, u, v):        # a 2D texture, texel centres at i + 0.5, no wrap needed inside a tile
    x, y = u * W - 0.5, v * H - 0.5
    x0, y0 = int(x // 1), int(y // 1)
    fx, fy = x - x0, y - y0
    def t(i, j):
        return px[j * W + i]
    return [(t(x0, y0)[c] * (1 - fx) + t(x0 + 1, y0)[c] * fx) * (1 - fy)
            + (t(x0, y0 + 1)[c] * (1 - fx) + t(x0 + 1, y0 + 1)[c] * fx) * fy for c in range(4)]


def frac(a):
    return a - (a // 1)


def noise_level(px, uvw, L):   # SkyClouds.hlsl NoiseLevel
    n = 2.0 ** (6 - L)
    cols, tile = min(n, 8.0), n + 2.0
    y0 = [0, 528, 664, 700, 710, 716][L]
    p = [c * n for c in uvw]
    inx, iny = frac(p[0] / n) * n + 1.0, frac(p[1] / n) * n + 1.0 + y0
    z = p[2] - 0.5
    f = frac(z)
    s = z - f
    s -= n * (s // n)
    row = s // cols
    a = bilinear(px, ((s - row * cols) * tile + inx) / W, (row * tile + iny) / H)
    s = s + 1 if s + 1 < n else 0.0
    row = s // cols
    b = bilinear(px, ((s - row * cols) * tile + inx) / W, (row * tile + iny) / H)
    return [a[c] + (b[c] - a[c]) * f for c in range(4)]


def tex3d(lv, n, uvw):          # wrapped trilinear, as the sampler on wxl-light's volume
    c = [u * n - 0.5 for u in uvw]
    i0 = [int(v // 1) for v in c]
    t = [v - i for v, i in zip(c, i0)]
    out = [0.0] * 4
    for k in range(8):
        w = 1.0
        idx = []
        for a in range(3):
            bit = (k >> a) & 1
            w *= t[a] if bit else 1 - t[a]
            idx.append((i0[a] + bit) % n)
        o = ((idx[2] * n + idx[1]) * n + idx[0]) * 4
        for ch in range(4):
            out[ch] += w * lv[o + ch]
    return out


def main(argv):
    path = argv[0] if argv else os.path.join(os.path.dirname(__file__), '..', 'CustomMpq', 'VFog', 'cloudnoise.bin')
    vol = open(path, 'rb').read()
    assert len(vol) == N * N * N * 4
    levels = mips(vol)
    px = atlas(levels)
    rnd = random.Random(1)
    worst = 0.0
    for L in range(LEVELS):
        for _ in range(300):
            uvw = [rnd.uniform(-3.0, 3.0) for _ in range(3)]
            got = noise_level(px, uvw, L)
            want = tex3d(levels[L], N >> L, uvw)
            worst = max(worst, max(abs(g - w) for g, w in zip(got, want)))
    assert worst < 1e-3, worst
    print('atlas ok: worst difference %.2g of 255 over 6 levels' % worst)


if __name__ == '__main__':
    main(sys.argv[1:])
