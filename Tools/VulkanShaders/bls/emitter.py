"""One DX9 record -> one GLSL 4.50 module, plus the metadata the manifest carries.

The interface contract every module shares (the device's pipelines are built against it):

  Vertex inputs   fixed attribute locations by semantic (VIN_LOCATION); the pipeline maps the
                  engine's vertex layout onto them. An input semantic outside the table is refused.
  Varyings        locations by semantic (VARYING_LOCATION), vec4 everywhere except fog (float);
                  position and point size are the built-ins. Every component of a written varying
                  is written (0 where the bytecode writes none), so any vertex module pairs with any
                  pixel module whose reads it covers.
  Constants       one std140 block, set 1 binding 0: { vec4 p[16]; vec4 misc; vec4 v[VCONST]; }.
                  p[] and misc come first so a pixel module, which never reads v[], can declare v[64]
                  whatever class the paired vertex module is. VCONST is 64 or 256 per the vertex
                  record's highest constant (manifest `vconst=`). A pixel record reading c16 or above
                  (only our own custom.MPQ shaders do) has PCONST rows (manifest `pconst=`, a multiple
                  of 4, at most 256) and declares { p[16]; misc; v[256]; px[PCONST - 16] }: c16.. go
                  AFTER the whole 256-row vertex window, so the vertex module's v[] offsets never move
                  and any vertex module still pairs with it. misc.x is the alpha test reference
                  (alphaRef / 255, 0 = off), misc.z / misc.w the stage-0 colour / alpha op (read by
                  the generated companion stages only).
  Samplers        a combined image sampler per D3D sampler slot, set 0 binding <slot>.
  Clip space      gl_Position.y is negated in every vertex module (D3D's +y up, Vulkan's +y down);
                  depth is already in [0, 1] (the device's projection is D3D's).

A record that uses something no rule covers is refused: logged, listed as a comment at the top of the
manifest, nothing emitted. The four vs_2_0 weather records (patter, rain, sand, snowpoint) write the
fixed-function outputs oFog / oPts and are drawn with no pixel shader; their modules carry the
epilogues (fog0, gl_PointSize) and the flags fixed_fog / point_size, and for each of them a
companion pixel module -- the fixed-function stage D3D runs after them -- is generated
(companion_module).
"""
import collections
import math

from . import dx9ir as ir

# ---- the interface contract -------------------------------------------------------------------
VIN_LOCATION = {'position0': 0, 'normal0': 1, 'color0': 2, 'color1': 3, 'texcoord0': 4,
                'texcoord1': 5, 'blendweight0': 6, 'blendindices0': 7}
VARYING_LOCATION = {'color0': 0, 'color1': 1, 'fog0': 10}
for _i in range(8):
    VARYING_LOCATION['texcoord%d' % _i] = 2 + _i
VARYING_TYPE = {'fog0': 'float'}
PCONST = 16              # pixel rows every stock record has; also the p[] array size
PCONST_MAX = 256         # rows a pixel record may have (16 in p[], the rest in px[])
VCONST_SMALL, VCONST_LARGE = 64, 256

# vs_2_0 fixed-function output registers -> semantic
VS2_OUT = {'oPos': 'position0', 'oD0': 'color0', 'oD1': 'color1', 'oFog': 'fog0', 'oPts': 'psize0'}
for _i in range(8):
    VS2_OUT['oT%d' % _i] = 'texcoord%d' % _i

# The pixel constant row the device fills with the fixed-function fog colour for the companion
# stages: (fogColor.rgb / 255, fogOn ? 1 : 0). No shipped pixel record reads c13..c15.
FOG_ROW = 15

SWZ = ir.SWZ


class Refused(Exception):
    pass


def fmt_float(x):
    if math.isnan(x) or math.isinf(x):
        raise Refused('non-finite def constant %r' % x)
    s = '%.9g' % x
    if 'e' in s or 'E' in s:
        m, e = s.split('e')
        if '.' not in m:
            m += '.0'
        return m + 'e' + e
    if '.' not in s:
        s += '.0'
    return s


def mask_letters(mask):
    return ''.join(SWZ[k] for k in range(4) if mask & (1 << k))


