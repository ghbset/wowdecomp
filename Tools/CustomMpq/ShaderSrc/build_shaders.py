"""Builds our own shaders (ShaderSrc/*.hlsl) into the client's .bls containers under
Tools/CustomMpq/Shaders, which build_custom_mpq.py packs as Shaders\\... in Data/custom.MPQ and
Tools/VulkanShaders translates for the Vulkan device like any client shader.

    python3 -I Tools/CustomMpq/ShaderSrc/build_shaders.py [--xlate /root/d3d11-port/shader-xlate]

Per shader: HLSL -> ps_3_0 bytecode (vkd3d-compiler, $VKD3D_PREFIX or <xlate>/../vkd3d-install) ->
Shaders/Pixel/ps_3_0/<Name>.bls (D3D9, and the source of the Vulkan translation), then
shader-xlate's `xlate.py one` -> Shaders/Pixel/ps_4_0/<Name>.bls (D3D11). The outputs are committed,
so building the MPQ needs neither tool.
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), 'Shaders')
# name: (profile, record a, b, c, d) -- the masks copied from the stock shader it stands beside
SHADERS = {'FarBlend': ('ps_3_0', 0x1e80, 0x200, 1, 0)}


def bls(code, a, b, c, d):
    rec = struct.pack('<IIHHI', a, b, c, d, len(code)) + code
    rec += b'\0' * (-len(rec) % 4)
    return struct.pack('<4sII', b'HSXG', 0x10003, 1) + rec


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--xlate', default='/root/d3d11-port/shader-xlate')
    a = ap.parse_args(argv)
    vk = os.environ.get('VKD3D_PREFIX', os.path.join(a.xlate, '..', 'vkd3d-install'))
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.join(vk, 'lib'))
    for name, (prof, ra, rb, rc, rd) in SHADERS.items():
        src = os.path.join(HERE, name + '.hlsl')
        r = subprocess.run([os.path.join(vk, 'bin', 'vkd3d-compiler'), '-x', 'hlsl', '-p', prof, '-b', 'd3dbc', '-o', '-', src],
                           capture_output=True, env=env)
        if r.returncode or not r.stdout:
            sys.exit('%s: %s' % (name, r.stderr.decode(errors='replace')))
        kind = 'Pixel' if prof.startswith('ps') else 'Vertex'
        d3d9 = os.path.join(OUT, kind, prof, name + '.bls')
        os.makedirs(os.path.dirname(d3d9), exist_ok=True)
        with open(d3d9, 'wb') as f:
            f.write(bls(r.stdout, ra, rb, rc, rd))
        sm4 = os.path.join(OUT, kind, prof[:3] + '4_0', name + '.bls')
        os.makedirs(os.path.dirname(sm4), exist_ok=True)
        r = subprocess.run([sys.executable, os.path.join(a.xlate, 'xlate.py'), 'one', d3d9, '-o', sm4], capture_output=True)
        if r.returncode or not os.path.isfile(sm4) or b' ok ' not in r.stdout:
            sys.exit('%s: xlate failed: %s %s' % (name, r.stdout.decode(errors='replace'), r.stderr.decode(errors='replace')))
        print('%s: %s (%d bytes), %s (%d bytes)' % (name, d3d9, os.path.getsize(d3d9), sm4, os.path.getsize(sm4)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
