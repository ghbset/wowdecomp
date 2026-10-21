"""G-buffer normals (gxNormalBuffer): rewrites a stock vs_3_0 / ps_3_0 token stream so the world's
shaders write their interpolated view-space normal to a second render target. GBUF-STATUS.md.

Vertex: every vs_3_0 record gains `dcl_texcoord7 oN` (N = first free output). A record with a
`dcl_normal` input writes the transformed normal there, captured just before the first dp3 that
dots a temp derived from the normal (all of .xyz) with something that is not (the lighting or env
dot: M2 `dp3 r0.w, r0, r2`, WMO `dp3_sat r0.x, -c12, r1`, terrain `dp3_sat r1.x, c24, r1`), .w = 1.
Every stock family transforms it into view space there (M2 bone rows, WMO c31-33, terrain c0-c2),
the space LegionLights' positions are in. No normal or no such dp3: oN = 0.

Pixel: ps_3_0 records gain `dcl_texcoord7 vN` and `oC1 = float4(normalize(vN.xyz) * 0.5 + 0.5, vN.w)`
before `end`; alpha 0 = no normal here (the passes then rebuild it from the depth).

Used by translate.py (Vulkan, before the GLSL emit, the md5 stays the stock record's) and by
make_pack (D3D9 / D3D11: rewritten .bls containers). Only SM3: SM2 records are returned unchanged.

    python3 -I normals.py stats <dir of .bls>   # per record: captured / zero / refused
    python3 -I normals.py pack <in dir> <out dir>   # rewrite vs_3_0 / ps_3_0 .bls files (same names)
"""
import os
import struct
import sys

if __package__ in (None, ''):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from bls import dx9ir as ir
else:
    from . import dx9ir as ir

TEXCOORD, NORMAL = 5, 3
USAGE_INDEX = 7             # TEXCOORD7: unused by every stock vs_3_0 / ps_3_0 record
OP_MOV, OP_MAD, OP_DP3, OP_NRM, OP_DCL, OP_DEF = 1, 4, 8, 36, 31, 81

# Pixel families that draw opaque world surfaces (blended draws still carry oC1; the device masks
# RT1 off for them). Everything else (UI, liquids, shadow maps, desaturate) is left alone.
PS_FAMILIES = ('combiners_', 'mapobj', 'terrain', 'detaildoodad')
VS_SKIP = ('ui', 'shadowmap', 'vsliquid', 'waterripples')


def _rt_bits(rt):
    return ((rt & 7) << 28) | ((rt & 0x18) << 8)


def dst(rt, idx, mask=0xF):
    return 0x80000000 | _rt_bits(rt) | idx | (mask << 16)


def src(rt, idx, swz=0xE4, mod=0):
    return 0x80000000 | _rt_bits(rt) | idx | (swz << 16) | (mod << 24)


def inst(op, *params):
    return [op | (len(params) << 24)] + list(params)


def dcl(usage, index, dtok):
    return inst(OP_DCL, 0x80000000 | usage | (index << 16), dtok)


def define(idx, values):
    return inst(OP_DEF, dst(ir.RT_CONST, idx), *struct.unpack('<4I', struct.pack('<4f', *values)))


class Refused(Exception):
    pass


def _free_const(sh, limit):
    """A float constant the record neither reads nor defines, below any relative-addressed base."""
    used, low = set(), limit
    for ins in sh.insts:
        for o in ([ins.dst] if ins.dst else []) + ins.srcs:
            if o.rtype == ir.RT_CONST:
                if o.rel:
                    low = min(low, o.index)
                else:
                    # m4x4 & co read consecutive rows
                    rows = {20: 4, 21: 3, 22: 4, 23: 3, 24: 2}.get(ins.op, 1) if o is not ins.dst else 1
                    used.update(range(o.index, o.index + rows))
    for c in range(low):
        if c not in used:
            return c
    raise Refused('no free constant')