class Emitter(object):
    """One record -> one GLSL module plus the metadata the manifest carries.

    The body is one pass over the Inst list. Registers are locals (`vec4 r0`, `vec4 o1`), the
    interface is by semantic (`color0`, `texcoord0`, `fog0`), and a prologue / epilogue connects
    the two: the y flip, the zero-fill of unwritten varying components, the vs_2_0 fixed outputs
    and the alpha-test discard."""

    def __init__(self, rec):
        self.rec = rec
        self.sh = rec.sh
        self.kind = rec.kind                       # 'vertex' | 'pixel'
        self.vs = self.sh.kind == 'vs'
        self.major = self.sh.major
        self.defs = {}                             # c index -> (x, y, z, w)
        self.defi = {}                             # i index -> ints
        self.inputs = collections.OrderedDict()    # register name -> semantic (vertex) / pixel reads
        self.in_masks = {}
        self.out_sems = collections.OrderedDict()  # output register name -> [semantic]
        self.samplers = {}                         # slot -> '2d' | 'cube'
        self.temps = set()
        self.uses_odepth = False
        self.uses_vpos = False       # dcl vPos (our custom.MPQ shaders only) -> gl_FragCoord
        self.outregs = collections.OrderedDict()
        self.uses_a0 = False
        self.uses_aL = False
        self.relative = False
        self.cmax = -1
        self.flags = set()
        self.helpers = set()
        self.body = []
        self.depth = 1
        self.loopn = 0
        self.ops_used = collections.Counter()

    # -- operands ---------------------------------------------------------------------------------

    def regname(self, o):
        return o.name(self.sh.kind, self.major)

    def const_expr(self, o):
        """A `c#` read: a def'd literal or the uniform window, direct or relative."""
        if o.rel:
            rt, ri, comp = o.rel
            if not self.vs:
                raise Refused('relative addressing in a pixel shader')
            self.relative = True
            if rt == ir.RT_LOOP:
                self.uses_aL = True
                idx = '%d + aL' % o.index
            elif rt == ir.RT_ADDR:
                self.uses_a0 = True
                idx = '%d + a0.%s' % (o.index, SWZ[comp])
            else:
                raise Refused('relative addressing through register type %d' % rt)
            return 'regs.v[%s]' % idx
        if o.index in self.defs:
            return 'c%d' % o.index
        self.cmax = max(self.cmax, o.index)
        if self.vs:
            if o.index >= VCONST_LARGE:
                raise Refused('vertex constant c%d beyond the 256-row window' % o.index)
            return 'regs.v[%d]' % o.index
        if o.index >= PCONST_MAX:
            raise Refused('pixel constant c%d beyond the %d-row window' % (o.index, PCONST_MAX))
        if o.index >= PCONST:
            return 'regs.px[%d]' % (o.index - PCONST)
        return 'regs.p[%d]' % o.index

    def base_expr(self, o):
        rt = o.rtype
        if rt == ir.RT_TEMP:
            self.temps.add(o.index)
            return 'r%d' % o.index
        if rt == ir.RT_CONST:
            return self.const_expr(o)
        if rt == ir.RT_INPUT:
            nm = self.regname(o)
            if nm not in self.inputs:
                raise Refused('%s read without a dcl' % nm)
            return nm
        if rt == ir.RT_TEXTURE and not self.vs:          # ps_2_0 t#
            nm = self.regname(o)
            if nm not in self.inputs:
                raise Refused('%s read without a dcl' % nm)
            return nm
        if rt == ir.RT_CONSTINT:
            if o.index not in self.defi:
                raise Refused('i%d read but not defi\'d (the engine uploads no integer constants)' % o.index)
            return 'i%d' % o.index
        if rt == ir.RT_CONSTBOOL:
            raise Refused('bool constant b%d' % o.index)
        if rt == ir.RT_MISCTYPE and o.index == 0 and self.uses_vpos:
            return 'vPos'
        if rt == ir.RT_MISCTYPE:
            raise Refused('%s is not supported' % self.regname(o))
        if rt == ir.RT_PREDICATE:
            raise Refused('predicate register')
        if rt in (ir.RT_ADDR, ir.RT_LOOP) and self.vs:
            raise Refused('%s read as a plain source' % self.regname(o))
        if rt == ir.RT_SAMPLER:
            raise Refused('sampler used as an arithmetic source')
        if rt in (ir.RT_RASTOUT, ir.RT_ATTROUT, ir.RT_TEXCRDOUT, ir.RT_COLOROUT):
            nm = self.regname(o)
            if nm == 'oC0':
                return 'oC0'
            self.outregs[nm] = True
            return nm
        raise Refused('register type %d (%s)' % (rt, self.regname(o)))

    def src_sw(self, o, sel):
        """Source narrowed to the components `sel` (indices into the swizzled vec4): base, the
        composed swizzle, then the modifier outside. sel=[0,1,2,3] is the whole source."""
        if o.rel and o.rtype != ir.RT_CONST:
            raise Refused('relative addressing on a non-constant register')
        base = self.base_expr(o)
        c = o.comps()
        letters = ''.join(SWZ[c[k]] for k in sel)
        e = base if letters == 'xyzw' else '%s.%s' % (base, letters)
        mod = o.mod
        if mod == 0:
            return e
        if mod == 1:
            return '-' + e
        if mod == 11:
            return 'abs(%s)' % e
        if mod == 12:
            return '-abs(%s)' % e
        if mod == 6:
            return '(1.0 - %s)' % e
        raise Refused('source modifier %s' % ir.SRCMOD.get(mod, ('', '', '?%d' % mod))[2])

    def src(self, o):
        return self.src_sw(o, [0, 1, 2, 3])

    def comp(self, o, k):
        return self.src_sw(o, [k])

    def srcm(self, o, d):
        """The source narrowed to the destination's write mask (componentwise ops)."""
        return self.src_sw(o, [k for k in range(4) if d.mask & (1 << k)])

    def sw(self, expr, letters):
        """`expr.letters`, parenthesised when expr is not a plain name."""
        plain = all(ch.isalnum() or ch in '_.[]' for ch in expr)
        call = expr[:1].isalpha() and expr.endswith(')') and expr.count('(') == expr.count(')')
        if (plain or call) and not expr.startswith('-'):
            return '%s.%s' % (expr, letters)
        return '(%s).%s' % (expr, letters)

    def dest_name(self, d):
        rt = d.rtype
        if d.rel:
            raise Refused('relative addressing on a destination')
        if rt == ir.RT_TEMP:
            self.temps.add(d.index)
            return 'r%d' % d.index
        if rt == ir.RT_ADDR and self.vs:
            self.uses_a0 = True
            return 'a0'
        if rt == ir.RT_COLOROUT:
            if d.index != 0:
                raise Refused('oC%d: multiple render targets' % d.index)
            return 'oC0'
        if rt == ir.RT_DEPTHOUT:
            self.uses_odepth = True     # -> gl_FragDepth (glslang adds DepthReplacing)
            return 'oDepth'
        if rt in (ir.RT_RASTOUT, ir.RT_ATTROUT, ir.RT_TEXCRDOUT) and self.vs:
            nm = self.regname(d)
            self.outregs[nm] = True
            return nm
        raise Refused('destination register type %d (%s)' % (rt, self.regname(d)))

    def assign(self, d, vec=None, scalar=None, full=None):
        """D.M = expr, with _sat applied before the mask. `vec` is already the mask's width
        (sources narrowed with srcm), `scalar` is replicated into it, `full` is a vec4 that gets
        `.M` applied."""
        if d.shift:
            raise Refused('destination shift modifier')
        name = self.dest_name(d)
        if name == 'a0':
            raise Refused('float assignment to a0')
        m = mask_letters(d.mask)
        n = len(m)
        if scalar is not None:
            if d.sat:
                scalar = 'clamp(%s, 0.0, 1.0)' % scalar
            rhs = scalar if n == 1 else 'vec%d(%s)' % (n, scalar)
        elif full is not None:
            rhs = full
            if d.sat:
                rhs = 'clamp(%s, 0.0, 1.0)' % rhs
            if n != 4:
                rhs = self.sw(rhs, m)
        else:
            rhs = vec
            if d.sat:
                rhs = 'clamp(%s, 0.0, 1.0)' % rhs
        if n == 4:
            self.line('%s = %s;' % (name, rhs))
        else:
            self.line('%s.%s = %s;' % (name, m, rhs))

    def line(self, s):
        self.body.append('    ' * self.depth + s)

    # -- declarations -----------------------------------------------------------------------------

    def dcl(self, ins):
        d = ins.dst
        if d.rtype == ir.RT_SAMPLER:
            t = ir.TEXTYPE.get(ins.dcl_textype, 'tt%d' % ins.dcl_textype)
            if t not in ('2d', 'cube'):
                raise Refused('sampler type %s' % t)
            if self.vs:
                raise Refused('vertex texture fetch (dcl_%s s%d)' % (t, d.index))
            if d.index > 15:
                raise Refused('sampler slot s%d' % d.index)
            self.samplers[d.index] = t
            return
        if d.rtype == ir.RT_MISCTYPE and d.index == 0 and not self.vs:
            self.uses_vpos = True
            return
        if d.centroid:
            raise Refused('_centroid declaration')
        u, ui = ins.dcl_usage
        sem = '%s%d' % (ir.USAGE.get(u, 'usage%d' % u), ui)
        nm = self.regname(d)
        if self.vs and d.rtype == ir.RT_OUTPUT and self.major >= 3:
            if sem != 'position0' and sem != 'psize0' and sem not in VARYING_LOCATION:
                raise Refused('output semantic %s' % sem)
            self.out_sems.setdefault(nm, []).append(sem)
            self.outregs[nm] = True
            return
        if d.rtype == ir.RT_INPUT and self.vs:
            if sem not in VIN_LOCATION:
                raise Refused('vertex input semantic %s' % sem)
            self.inputs[nm] = sem
            self.in_masks[nm] = d.mask
            return
        if not self.vs:
            if self.major < 3:
                # ps_2_0: no semantic token; t# is texcoord#, v# is color# (and is clamped)
                sem = ('texcoord%d' if d.rtype == ir.RT_TEXTURE else 'color%d') % d.index
            if sem not in VARYING_LOCATION:
                raise Refused('pixel input semantic %s' % sem)
            self.inputs[nm] = sem
            self.in_masks[nm] = d.mask
            return
        raise Refused('dcl of %s' % nm)

    # -- instructions -----------------------------------------------------------------------------

    def emit(self, ins):
        op = ins.op
        if ins.predicated or ins.pred is not None:
            raise Refused('predicated instruction')
        if op == ir.OP_DCL:
            return self.dcl(ins)
        if op == ir.OP_DEF:
            self.defs[ins.dst.index] = ins.values
            return
        if op == ir.OP_DEFI:
            self.defi[ins.dst.index] = ins.values
            return
        if op == ir.OP_DEFB:
            raise Refused('defb')
        mn = ins.mnemonic()
        self.ops_used[mn] += 1
        rule = RULES.get(op)
        if rule is None:
            raise Refused('no rule for opcode %s' % mn)
        rule(self, ins)

    # one rule per opcode; A, B, C are the sources after swizzle and modifier, D the destination
    def r_mov(self, i):
        self.assign(i.dst, vec=self.srcm(i.srcs[0], i.dst))

    def r_add(self, i):
        d = i.dst
        self.assign(d, vec='%s + %s' % (self.srcm(i.srcs[0], d), self.srcm(i.srcs[1], d)))

    def r_sub(self, i):
        d = i.dst
        self.assign(d, vec='%s - %s' % (self.srcm(i.srcs[0], d), self.srcm(i.srcs[1], d)))

    def r_mul(self, i):
        d = i.dst
        self.assign(d, vec='%s * %s' % (self.srcm(i.srcs[0], d), self.srcm(i.srcs[1], d)))

    def r_mad(self, i):
        d = i.dst
        self.assign(d, vec='%s * %s + %s' % tuple(self.srcm(s, d) for s in i.srcs[:3]))

    def r_dp3(self, i):
        a, b = self.src_sw(i.srcs[0], [0, 1, 2]), self.src_sw(i.srcs[1], [0, 1, 2])
        self.assign(i.dst, scalar='dot(%s, %s)' % (a, b))

    def r_dp4(self, i):
        self.assign(i.dst, scalar='dot(%s, %s)' % (self.src(i.srcs[0]), self.src(i.srcs[1])))

    def r_dp2add(self, i):
        a, b, c = i.srcs[:3]
        self.assign(i.dst, scalar='%s * %s + %s * %s + %s'
                    % (self.comp(a, 0), self.comp(b, 0), self.comp(a, 1), self.comp(b, 1), self.comp(c, 0)))

    def r_min(self, i):
        d = i.dst
        self.assign(d, vec='min(%s, %s)' % (self.srcm(i.srcs[0], d), self.srcm(i.srcs[1], d)))

    def r_max(self, i):
        d = i.dst
        self.assign(d, vec='max(%s, %s)' % (self.srcm(i.srcs[0], d), self.srcm(i.srcs[1], d)))

    def r_lrp(self, i):
        # lrp dst, s0, s1, s2 = s0 * s1 + (1 - s0) * s2 = mix(s2, s1, s0)
        d = i.dst
        a, b, c = (self.srcm(s, d) for s in i.srcs[:3])
        self.assign(d, vec='mix(%s, %s, %s)' % (c, b, a))

    def r_rcp(self, i):
        self.assign(i.dst, scalar='1.0 / %s' % self.comp(i.srcs[0], 0))

    def r_rsq(self, i):
        self.helpers.add('rsq')
        self.assign(i.dst, scalar='dx_rsq(%s)' % self.comp(i.srcs[0], 0))

    def r_pow(self, i):
        self.helpers.add('pow')
        self.assign(i.dst, scalar='dx_pow(%s, %s)' % (self.comp(i.srcs[0], 0), self.comp(i.srcs[1], 0)))

    def r_exp(self, i):
        self.assign(i.dst, scalar='exp2(%s)' % self.comp(i.srcs[0], 0))

    def r_log(self, i):
        self.assign(i.dst, scalar='log2(abs(%s))' % self.comp(i.srcs[0], 0))

    def r_nrm(self, i):
        self.helpers.add('nrm')
        self.assign(i.dst, full='dx_nrm(%s)' % self.src(i.srcs[0]))

    def r_abs(self, i):
        self.assign(i.dst, vec='abs(%s)' % self.srcm(i.srcs[0], i.dst))

    def r_frc(self, i):
        self.assign(i.dst, vec='fract(%s)' % self.srcm(i.srcs[0], i.dst))

    def r_compare(self, i, fn, op):
        d = i.dst
        a, b = self.srcm(i.srcs[0], d), self.srcm(i.srcs[1], d)
        n = bin(d.mask).count('1')
        if n == 1:
            self.assign(d, vec='float(%s %s %s)' % (a, op, b))
        else:
            self.assign(d, vec='vec%d(%s(%s, %s))' % (n, fn, a, b))

    def r_slt(self, i):
        self.r_compare(i, 'lessThan', '<')

    def r_sge(self, i):
        self.r_compare(i, 'greaterThanEqual', '>=')

    def r_sincos(self, i):
        # vs_3_0: one source; vs_2_0: two extra Taylor-coefficient constants the macro used
        s = self.comp(i.srcs[0], 0)
        m = i.dst.mask & 0xF
        if m & 0xC:
            raise Refused('sincos writing .z or .w')
        if m == 1:
            self.assign(i.dst, vec='cos(%s)' % s)
        elif m == 2:
            self.assign(i.dst, vec='sin(%s)' % s)
        else:
            self.assign(i.dst, vec='vec2(cos(%s), sin(%s))' % (s, s))

    def r_cmp(self, i):
        # per component: a >= 0 ? b : c
        d = i.dst
        a, b, c = (self.srcm(s, d) for s in i.srcs[:3])
        n = bin(d.mask).count('1')
        if n == 1:
            self.assign(d, vec='(%s >= 0.0 ? %s : %s)' % (a, b, c))
        else:
            self.assign(d, vec='mix(%s, %s, greaterThanEqual(%s, vec%d(0.0)))' % (c, b, a, n))

    def r_mova(self, i):
        d = i.dst
        if d.rtype != ir.RT_ADDR:
            raise Refused('mova into a non-address register')
        # round to nearest, into the integer address register
        self.uses_a0 = True
        m = mask_letters(d.mask)
        n = len(m)
        src = self.srcm(i.srcs[0], d)
        rhs = 'int(round(%s))' % src if n == 1 else 'ivec%d(round(%s))' % (n, src)
        if n == 4:
            self.line('a0 = %s;' % rhs)
        else:
            self.line('a0.%s = %s;' % (m, rhs))

    def r_texld(self, i):
        if i.ctrl & 3:
            raise Refused('%s' % i.mnemonic())
        coord, samp = i.srcs[0], i.srcs[1]
        if samp.rtype != ir.RT_SAMPLER:
            raise Refused('texld without a sampler operand')
        t = self.samplers.get(samp.index)
        if t is None:
            raise Refused('s%d sampled without a dcl' % samp.index)
        uv = self.src_sw(coord, [0, 1] if t == '2d' else [0, 1, 2])
        self.assign(i.dst, full='texture(s%d, %s)' % (samp.index, uv))

    def r_texldl(self, i):
        coord, samp = i.srcs[0], i.srcs[1]
        if samp.rtype != ir.RT_SAMPLER:
            raise Refused('texldl without a sampler operand')
        t = self.samplers.get(samp.index)
        if t is None:
            raise Refused('s%d sampled without a dcl' % samp.index)
        uv = self.src_sw(coord, [0, 1] if t == '2d' else [0, 1, 2])
        self.assign(i.dst, full='textureLod(s%d, %s, %s)' % (samp.index, uv, self.comp(coord, 3)))

    def r_texldd(self, i):
        # texldd dst, coord, sampler, ddx, ddy (tex2Dgrad): explicit gradients, so no derivative orientation to fix
        coord, samp, ddx, ddy = i.srcs[0], i.srcs[1], i.srcs[2], i.srcs[3]
        if samp.rtype != ir.RT_SAMPLER:
            raise Refused('texldd without a sampler operand')
        t = self.samplers.get(samp.index)
        if t is None:
            raise Refused('s%d sampled without a dcl' % samp.index)
        sel = [0, 1] if t == '2d' else [0, 1, 2]
        self.assign(i.dst, full='textureGrad(s%d, %s, %s, %s)' % (samp.index, self.src_sw(coord, sel),
                                                                  self.src_sw(ddx, sel), self.src_sw(ddy, sel)))

    def r_texkill(self, i):
        d = i.dst
        if d.rtype == ir.RT_TEMP:
            self.temps.add(d.index)
            nm = 'r%d' % d.index
        elif d.rtype == ir.RT_TEXTURE and not self.vs and self.major < 3:
            nm = self.regname(d)
        else:
            raise Refused('texkill on %s' % self.regname(d))
        # SM2+: discard when any of the four components is < 0
        self.line('if (any(lessThan(%s, vec4(0.0)))) discard;' % nm)

    def r_ifc(self, i):
        cmpop = {1: '>', 2: '==', 3: '>=', 4: '<', 5: '!=', 6: '<='}.get(i.ctrl & 7)
        if cmpop is None:
            raise Refused('if with comparison %d' % (i.ctrl & 7))
        self.line('if (%s %s %s) {' % (self.comp(i.srcs[0], 0), cmpop, self.comp(i.srcs[1], 0)))
        self.depth += 1

    def r_if_bool(self, i):
        raise Refused('if on a bool register')

    def r_else(self, i):
        self.depth -= 1
        self.line('} else {')
        self.depth += 1

    def r_endif(self, i):
        self.depth -= 1
        self.line('}')

    def r_loop(self, i):
        # loop aL, i#: i#.x iterations, aL from i#.y, step i#.z
        if len(i.srcs) != 2 or i.srcs[0].rtype != ir.RT_LOOP or i.srcs[1].rtype != ir.RT_CONSTINT:
            raise Refused('loop operands')
        iv = self.base_expr(i.srcs[1])
        self.uses_aL = True
        self.loopn += 1
        n = 'n%d' % self.loopn
        self.line('for (int aL = %s.y, %s = 0; %s < %s.x; ++%s, aL += %s.z) {' % (iv, n, n, iv, n, iv))
        self.depth += 1

    def r_endloop(self, i):
        self.depth -= 1
        self.line('}')

    def r_rep(self, i):
        # rep i#: i#.x iterations, no aL
        if len(i.srcs) != 1 or i.srcs[0].rtype != ir.RT_CONSTINT:
            raise Refused('rep operand')
        iv = self.base_expr(i.srcs[0])
        self.loopn += 1
        n = 'n%d' % self.loopn
        self.line('for (int %s = 0; %s < %s.x; ++%s) {' % (n, n, iv, n))
        self.depth += 1

    def r_break(self, i):
        self.line('break;')

    def r_breakc(self, i):
        cmpop = {1: '>', 2: '==', 3: '>=', 4: '<', 5: '!=', 6: '<='}.get(i.ctrl & 7)
        if cmpop is None:
            raise Refused('breakc with comparison %d' % (i.ctrl & 7))
        self.line('if (%s %s %s) break;' % (self.comp(i.srcs[0], 0), cmpop, self.comp(i.srcs[1], 0)))

    def r_nop(self, i):
        pass

    # -- the module ------------------------------------------------------------------------------

    def run(self):
        sh = self.sh
        if sh.major < 2:
            raise Refused('SM1 stream')
        for ins in sh.insts:
            self.emit(ins)
        if self.depth != 1:
            raise Refused('unbalanced flow control')
        # The varyings this module writes / reads, by semantic
        if self.vs:
            if self.major >= 3:
                for nm in self.outregs:
                    if nm not in self.out_sems:
                        raise Refused('%s written without a dcl' % nm)
            else:
                for nm in self.outregs:
                    sem = VS2_OUT.get(nm)
                    if sem is None:
                        raise Refused('vs_2_0 output %s' % nm)
                    self.out_sems.setdefault(nm, []).append(sem)
                    if sem == 'fog0':
                        self.flags.add('fixed_fog')
                    if sem == 'psize0':
                        self.flags.add('point_size')
            self.outputs = []
            for nm, sems in self.out_sems.items():
                for s in sems:
                    if s == 'psize0':
                        self.flags.add('point_size')
                    if s not in self.outputs:
                        self.outputs.append(s)
            if 'position0' not in self.outputs:
                raise Refused('no position output')
            self.vconst = VCONST_LARGE if (self.relative or self.cmax >= VCONST_SMALL) else VCONST_SMALL
        else:
            self.reads = []
            for nm, s in self.inputs.items():
                if s not in self.reads:
                    self.reads.append(s)
            self.vconst = VCONST_SMALL
        self.pconst = PCONST
        if not self.vs and self.cmax >= PCONST:
            self.pconst = (self.cmax + 1 + 3) // 4 * 4
        return self.text()

    def text(self):
        rec = self.rec
        L = []
        P = L.append
        P('#version 450')
        P('// %s (%s), %d bytes, md5 %s' % (' = '.join(rec.names), self.sh.profile, len(rec.code), rec.md5))
        P('// Generated by Tools/VulkanShaders from the client\'s own bytecode; do not edit.')
        P('// The record, as fxc disassembles it:')
        for ln in ir.render(self.sh).split('\n')[1:-1]:
            P('//  ' + ln)
        P('')
        if self.vs:
            P('// Vertex inputs, at the fixed location of their semantic (the pipeline maps the engine\'s')
            P('// vertex layout onto these). A float3 element reads .w as 1.0, a float2 reads .zw as (0, 1).')
            for nm, sem in self.inputs.items():
                P('layout(location = %d) in vec4 %s;   // %s' % (VIN_LOCATION[sem], nm, sem))
            P('')
            P('// Varyings, by semantic; every component is written (0 where the bytecode writes none).')
            for sem in self.outputs:
                if sem in ('position0', 'psize0'):
                    continue
                t = VARYING_TYPE.get(sem, 'vec4')
                P('layout(location = %d) out %s %s;' % (VARYING_LOCATION[sem], t, sem))
        else:
            P('// Varyings read, by semantic, at the same locations the vertex module writes them.')
            for sem in self.reads:
                t = VARYING_TYPE.get(sem, 'vec4')
                P('layout(location = %d) in %s %s;' % (VARYING_LOCATION[sem], t, sem))
            P('')
            P('layout(location = 0) out vec4 oC0;')
            if self.samplers:
                P('')
                P('// Combined image samplers, one binding per D3D sampler slot (set 0).')
                for slot in sorted(self.samplers):
                    t = 'sampler2D' if self.samplers[slot] == '2d' else 'samplerCube'
                    P('layout(set = 0, binding = %d) uniform %s s%d;' % (slot, t, slot))
        P('')
        P('// One slice of the uniform ring. p[] and misc come FIRST so that a pixel module, which never')
        P('// reads v[], can declare v[64] whatever class the paired vertex module is: p[i] is at byte')
        P('// 16*i, misc at 256, v[i] at 272 + 16*i, in both classes. VCONST is 64 or 256 per the vertex')
        P('// record\'s highest constant (manifest `vconst=`); the slice is 272 + 16*VCONST bytes.')
        P('layout(std140, set = 1, binding = 0) uniform Regs {')
        P('    vec4 p[%d];     // pixel c0..c%d' % (PCONST, PCONST - 1))
        P('    vec4 misc;       // misc.x = alphaRef / 255 (fixed-function alpha test, 0 = off)')
        if self.pconst > PCONST:
            P('    vec4 v[%d];     // vertex c0..c%d (not read here; always 256 rows before px[])'
              % (VCONST_LARGE, VCONST_LARGE - 1))
            P('    vec4 px[%d];    // pixel c%d..c%d (manifest pconst=%d), at byte %d'
              % (self.pconst - PCONST, PCONST, self.pconst - 1, self.pconst, 272 + 16 * VCONST_LARGE))
        else:
            P('    vec4 v[%d];     // vertex c0..c%d' % (self.vconst, self.vconst - 1))
        P('} regs;')
        if self.defs or self.defi:
            P('')
            P('// Constants the bytecode def\'s: literals, never uploaded by the engine.')
            for idx in sorted(self.defs):
                P('const vec4 c%d = vec4(%s);' % (idx, ', '.join(fmt_float(x) for x in self.defs[idx])))
            for idx in sorted(self.defi):
                P('const ivec4 i%d = ivec4(%s);' % (idx, ', '.join('%d' % x for x in self.defi[idx])))
        if self.helpers:
            P('')
            P('// D3D9 semantics of the scalar ops.')
            if 'rsq' in self.helpers:
                P('float dx_rsq(float x) { return inversesqrt(abs(x)); }')
            if 'pow' in self.helpers:
                P('float dx_pow(float x, float y) { return x == 0.0 ? 0.0 : exp2(y * log2(abs(x))); }')
            if 'nrm' in self.helpers:
                P('vec4 dx_nrm(vec4 v) { return v * inversesqrt(dot(v.xyz, v.xyz)); }')
        P('')
        P('void main()')
        P('{')
        # prologue
        for t in sorted(self.temps):
            P('    vec4 r%d = vec4(0.0);' % t)
        if self.uses_a0:
            P('    ivec4 a0 = ivec4(0);')
        if self.vs:
            for nm in self.outregs:
                P('    vec4 %s = vec4(0.0);' % nm)
        else:
            P('    oC0 = vec4(0.0);')
            if self.uses_vpos:
                P('    vec4 vPos = vec4(gl_FragCoord.xy - 0.5, 0.0, 0.0);   // D3D9 vPos is the pixel corner')
            if self.uses_odepth:
                P('    vec4 oDepth = vec4(gl_FragCoord.z);   // oDepth -> gl_FragDepth (D3D: one write on every path)')
            for nm, sem in self.inputs.items():
                if sem == 'fog0':
                    rhs = 'vec4(fog0, 0.0, 0.0, 0.0)'
                elif self.major < 3 and nm.startswith('v'):
                    rhs = 'clamp(%s, 0.0, 1.0)' % sem       # ps_2_0 colour inputs are clamped by SM2
                else:
                    rhs = sem
                P('    vec4 %s = %s;' % (nm, rhs))
        P('')
        L.extend(self.body)
        P('')
        # epilogue
        if self.vs:
            for nm, sems in self.out_sems.items():
                for sem in sems:
                    if sem == 'position0':
                        P('    // D3D clip space has +y up, Vulkan +y down; z is already in [0, 1].')
                        P('    gl_Position = vec4(%s.x, -%s.y, %s.z, %s.w);' % (nm, nm, nm, nm))
                    elif sem == 'psize0':
                        P('    gl_PointSize = %s.x;' % nm)
                    elif sem == 'fog0':
                        P('    fog0 = %s.x;' % nm)
                    else:
                        P('    %s = %s;' % (sem, nm))
        else:
            P('    // The alpha test (GxRs_AlphaRef) is not in the bytecode: GREATEREQUAL against')
            P('    // misc.x = alphaRef / 255, off when 0; negative: gxAlphaToCoverage (alpha sharpened')
            P('    // to a one-pixel ramp around -misc.x for the coverage mask).')
            P(A2C_TEST.format(v='oC0'))
            if self.uses_odepth:
                P('    gl_FragDepth = clamp(oDepth.x, 0.0, 1.0);')
        P('}')
        return '\n'.join(L) + '\n'


