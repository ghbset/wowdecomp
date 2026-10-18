"""Builds our own shaders (ShaderSrc/*.hlsl) into the client's .bls containers under
Tools/CustomMpq/Shaders, which build_custom_mpq.py packs as Shaders\\... in Data/custom.MPQ and
Tools/VulkanShaders translates for the Vulkan device like any client shader.

    python3 -I Tools/CustomMpq/ShaderSrc/build_shaders.py [--xlate /root/d3d11-port/shader-xlate] [name ...]

Per shader: HLSL, preprocessed by vkd3d-compiler -E ($VKD3D_PREFIX or <xlate>/../vkd3d-install), ->
ps_3_0 bytecode by Microsoft's d3dcompiler_47 under wine (--fxc: the fxc.exe wrapper and DLL of
/root/wxl-depth/tools, wxl-depth's tools/fxc.cpp; $WINEPREFIX, default /tmp/testprefix, as its bakeps.py) ->
Shaders/Pixel/ps_3_0/<Name>.bls (D3D9, and the source of the Vulkan translation), then
shader-xlate's `xlate.py one` -> Shaders/Pixel/ps_4_0/<Name>.bls (D3D11). The outputs are committed,
so building the MPQ needs none of these tools.

Why fxc and not vkd3d's own d3dbc backend (FarBlend's): vkd3d 2.1 allocates the VFog marches 40 to 98
temps, ps_3_0 has 32. fxc fits them, and its output is what every stock shader the translators already
handle looks like. A variant is a few #defines plus an #include of its entry file, preprocessed with
ShaderSrc as the working directory (vkd3d-compiler has no -D; the fxc wrapper resolves no includes).

VFog (docs/VFOG_PORT.md section 5): VFogMarch{Q}[L][N] and VFogComposite{Q}[L][N], Q = 1/2/3 (STEPS
16/24/32 as coa-vfog CMakeLists.txt), L = LOCAL_LIGHTS, N = VF_NOISE (density variation + authored noise),
plus VFogTemporal and VFogHistoryDepth, and the god rays VFogRayMask, VFogRayBlur and VFogRayComposite.
Grading (coa vp_grade): VFogGrade.
Water (coa vw_*): VFogWaterDepth, VFogWaterMask, VFogWater{1,2,3} (SSR_STEPS 0/16/32) and VFogRippleStep;
GPU waves VFogWave{Evolve,Fft,Surface,Mip,Pack}.
Fading models (wxl-vfog 0.6.3): VFogDepthCopy. MSAA sample split (coa): VFogSilhouette and VFogCompositeSplit{Q}[L][N].
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'Shaders')

# Record masks as the stock full-screen FFX pixel shaders carry them (FFXComposite: 0x800, 0x200, 3, 0):
# a = inputs (0x800 texcoord 0), b = outputs (0x200 colour 0), c = the samplers used, a bit each. The
# loader (CGxDevice::IShaderLoad / CGxShader::Load) only stores them.
TEXCOORD0, COLOR0 = 0x800, 0x200


def samplers(*regs):
    return sum(1 << r for r in regs)


# name: (profile, a, b, c, d, entry file, defines)
SHADERS = {
    'VFogTemporal': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0, 1, 2, 3), 0, 'VFogTemporal', {}),
    'VFogHistoryDepth': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0), 0, 'VFogHistoryDepth', {}),
    # god rays (coa vf_ray_*): mask, radial blur x2, composite after the world
    'VFogRayMask': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0, 1), 0, 'VFogRayMask', {}),
    'VFogRayBlur': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0), 0, 'VFogRayBlur', {}),
    'VFogRayComposite': ('ps_3_0', TEXCOORD0, COLOR0, samplers(2, 3), 0, 'VFogRayComposite', {}),
    # colour grading (coa vp_grade): the light's curve over the finished world, before names and the UI
    'VFogGrade': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0), 0, 'VFogGrade', {}),
    # modern water (coa vw_*, docs "Water"): linear depth copies, then one shading pass per water class
    'VFogWaterDepth': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0), 0, 'VFogWaterDepth', {'MASK': 0}),
    'VFogWaterMask': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0, 1), 0, 'VFogWaterDepth', {'MASK': 1}),
    # water ripples and wakes (coa vw_ripple_step): one 30 Hz step of the Gr1616F height field
    'VFogRippleStep': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0), 0, 'VFogRippleStep', {}),
    # GPU waves (coa vw_fft_*, docs "Waves"): evolve + fold, radix-2 stages, surface moments / foam, moment mips, pack
    'VFogWaveEvolve': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0, 1), 0, 'VFogWaveEvolve', {}),
    'VFogWaveFft': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0), 0, 'VFogWaveFft', {}),
    'VFogWaveSurface': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0, 1, 2), 0, 'VFogWaveSurface', {}),
    'VFogWaveMip': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0), 0, 'VFogWaveMip', {}),
    'VFogWavePack': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0, 1), 0, 'VFogWavePack', {}),
    # fading models' cross-fade (wxl-vfog 0.6.3, vfogFadeStencil): the scene depth into R32F, behind and with them
    'VFogDepthCopy': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0), 0, 'VFogDepthCopy', {}),
    # MSAA silhouette samples marked in the stencil by an oDepth pass (coa vf_silhouette_mask; Vulkan: gl_FragDepth)
    'VFogSilhouette': ('ps_3_0', TEXCOORD0, COLOR0, samplers(0), 0, 'VFogSilhouette', {}),
}
# VFogWater{1,2,3}: coa ps_vw_shade_low/mid/high (SSR_STEPS 0/16/32, cmake/WaterShaders.cmake)
for _v, _ssr in ((1, 0), (2, 16), (3, 32)):
    SHADERS['VFogWater%d' % _v] = ('ps_3_0', TEXCOORD0, COLOR0, samplers(*range(0, 13), 14, 15), 0, 'VFogWater',
                                   {'SSR_STEPS': _ssr})
for _q, _steps in ((1, 16), (2, 24), (3, 32)):
    for _lights in (0, 1):
        for _noise in (0, 1):
            _suffix = '%d%s%s' % (_q, 'L' if _lights else '', 'N' if _noise else '')
            _defs = {'STEPS': _steps, 'LOCAL_LIGHTS': _lights, 'VF_NOISE': _noise}
            _march_noise = (9, 10) if _noise else ()
            _composite_noise = ((9,) if _lights else (9, 10)) if _noise else ()    # VFogComposite.hlsl: L drops s10
            SHADERS['VFogMarch' + _suffix] = ('ps_3_0', TEXCOORD0, COLOR0, samplers(0, *_march_noise), 0,
                                              'VFogMarch', _defs)
            SHADERS['VFogComposite' + _suffix] = ('ps_3_0', TEXCOORD0, COLOR0,
                                                  samplers(0, 1, 3, 4, *_composite_noise), 0, 'VFogComposite', _defs)
            # MSAA sample split (coa vf_composite_split, vfogMsaaSplit): the same composite per silhouette side
            SHADERS['VFogCompositeSplit' + _suffix] = ('ps_3_0', TEXCOORD0, COLOR0,
                                                       samplers(0, 1, 3, 4, *_composite_noise), 0, 'VFogComposite',
                                                       dict(_defs, SPLIT=1))


def bls(code, a, b, c, d):
    rec = struct.pack('<IIHHI', a, b, c, d, len(code)) + code
    rec += b'\0' * (-len(rec) % 4)
    return struct.pack('<4sII', b'HSXG', 0x10003, 1) + rec


def build(name, a, vk, env):
    prof, ra, rb, rc, rd, entry, defines = SHADERS[name]
    src = ''.join('#define %s %s\n' % kv for kv in sorted(defines.items())) + '#include "%s.hlsl"\n' % entry
    r = subprocess.run([os.path.join(vk, 'bin', 'vkd3d-compiler'), '-x', 'hlsl', '-E', '-o', '-', '-'],
                       input=src.encode(), capture_output=True, env=env, cwd=HERE)
    if r.returncode or not r.stdout:
        return '%s: preprocess failed: %s' % (name, r.stderr.decode(errors='replace'))
    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, name + '.hlsl'), 'wb') as f:
            f.write(r.stdout)
        out = os.path.join(tmp, name + '.bin')
        r = subprocess.run(['wine', os.path.join(a.fxc, 'fxc.exe'), os.path.join(a.fxc, 'd3dcompiler_47.dll'),
                            os.path.join(tmp, name + '.hlsl'), out, prof], capture_output=True, cwd=tmp,
                           env=dict(os.environ, WINEPREFIX=os.environ.get('WINEPREFIX', '/tmp/testprefix'),
                                    WINEDEBUG='-all'))
        if r.returncode or not os.path.isfile(out):
            return '%s: fxc failed: %s' % (name, r.stdout.decode(errors='replace')[-2000:])
        with open(out, 'rb') as f:
            code = f.read()
    kind = 'Pixel' if prof.startswith('ps') else 'Vertex'
    d3d9 = os.path.join(OUT, kind, prof, name + '.bls')
    os.makedirs(os.path.dirname(d3d9), exist_ok=True)
    with open(d3d9, 'wb') as f:
        f.write(bls(code, ra, rb, rc, rd))
    sm4 = os.path.join(OUT, kind, prof[:3] + '4_0', name + '.bls')
    os.makedirs(os.path.dirname(sm4), exist_ok=True)
    r = subprocess.run([sys.executable, os.path.join(a.xlate, 'xlate.py'), 'one', d3d9, '-o', sm4], capture_output=True)
    if r.returncode or not os.path.isfile(sm4) or b' ok ' not in r.stdout:
        return '%s: xlate failed: %s %s' % (name, r.stdout.decode(errors='replace'), r.stderr.decode(errors='replace'))
    print('%s: %s (%d bytes), %s (%d bytes)' % (name, d3d9, os.path.getsize(d3d9), sm4, os.path.getsize(sm4)),
          flush=True)
    return None


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--xlate', default='/root/d3d11-port/shader-xlate')
    ap.add_argument('--fxc', default='/root/wxl-depth/tools', help='folder holding fxc.exe and d3dcompiler_47.dll')
    ap.add_argument('-j', type=int, default=8, help='shaders compiled at once (each is one wine process)')
    ap.add_argument('names', nargs='*', help='only these shaders (default: all)')
    a = ap.parse_args(argv)
    vk = os.environ.get('VKD3D_PREFIX', os.path.join(a.xlate, '..', 'vkd3d-install'))
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(vk, 'lib'))
    for name in a.names:
        if name not in SHADERS:
            sys.exit('unknown shader %s' % name)
    with concurrent.futures.ThreadPoolExecutor(a.j) as pool:
        errors = [e for e in pool.map(lambda n: build(n, a, vk, env), a.names or list(SHADERS)) if e]
    if errors:
        sys.exit('\n'.join(errors))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
