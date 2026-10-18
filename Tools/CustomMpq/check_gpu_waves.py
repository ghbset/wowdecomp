#!/usr/bin/env python3
"""Offline equivalence check: VFog GPU waves (VFogWave*.hlsl, VolumetricFogWaterWavesGpu.cpp) vs the CPU path
(VolumetricFogWaterWaves.cpp). Reimplements both in numpy; the GPU side rounds to float16 at every render-target
store (h0 upload, evolve, each butterfly stage, moments, foam, mips, slopes) and to 8 bits at the state store.
Runs every tile of VFog/waterdata.bin at 128 and 256 for a few steps (foam included) and asserts the slopes and
state agree. Usage: python3 Tools/CustomMpq/check_gpu_waves.py"""
import math, os, struct, sys
import numpy as np

f32, f16 = np.float32, np.float16
HERE = os.path.dirname(os.path.abspath(__file__))

# ---- constants (VolumetricFogWaterWaves.cpp) ----
G, INV_G, TWO_PI_D, LOOP = 9.80665, 0.10197161883115768, 6.2831854820251465, 1024.0
VAR_RANGE, WIDE_RANGE, MAX_DT = 0.25, 1.0, 0.1
WIND = np.array([0.8, 0.6], dtype=f32).astype(np.float64)
WIND /= math.hypot(*WIND)
HALF_CEILING = 16384.0          # GPU: largest |h0| texel after the prescale (VolumetricFogWaterWaves.cpp WaveSpectrum)


def tiles():
    b = open(os.path.join(HERE, "VFog", "waterdata.bin"), "rb").read()
    off, cnt, rs = struct.unpack_from("<3I", b, 24)
    return [struct.unpack_from("<I10f", b, off + i * rs) for i in range(cnt)]


def noise(u, v):
    h = np.sin(u * 12.9898 + v * 78.233) * 43758.546875
    return np.clip(h - np.floor(h), 1e-16, 1.0)


def gaussian(u, v, ro, ao):
    r = np.sqrt(-2.0 * np.log(noise(u + ro, v + ro)))
    a = TWO_PI_D * noise(u + ao, v + ao)
    return r * np.cos(a), r * np.sin(a)


def spectrum(kx, ky, amp, inv_peak2, align):
    sq = kx * kx + ky * ky
    mag = np.sqrt(sq)
    with np.errstate(all="ignore"):
        along = (kx * WIND[0] + ky * WIND[1]) / mag
        p = amp / (sq * sq) * np.exp(-inv_peak2 / sq) * np.power(np.maximum(along, 0.0), align)
        out = np.sqrt(p) / math.sqrt(2.0)
    return np.where((mag < 1e-3) | ~(along > 0.0), 0.0, out)


def configure(tile, n, wind_speed, amp_scale):
    _, size, amplitude, windmul, align = tile[:5]
    L = max(1.0, math.floor(size))
    amp = amplitude * amp_scale
    wind = windmul * wind_speed
    peak = wind * wind * INV_G
    inv_peak2 = min(1.0 / (peak * peak), 1e30) if peak > 0 else 1e30
    lprr = f32(LOOP * math.sqrt(G / (TWO_PI_D * L)))
    kx = (TWO_PI_D * (np.arange(n) - 0.5 * n) / L).astype(f32)
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    u, v = x / n, y / n
    tr, ti = gaussian(u, v, 0.13, 0.07)
    ar, _ = gaussian(u, v, 0.29, 0.19)
    KX, KY = kx[None, :].astype(np.float64).repeat(n, 0), kx[:, None].astype(np.float64).repeat(n, 1)
    s1 = spectrum(KX, KY, amp, inv_peak2, align)
    s2 = spectrum(-KX, -KY, amp, inv_peak2, align)
    h0t = (tr * s1 + 1j * ti * s1).astype(np.complex64)
    h0a = (ar * s2).astype(f32)
    cx, cy = x - 0.5 * n, y - 0.5 * n
    root = np.sqrt(np.sqrt(cx * cx + cy * cy)).astype(f32)
    freq = np.floor(root * lprr + f32(0.5)).astype(f32)
    return dict(n=n, L=L, kx=kx, h0t=h0t, h0a=h0a, freq=freq, lprr=lprr,
                foam=tile[5:8], oxy=tile[8:11])