# The alpha test of every translated pixel module (misc.x = alphaRef / 255; < 0 = alpha-to-coverage).
A2C_TEST = ('    if (regs.misc.x < 0.0) {{ {v}.a = clamp(({v}.a + regs.misc.x) / max(fwidth({v}.a), 1e-4) + 0.5, 0.0, 1.0); '
            'if ({v}.a <= 0.0) discard; }}\n'
            '    else if (regs.misc.x > 0.0 && floor({v}.a * 255.0 + 0.5) < floor(regs.misc.x * 255.0 + 0.5)) discard;')

RULES = {
    0: Emitter.r_nop, 1: Emitter.r_mov, 2: Emitter.r_add, 3: Emitter.r_sub, 4: Emitter.r_mad,
    5: Emitter.r_mul, 6: Emitter.r_rcp, 7: Emitter.r_rsq, 8: Emitter.r_dp3, 9: Emitter.r_dp4,
    10: Emitter.r_min, 11: Emitter.r_max, 12: Emitter.r_slt, 13: Emitter.r_sge, 14: Emitter.r_exp,
    15: Emitter.r_log, 18: Emitter.r_lrp, 19: Emitter.r_frc, 27: Emitter.r_loop, 29: Emitter.r_endloop,
    32: Emitter.r_pow, 35: Emitter.r_abs, 36: Emitter.r_nrm, 37: Emitter.r_sincos,
    38: Emitter.r_rep, 39: Emitter.r_endloop,
    40: Emitter.r_if_bool, 41: Emitter.r_ifc, 42: Emitter.r_else, 43: Emitter.r_endif,
    44: Emitter.r_break, 45: Emitter.r_breakc,
    46: Emitter.r_mova, 65: Emitter.r_texkill, 66: Emitter.r_texld, 88: Emitter.r_cmp,
    90: Emitter.r_dp2add, 93: Emitter.r_texldd, 95: Emitter.r_texldl,
}