def _max_index(sh, rtype):
    m = -1
    for ins in sh.insts:
        for o in ([ins.dst] if ins.dst else []) + ins.srcs:
            if o.rtype == rtype:
                m = max(m, o.index)
    return m


def _header_end(sh):
    """Token offset after the last dcl/def (where new declarations go)."""
    pos = 1
    for ins in sh.insts:
        if ins.op in (OP_DCL, OP_DEF, 47, 48):
            pos = ins.pos + 1 + len(_inst_tokens(ins))
        else:
            break
    return pos


_TOKS = None


def _inst_tokens(ins):
    return _TOKS[ins.pos + 1: ins.pos + 1 + ((_TOKS[ins.pos] >> 24) & 0xF)]


def _vs_capture(sh):
    """(instruction position, temp index, swizzle) of the normal just before its first lighting dot."""
    normal = [ins.dst.index for ins in sh.insts if ins.op == OP_DCL and ins.dst.rtype == ir.RT_INPUT
              and ins.dcl_usage == (NORMAL, 0)]
    if not normal:
        return None
    taint = {}   # (rtype, index) -> set of tainted components
    taint[(ir.RT_INPUT, normal[0])] = {0, 1, 2, 3}

    def tainted(o, comps=range(4)):
        t = taint.get((o.rtype, o.index), ())
        return any(o.comps()[c] in t for c in comps)

    for ins in sh.insts:
        if ins.op in (OP_DCL, OP_DEF, 47, 48) or ins.dst is None:
            continue
        if ins.op == OP_DP3 and len(ins.srcs) == 2:
            for a, b in ((0, 1), (1, 0)):
                s, other = ins.srcs[a], ins.srcs[b]
                t = taint.get((s.rtype, s.index), ())
                if s.rtype == ir.RT_TEMP and not s.rel and all(s.comps()[c] in t for c in range(3)) \
                        and not tainted(other):
                    return ins.pos, s.index, s.swizzle
        hit = any(tainted(s) for s in ins.srcs)
        if hit and ins.dst.rtype == ir.RT_OUTPUT and ins.dst.mask & 7 == 7 and not ins.predicated:
            return ins.pos, None, None      # the record passes the normal on itself (shadowed perms)
        key = (ins.dst.rtype, ins.dst.index)
        comps = {c for c in range(4) if ins.dst.mask & (1 << c)}
        cur = set(taint.get(key, ()))
        cur = cur | comps if hit else cur - comps
        taint[key] = cur
    return None