def stencil_consts(c):
    n, L = c["n"], c["L"]
    tpu = n / L
    wide = min(max(int(math.floor(2.0 * tpu + 0.5)), 1), n // 2)
    return f32(0.5 * tpu), wide, f32(tpu / (2.0 * wide))


def foam_step(prev, jac, p, dt):
    f = np.clip(prev, 0, 1)
    inj = np.maximum(f32(p[0]) + f32(1) - jac, 0)
    return np.clip(f + dt * (f32(p[1]) * inj * (f32(1) - f) - f32(p[2]) * f), 0, 1).astype(f32)


def shift(a, dx, dy):     # value at (x + dx, y + dy), wrapping
    return np.roll(np.roll(a, -dy, axis=0), -dx, axis=1)


def surface(disp, c, half, wide, invw):
    d = lambda k, dx, dy: (shift(disp[k], dx, dy) - shift(disp[k], -dx, -dy)) * half
    dXx, dYx, dUx = d(0, 1, 0), d(1, 1, 0), d(2, 1, 0)
    dXy, dYy, dUy = d(0, 0, 1), d(1, 0, 1), d(2, 0, 1)
    wx = (shift(disp[2], wide, 0) - shift(disp[2], -wide, 0)) * invw
    wy = (shift(disp[2], 0, wide) - shift(disp[2], 0, -wide)) * invw
    jac = (f32(1) + dXx) * (f32(1) + dYy) - dXy * dYx
    return dUx, dUy, wx * wx + wy * wy, jac


def byte(v):
    return np.floor(np.clip(v, 0, 1) * f32(255) + f32(0.5))


def box(a):
    return f32(0.25) * (a[0::2, 0::2] + a[0::2, 1::2] + a[1::2, 0::2] + a[1::2, 1::2])


class Cpu:
    def __init__(s, c):
        s.c, n = c, c["n"]
        s.foam = np.zeros((n, n), f32)
        s.oxy = np.zeros((n, n), f32)
        s.last = -1.0

    def step(s, seconds):
        c, n = s.c, s.c["n"]
        lf = (seconds / LOOP) % 1.0
        ph = TWO_PI_D * ((c["freq"].astype(np.float64) * lf) % 1.0)
        rot = (np.cos(ph) + 1j * np.sin(ph)).astype(np.complex64)
        away = (c["h0a"] + 1j * c["h0t"].imag).astype(np.complex64)
        h = c["h0t"] * rot + np.conj(away) * np.conj(rot)
        hm = np.roll(np.roll(h[::-1, ::-1], 1, 0), 1, 1)        # height[(n - y) % n, (n - x) % n]
        kx = c["kx"]
        KX, KY = kx[None, :].repeat(n, 0), kx[:, None].repeat(n, 1)
        div = np.maximum(np.sqrt(KX.astype(np.float64) ** 2 + KY.astype(np.float64) ** 2), 1e-16).astype(f32)
        ax, ay = KX / div, KY / div
        bx, by = np.roll(np.roll(ax[::-1, ::-1], 1, 0), 1, 1), np.roll(np.roll(ay[::-1, ::-1], 1, 0), 1, 1)
        fh = 0.5 * (h + np.conj(hm))
        fx = 0.5 * (-1j * ax * h + np.conj(-1j * bx * hm))
        fy = 0.5 * (-1j * ay * h + np.conj(-1j * by * hm))
        hx = transform2d((fh + 1j * fx).astype(np.complex64))
        al = transform2d(fy.astype(np.complex64))
        y, x = np.mgrid[0:n, 0:n]
        sc = (np.where((x + y) & 1, -1.0, 1.0) / 65536.0).astype(f32)
        disp = [(sc * hx.imag).astype(f32), (sc * al.real).astype(f32), (-sc * hx.real).astype(f32)]
        half, wide, invw = stencil_consts(c)
        dt = f32(0.0 if s.last < 0 else min(max(seconds - s.last, 0.0), MAX_DT))
        s.last = seconds
        dUx, dUy, w2, jac = surface(disp, c, half, wide, invw)
        s.foam = foam_step(s.foam, jac, c["foam"], dt)
        s.oxy = foam_step(s.oxy, jac, c["oxy"], dt)
        mo = [dUx, dUy, dUx * dUx + dUy * dUy, w2]
        slopes, state = [], []
        while True:
            var = np.maximum(mo[2] - (mo[0] * mo[0] + mo[1] * mo[1]), 0)
            slopes.append(np.stack([mo[0].astype(f16), mo[1].astype(f16)]).astype(f32))
            lvl0 = not state
            st = [byte(np.sqrt(var / f32(VAR_RANGE))), byte(np.sqrt(mo[3] / f32(WIDE_RANGE))),
                  byte(s.foam if lvl0 else 0 * var), byte(s.oxy if lvl0 else 0 * var)]
            state.append(np.stack(st))
            if mo[0].shape[0] == 1:
                return slopes, state
            mo = [box(m) for m in mo]


def transform_lines(g):
    """CPU TransformLine on every row of g (complex64): bit reversal, then float radix-2 stages, e^{+2 pi i k / n}."""
    n = g.shape[1]
    g = g[:, rev_bits(np.arange(n), n)].copy()
    tw = np.array([complex(math.cos(6.283185307179586 * k / n), math.sin(6.283185307179586 * k / n)) for k in range(n // 2)],
                  np.complex64)
    length = 2
    while length <= n:
        h = length // 2
        blocks = g.reshape(g.shape[0], n // length, length)
        a = blocks[:, :, :h].copy()
        b = blocks[:, :, h:] * tw[np.arange(h) * (n // length)]
        blocks[:, :, :h] = a + b
        blocks[:, :, h:] = a - b
        g = blocks.reshape(g.shape[0], n)
        length *= 2
    return g


def transform2d(g):
    return transform_lines(transform_lines(g).T.copy()).T.copy()


def rev_bits(i, n):
    r = np.zeros_like(i)
    for b in range(int(math.log2(n))):
        r = r * 2 + ((i >> b) & 1)
    return r


def h16(a):
    return np.asarray(a, f32).astype(f16).astype(f32)


def split16(v):
    """An FFT texel holds one complex value as (re hi, im hi, re lo, im lo), vw_wave_common.hlsli SplitSigned:
    coa's SplitForHalfStorage on |v| (hi = 9 bits, exact in half) with v's sign; read back as hi + lo."""
    v = np.asarray(v, f32)
    a = np.abs(v)
    step = np.exp2(np.floor(np.log2(np.maximum(a, f32(6.10351563e-5))))) / f32(512)
    coarse = np.floor(a / step) * step
    res = np.minimum(a - coarse, step * f32(0.99951171875))
    sg = np.sign(v)
    return h16(sg * coarse) + h16(sg * res)


def cmul(ar, ai, br, bi):
    return ar * br - ai * bi, ar * bi + ai * br


class Gpu:
    def __init__(s, c):
        s.c, n = c, c["n"]
        # WaveSpectrum: prescale n^2/65536 * 2^-e (the 1/2 per butterfly stage gives the rest), e keeps |texel| <= 16384.
        peak = max(np.abs(c["h0t"].real).max(), np.abs(c["h0t"].imag).max(), np.abs(c["h0a"]).max()) * (n * n / 65536.0)
        s.e = max(0, math.ceil(math.log2(peak / HALF_CEILING))) if peak > 0 else 0
        S = f32(n * n / 65536.0 / 2.0 ** s.e)
        # two textures: coarse (exact in half) + residual, as the FFT texels (one rounding there moved stiff foam 12 units)
        s.h0 = [split16(c["h0t"].real * S), split16(c["h0t"].imag * S), split16(c["h0a"] * S), h16(c["freq"] % 2048)]
        s.foam = [np.zeros((n, n), f32) for _ in range(4)]      # SplitForHalfStorage: foam, oxygen, residuals
        s.last = -1.0

    def evolve(s, lf):
        c, n = s.c, s.c["n"]
        q = np.arange(n)
        xs, ys = rev_bits(q, n), rev_bits(q, n)
        X, Y = xs[None, :].repeat(n, 0), ys[:, None].repeat(n, 1)
        MX, MY = (n - X) % n, (n - Y) % n

        def mode(X, Y):
            tr, ti, ar, low = (h[Y, X] for h in s.h0)
            cx, cy = (X - n // 2).astype(f32), (Y - n // 2).astype(f32)
            r2 = cx * cx + cy * cy
            # the CPU's frequency mod 2048 (h0.w, exact in half) + the multiple of 2048 nearest the shader's estimate
            # (GPU sqrt may be off by an ulp or two; the estimate only has to be within 1024)
            estimate = np.sqrt(np.sqrt(r2)) * c["lprr"] + f32(0.5) + f32(0.25)   # +0.25: deliberately off
            freq = low + f32(2048) * np.floor((estimate - low) / f32(2048) + f32(0.5))
            f = (freq * lf) % f32(1)
            f = np.where(f >= f32(0.5), f - f32(1), f)
            ph = f32(TWO_PI_D) * f
            cr, sn = np.cos(ph), np.sin(ph)
            # towards * rot + conj(away) * conj(rot), away = (awayReal, towards.imag)
            hr = tr * cr - ti * sn + ar * cr - ti * sn
            hi = tr * sn + ti * cr - ar * sn - ti * cr
            l = np.sqrt(r2)
            l = np.where(l > 0, l, f32(1))
            return hr, hi, cx / l, cy / l

        hr, hi, ax, ay = mode(X, Y)
        mr, mi, bx, by = mode(MX, MY)
        # fold: 0.5 (h + conj hm); X: 0.5 (-i ax h + conj(-i bx hm)); Y likewise
        fhr, fhi = f32(0.5) * (hr + mr), f32(0.5) * (hi - mi)
        fxr, fxi = f32(0.5) * (ax * hi + bx * mi), f32(0.5) * (-ax * hr + bx * mr)
        fyr, fyi = f32(0.5) * (ay * hi + by * mi), f32(0.5) * (-ay * hr + by * mr)
        return [split16(fhr - fxi), split16(fhi + fxr), split16(fyr), split16(fyi)]

    def butterflies(s, t, axis):
        n = s.c["n"]
        i = np.arange(n)
        length = 2
        while length <= n:
            h = length // 2
            p = i % length
            k = p % h
            a_i, b_i = i - p + k, i - p + k + h
            f = (p / length).astype(f32)
            f = np.where(f >= f32(0.5), f - f32(1), f)
            wr, wi = np.cos(f32(TWO_PI_D) * f), np.sin(f32(TWO_PI_D) * f)
            if axis == 1:
                wr, wi = wr[None, :], wi[None, :]
                A, B = [c[:, a_i] for c in t], [c[:, b_i] for c in t]
            else:
                wr, wi = wr[:, None], wi[:, None]
                A, B = [c[a_i, :] for c in t], [c[b_i, :] for c in t]
            out = []
            for j in (0, 2):
                br, bi = cmul(wr, wi, B[j], B[j + 1])
                out += [split16((A[j] + br) * f32(0.5)), split16((A[j + 1] + bi) * f32(0.5))]
            t = out
            length *= 2
        return t

    def step(s, seconds):
        c, n = s.c, s.c["n"]
        t = s.evolve(f32((seconds / LOOP) % 1.0))
        t = s.butterflies(s.butterflies(t, 1), 0)
        y, x = np.mgrid[0:n, 0:n]
        sc = (np.where((x + y) & 1, -1.0, 1.0) * 2.0 ** s.e).astype(f32)
        disp = [sc * t[1], sc * t[2], -sc * t[0]]
        half, wide, invw = stencil_consts(c)
        dt = f32(0.0 if s.last < 0 else min(max(seconds - s.last, 0.0), MAX_DT))
        s.last = seconds
        dUx, dUy, w2, jac = surface(disp, c, half, wide, invw)
        prev = [np.clip(s.foam[0] + s.foam[2], 0, 1), np.clip(s.foam[1] + s.foam[3], 0, 1)]
        nf = [foam_step(prev[0], jac, c["foam"], dt), foam_step(prev[1], jac, c["oxy"], dt)]
        s.foam = split_for_half(nf)
        foam = [np.clip(s.foam[0] + s.foam[2], 0, 1), np.clip(s.foam[1] + s.foam[3], 0, 1)]
        mo = [h16(dUx), h16(dUy), np.zeros_like(dUx), h16(w2)]      # mean slope, central variance, wide^2
        slopes, state = [], []
        while True:
            slopes.append(np.stack([h16(mo[0]), h16(mo[1])]))
            lvl0 = not state
            st = [np.round(np.clip(np.sqrt(mo[2] / f32(VAR_RANGE)), 0, 1) * 255), np.round(np.clip(np.sqrt(mo[3] / f32(WIDE_RANGE)), 0, 1) * 255),
                  np.round((foam[0] if lvl0 else 0 * mo[0]) * 255), np.round((foam[1] if lvl0 else 0 * mo[0]) * 255)]
            state.append(np.stack(st))
            if mo[0].shape[0] == 1:
                return slopes, state
            m = [box(mo[0]), box(mo[1])]
            up = [np.repeat(np.repeat(v, 2, 0), 2, 1) for v in m]
            var = box(mo[2] + (mo[0] - up[0]) ** 2 + (mo[1] - up[1]) ** 2)
            mo = [h16(m[0]), h16(m[1]), h16(var), h16(box(mo[3]))]


def split_for_half(states):
    out = [None] * 4
    for j, sv in enumerate(states):
        step = np.exp2(np.floor(np.log2(np.maximum(sv, f32(6.10351563e-5))))) / f32(512)
        coarse = np.floor(sv / step) * step
        out[j] = h16(coarse)
        out[j + 2] = h16(np.minimum(sv - coarse, step * f32(0.99951171875)))
    return out


def main():
    worst = dict(slope=0.0, state=0.0)
    ok = True
    print(f"{'tile':>4} {'n':>3} {'set':>6} {'e':>2} {'slope max':>9} {'slope rms':>9} | state max/rms (8-bit units) R G B A | flicker")
    for setting, (wind, waves) in {"dflt": (2.0, 1.0), "max": (10.0, 2.0)}.items():
        for tile in tiles():
            for n in (128, 256):
                c = configure(tile, n, wind, waves * waves)
                cpu, gpu = Cpu(c), Gpu(c)
                history = []
                sm, ss, sn = 0.0, 0.0, 0
                stmax, stsq, stn = np.zeros(4), np.zeros(4), 0
                flicker = 0     # foam/oxygen texel-steps off by > 3 units where the texel flickers
                for k in range(12):
                    secs = 3611.37 + 0.07 * k       # 70 ms steps: foam builds; a late loop phase
                    cs, cst = cpu.step(secs)
                    gs, gst = gpu.step(secs)
                    for a, b in zip(cs, gs):
                        d = np.abs(a - b)
                        sm, ss, sn = max(sm, float(d.max())), ss + float((d * d).sum()), sn + d.size
                    for level, (a, b) in enumerate(zip(cst, gst)):
                        d = np.abs(a - b).reshape(4, -1)
                        if level == 0:
                            # dt * rate > 2 (oxygen 200/s at 70 ms) makes FoamStep a 2-cycle at saturation (1 <-> 1 - decay
                            # dt): a texel there flickers on either side (> 3 units in one of the last two steps) and any
                            # rounding picks the other phase.
                            history = (history + [(a, b)])[-3:]
                            own = np.max([np.maximum(np.abs(x[0] - y[0]), np.abs(x[1] - y[1]))
                                          for x, y in zip(history, history[1:])] or [0 * a], axis=0)
                            osc = (d[2:] > 3) & (own.reshape(4, -1)[2:] > 3)
                            flicker += int(osc.sum())
                            d[2:][osc] = 0
                        stmax = np.maximum(stmax, d.max(1))
                        stsq += (d * d).sum(1)
                        stn += d.shape[1]
                srms, strms = math.sqrt(ss / sn), np.sqrt(stsq / stn)
                print(f"{tile[0]:>4} {n:>3} {setting:>6} {gpu.e:>2} {sm:9.2e} {srms:9.2e} | "
                      + " ".join(f"{m:.0f}/{r:.3f}" for m, r in zip(stmax, strms)) + f" | {flicker}")
                worst["slope"] = max(worst["slope"], sm)
                worst["state"] = max(worst["state"], float(strms.max()))
                # thresholds: slopes within 2e-3 (1 half ulp at |slope| 1..2), R/G at most 1 unit off (an 8-bit rounding
                # flip), every channel RMS < 0.2 unit; foam/oxygen at most 4 units off except flickering texels (counted,
                # at most 0.01 % of the texel-steps).
                if sm > 2e-3 or stmax[:2].max() > 1 or strms.max() > 0.2 or stmax[2:].max() > 4 or flicker > 1e-4 * 12 * n * n:
                    ok = False
    print(f"worst slope max {worst['slope']:.2e}, worst state rms {worst['state']:.3f} units")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