# ---- the fixed-function pixel stage for the vs_2_0 records --------------------------------------

def companion_module(em, rec):
    """The pixel stage D3D runs after a vs_2_0 record that is drawn with no pixel shader.

    The four weather records (patter, rain, sand, snowpoint) exist in no pixel profile: the engine
    binds them with GxRs_PixelShader null, and D3D then runs the fixed-function cascade on the
    vertex shader's outputs: CURRENT = oD0, stage 0 combines the texel with it by GxRs_ColorOp0 /
    GxRs_AlphaOp0, the alpha test, then -- when fog is on -- the fog blend lerp(fogColor, rgb, oFog).
    The stage is emitted in the translated interface (color0 at 0, texcoord0 as a vec4 at 2, fog0
    at 10) with the fog colour in regs.p[FOG_ROW], one module per distinct (texcoord source, fog)
    combination, named by the md5 of its own text.

    Returns (glsl text, meta) with meta = dict(samplers, reads, flags)."""
    point_sprite = 'point_size' in em.flags
    fixed_fog = 'fixed_fog' in em.flags
    has_uv = 'texcoord0' in em.outputs
    reads = ['color0']
    if has_uv and not point_sprite:
        reads.append('texcoord0')
    if fixed_fog:
        reads.append('fog0')
    flags = set()
    if fixed_fog:
        flags.add('fixed_fog')
    if point_sprite:
        flags.add('point_sprite')
    L = []
    P = L.append
    P('#version 450')
    P('// The fixed-function pixel stage for the vs_2_0 records drawn with no pixel shader')
    P('// (GxRs_PixelShader null): ' + ', '.join(sorted(flags)) + '.')
    P('// Generated by Tools/VulkanShaders; do not edit. No bytecode stands behind this module: it is')
    P('// what D3D does after such a vertex shader, in the translated varying interface so it pairs')
    P('// with the translated vertex module. The device binds it when the draw\'s vertex record carries')
    P('// a `companion` line and the draw has no pixel shader.')
    P('//')
    P('//   CURRENT = color0 (the vertex shader\'s oD0, clamped as the fixed-function interpolator is);')
    if point_sprite:
        P('//   stage 0 samples s0 at gl_PointCoord: D3DRS_POINTSPRITEENABLE replaces every stage\'s')
        P('//   texcoord with the (0,0)..(1,1) sweep across the sprite, and the vertex shader writes')
        P('//   no texcoord at all; Vulkan\'s point coordinate has the same origin (upper left);')
    elif has_uv:
        P('//   stage 0 samples s0 at texcoord0.xy (the vertex shader\'s oT0);')
    else:
        P('//   stage 0 samples s0 at (0, 0): the vertex shader writes no texcoord;')
    P('//   the texel combines with CURRENT by the EGxTexOp in misc.z (colour) / misc.w (alpha)')
    P('//   (0 Mod, 1 Mod2x, 2 Add, 3 PassThru, 4 Decal, 5 Fade; above 5 the stage is skipped),')
    P('//   saturated as D3D saturates each stage;')
    P('//   the alpha test: discard when misc.x > 0 and the 8-bit alpha < misc.x * 255;')
    if fixed_fog:
        P('//   fog: oFog is the fog FACTOR with a programmable vertex shader (FOGVERTEXMODE is')
        P('//   ignored), blended as f * rgb + (1 - f) * fogColor only while D3DRS_FOGENABLE is on.')
        P('//   The colour and the enable come from regs.p[%d] = (fogColor.rgb / 255, fogOn ? 1 : 0),' % FOG_ROW)
        P('//   a row no shipped pixel record reads, written by the device on every draw.')
    else:
        P('//   fog: the vertex shader writes no oFog, so there is no factor to blend with; none.')
    P('')
    P('// Varyings read, by semantic, at the locations the translated vertex modules write them.')
    for sem in reads:
        t = VARYING_TYPE.get(sem, 'vec4')
        P('layout(location = %d) in %s %s;' % (VARYING_LOCATION[sem], t, sem))
    P('')
    P('layout(location = 0) out vec4 oC0;')
    P('')
    P('// Texture stage 0 (D3D sampler slot 0 = texture stage 0), set 0 binding 0.')
    P('layout(set = 0, binding = 0) uniform sampler2D s0;')
    P('')
    P('// The same slice as every translated module (p[] and misc first, v[] last).')
    P('layout(std140, set = 1, binding = 0) uniform Regs {')
    P('    vec4 p[%d];     // pixel c0..c%d; p[%d] = (fogColor.rgb, fogOn) for this module'
      % (PCONST, PCONST - 1, FOG_ROW))
    P('    vec4 misc;       // .x alphaRef / 255 (0 = test off), .z stage-0 colour op, .w stage-0 alpha op')
    P('    vec4 v[%d];     // vertex c0..c%d (not read here)' % (VCONST_SMALL, VCONST_SMALL - 1))
    P('} regs;')
    P('')
    P('// One EGxTexOp on all four channels; the caller keeps rgb from the colour op and a from the')
    P('// alpha op. CURRENT alpha and DIFFUSE alpha are the same value at stage 0.')
    P('vec4 ff_op(int op, vec4 t, vec4 cur)')
    P('{')
    P('    float a = cur.a;')
    P('    if (op == 0) return t * cur;')
    P('    if (op == 1) return 2.0 * t * cur;')
    P('    if (op == 2) return t + cur;')
    P('    if (op == 3) return cur;')
    P('    if (op == 4) return cur * a + t * (1.0 - a);')
    P('    if (op == 5) return t * a + cur * (1.0 - a);')
    P('    return cur;')
    P('}')
    P('')
    P('void main()')
    P('{')
    P('    vec4 cur = clamp(color0, 0.0, 1.0);')
    if point_sprite:
        P('    vec2 uv = gl_PointCoord;')
    elif has_uv:
        P('    vec2 uv = texcoord0.xy;')
    else:
        P('    vec2 uv = vec2(0.0);')
    P('    int cop = int(regs.misc.z + 0.5);')
    P('    int aop = int(regs.misc.w + 0.5);')
    P('    if (cop <= 5)')
    P('    {')
    P('        vec4 t = texture(s0, uv);')
    P('        cur = clamp(vec4(ff_op(cop, t, cur).rgb, ff_op(aop, t, cur).a), 0.0, 1.0);')
    P('    }')
    P('')
    P('    // The alpha test, the same discard as every translated pixel module.')
    P(A2C_TEST.format(v='cur'))
    if fixed_fog:
        P('')
        P('    // The fixed-function fog blend, on rgb only, with the vertex shader\'s oFog as the factor')
        P('    // (clamped as the hardware clamps the fog register) while fog is on.')
        P('    if (regs.p[%d].w > 0.0) cur.rgb = mix(regs.p[%d].rgb, cur.rgb, clamp(fog0, 0.0, 1.0));'
          % (FOG_ROW, FOG_ROW))
    P('')
    P('    oC0 = cur;')
    P('}')
    text = '\n'.join(L) + '\n'
    return text, dict(samplers={0: '2d'}, reads=reads, flags=flags)