def inject(code, kind_hint=None):
    """The rewritten token stream, or None when the record is left alone (SM2, or refused)."""
    global _TOKS
    sh = ir.decode(code)
    if sh.major != 3:
        return None
    _TOKS = struct.unpack('<%dI' % (len(code) // 4), code)
    toks = list(_TOKS)
    end = next(i for i in range(len(toks) - 1, 0, -1) if toks[i] == 0x0000FFFF)
    head = _header_end(sh)
    if sh.kind == 'vs':
        o = _max_index(sh, ir.RT_OUTPUT) + 1
        if o > 11:
            raise Refused('no free output')
        c = _free_const(sh, 256)
        decl = dcl(TEXCOORD, USAGE_INDEX, dst(ir.RT_OUTPUT, o)) + define(c, (0.0, 1.0, 0.0, 0.0))
        cap = _vs_capture(sh)
        if cap:
            pos, r, swz = cap
            if r is None:   # the same instruction again, into oN.xyz, right after the original
                n = (toks[pos] >> 24) & 0xF
                again = [toks[pos], dst(ir.RT_OUTPUT, o, 0x7) | (toks[pos + 1] & 0x0FF00000)] + toks[pos + 2: pos + 1 + n]
                pos += 1 + n
                body = again + inst(OP_MOV, dst(ir.RT_OUTPUT, o, 0x8), src(ir.RT_CONST, c, 0x55))
            else:
                body = inst(OP_MOV, dst(ir.RT_OUTPUT, o, 0x7), src(ir.RT_TEMP, r, swz)) + \
                    inst(OP_MOV, dst(ir.RT_OUTPUT, o, 0x8), src(ir.RT_CONST, c, 0x55))   # .yyyy = 1
        else:
            pos = end
            body = inst(OP_MOV, dst(ir.RT_OUTPUT, o), src(ir.RT_CONST, c, 0x00))     # .xxxx = 0
        out = toks[:head] + decl + toks[head:pos] + body + toks[pos:]
        return struct.pack('<%dI' % len(out), *out), bool(cap)
    v = _max_index(sh, ir.RT_INPUT) + 1
    r = _max_index(sh, ir.RT_TEMP) + 1
    if v > 9 or r > 31:
        raise Refused('no free input/temp')
    if _max_index(sh, ir.RT_COLOROUT) > 0:
        raise Refused('already MRT')
    c = _free_const(sh, 224)
    decl = dcl(TEXCOORD, USAGE_INDEX, dst(ir.RT_INPUT, v)) + define(c, (0.5, 0.5, 0.5, 0.5))
    body = inst(OP_NRM, dst(ir.RT_TEMP, r, 0x7), src(ir.RT_INPUT, v)) + \
        inst(OP_MAD, dst(ir.RT_TEMP, r, 0x7), src(ir.RT_TEMP, r), src(ir.RT_CONST, c), src(ir.RT_CONST, c)) + \
        inst(OP_MOV, dst(ir.RT_TEMP, r, 0x8), src(ir.RT_INPUT, v)) + \
        inst(OP_MOV, dst(ir.RT_COLOROUT, 1), src(ir.RT_TEMP, r))
    out = toks[:head] + decl + toks[head:end] + body + toks[end:]
    return struct.pack('<%dI' % len(out), *out), True


def wanted(name, kind):
    """Does this engine shader name (e.g. 'Combiners_Opaque', 'Terrain') get a normal?"""
    n = os.path.basename(name).lower()
    if kind == 'ps':
        return n.startswith(PS_FAMILIES)
    return not n.startswith(VS_SKIP)


def rewrite_record(name, kind, code):
    """translate.py's hook: the record's code with the normal plumbing, or the code unchanged."""
    if not wanted(name, kind):
        return code
    try:
        r = inject(code)
    except Refused:
        return code
    return r[0] if r else code


def _container(data, fn):
    from bls import archive     # here: archive imports this module
    magic, ver, nperm, recs, _ = archive.split(data)
    out = bytearray(struct.pack('<III', magic, ver, nperm))
    for rec in recs:
        code = fn(rec['code']) if rec['size'] else rec['code']
        out += struct.pack('<IIHHI', rec['a'], rec['b'], rec['c'], rec['d'], len(code)) + code
        out += b'\0' * ((-len(out)) % 4)
    return bytes(out)


def main(argv):
    mode, root = argv[0], argv[1]
    stats = {}
    for dirpath, _, files in os.walk(root):
        for f in sorted(files):
            if not f.endswith('.bls'):
                continue
            prof = os.path.basename(dirpath)
            if prof not in ('vs_3_0', 'ps_3_0'):
                continue
            kind = prof[:2]
            name = f[:-4]
            if not wanted(name, kind):
                continue
            path = os.path.join(dirpath, f)
            data = open(path, 'rb').read()

            def fn(code, name=name):
                try:
                    r = inject(code)
                except Refused as e:
                    stats.setdefault(name, []).append('refused:' + str(e))
                    return code
                stats.setdefault(name, []).append('normal' if r[1] else 'zero')
                ir.decode(r[0])     # must decode back
                return r[0]
            new = _container(data, fn)
            if mode == 'pack':
                dst_dir = os.path.join(argv[2], os.path.relpath(dirpath, root))
                os.makedirs(dst_dir, exist_ok=True)
                open(os.path.join(dst_dir, f), 'wb').write(new)
    for name, s in sorted(stats.items()):
        print('%-32s %s' % (name, ' '.join('%s=%d' % (k, s.count(k)) for k in sorted(set(s)))))


if __name__ == '__main__':
    main(sys.argv[1:])