def companion_block(md5, meta, names, spv_rel):
    L = ['record %s pixel ff %s vconst=%d pconst=%d' % (md5, spv_rel, VCONST_SMALL, PCONST)]
    for n in names:
        L.append('name %s' % n)
    for slot in sorted(meta['samplers']):
        L.append('sampler %d %s' % (slot, meta['samplers'][slot]))
    for s in meta['reads']:
        L.append('reads %s' % s)
    for f in sorted(meta['flags']):
        L.append('flag %s' % f)
    return '\n'.join(L)


def manifest_block(em, rec, spv_rel, companion=None):
    L = ['record %s %s %s %s vconst=%d pconst=%d'
         % (rec.md5, rec.kind, em.sh.profile, spv_rel, em.vconst, em.pconst)]
    for n in rec.names:
        L.append('name %s' % n)
    if rec.kind == 'pixel':
        for slot in sorted(em.samplers):
            L.append('sampler %d %s' % (slot, em.samplers[slot]))
        for s in em.reads:
            L.append('reads %s' % s)
    else:
        for nm, sem in em.inputs.items():
            L.append('input %s %s' % (sem, nm))
        for s in em.outputs:
            L.append('output %s' % s)
    for f in sorted(em.flags):
        L.append('flag %s' % f)
    if companion:
        L.append('companion %s' % companion)
    return '\n'.join(L)
