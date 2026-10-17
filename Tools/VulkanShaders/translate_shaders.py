"""Builds the Vulkan backend's translated shaders from your own 3.3.5a client: the client's DX9
shaders (Shaders\\Vertex\\vs_3_0\\*.bls, Shaders\\Pixel\\ps_3_0\\*.bls and the profiles the engine
falls back to), translated to SPIR-V, plus the manifest the device reads.

The translated modules are derived from the client's own shader archives, so each user makes them
from their own copy of the game; nothing they produce is versioned. Python 3.8+, standard library
only, plus glslang.

    python Tools/VulkanShaders/translate_shaders.py --client "C:/Games/World of Warcraft 3.3.5a"
    python Tools/VulkanShaders/translate_shaders.py --client <client> --glslang <path to glslang.exe>
    python Tools/VulkanShaders/translate_shaders.py --client <client> --deploy   # also copy into the client

The output is Tools/VulkanShaders/translated/ (gitignored; --out elsewhere). The device reads
<folder of the executable>\\VulkanShaders\\manifest.txt with vertex\\ and pixel\\ beside it, or the
folder named by the GX_VULKAN_SHADERS environment variable; --deploy copies the manifest and every
.spv into <client>\\VulkanShaders\\, removing stale modules.
Data\\custom.MPQ (Tools/CustomMpq/build_custom_mpq.py) packs that folder and is read first;
the loose folder is the fallback, and the only source while GX_VULKAN_SHADERS is set.
"""
import argparse
import glob
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, 'translated')


def find_data(client):
    """The client's Data folder, from the client folder or the Data folder itself."""
    for cand in (os.path.join(client, 'Data'), client):
        if os.path.isfile(os.path.join(cand, 'common.MPQ')):
            return os.path.normpath(cand)
    return None


def find_glslang(explicit):
    """--glslang, $GLSLANG, the Vulkan SDK, then PATH."""
    for cand in (explicit, os.environ.get('GLSLANG')):
        if cand and os.path.isfile(cand):
            return cand
    sdk = os.environ.get('VULKAN_SDK')
    if sdk:
        for name in ('glslang.exe', 'glslangValidator.exe', 'glslang', 'glslangValidator'):
            for sub in ('Bin', 'bin'):
                path = os.path.join(sdk, sub, name)
                if os.path.isfile(path):
                    return path
    for name in ('glslang', 'glslangValidator'):
        found = shutil.which(name)
        if found:
            return found
    return None


def deploy(out, client):
    """Mirrors what the device reads (manifest.txt, index.txt, every .spv) into <client>/VulkanShaders/."""
    dst = os.path.join(client, 'VulkanShaders')
    copied = removed = 0
    os.makedirs(dst, exist_ok=True)
    for name in ('manifest.txt', 'index.txt'):
        shutil.copy2(os.path.join(out, name), os.path.join(dst, name))
        copied += 1
    for kind in ('vertex', 'pixel'):
        src_dir, dst_dir = os.path.join(out, kind), os.path.join(dst, kind)
        os.makedirs(dst_dir, exist_ok=True)
        wanted = {os.path.basename(p) for p in glob.glob(os.path.join(src_dir, '*.spv'))}
        for name in wanted:
            shutil.copy2(os.path.join(src_dir, name), os.path.join(dst_dir, name))
            copied += 1
        for p in glob.glob(os.path.join(dst_dir, '*.spv')):
            if os.path.basename(p) not in wanted:
                os.remove(p)
                removed += 1
    print('deployed %d file(s) to %s (%d stale removed)' % (copied, dst, removed))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--client', required=True, help='the 3.3.5a (12340) client folder (or its Data folder)')
    ap.add_argument('--glslang', default=None, help='glslang.exe / glslangValidator.exe (also $GLSLANG)')
    ap.add_argument('--out', default=DEFAULT_OUT, help='the output tree (default: translated/ here)')
    ap.add_argument('--deploy', action='store_true', help='copy the result into <client>/VulkanShaders/')
    ap.add_argument('--names', default=None, help='only these Name:perm (comma-separated), for a look')
    ap.add_argument('--print', action='store_true', help='print the GLSL of each emitted record')
    args = ap.parse_args(argv)

    data = find_data(args.client)
    if not data:
        print('no common.MPQ under %r or its Data folder: --client is the 3.3.5a client folder' % args.client,
              file=sys.stderr)
        return 2
    glslang = find_glslang(args.glslang)
    if not glslang:
        print('glslang not found: pass --glslang, set $GLSLANG or $VULKAN_SDK, or put it on PATH '
              '(prebuilt binaries: https://github.com/KhronosGroup/glslang/releases)', file=sys.stderr)
        return 2
    out = os.path.normpath(args.out)
    print('client data : %s\nglslang     : %s\noutput      : %s' % (data, glslang, out))

    sys.path.insert(0, HERE)
    from bls import translate
    argv2 = ['--data', data, '--glslang', glslang]
    if args.names:
        argv2 += ['--names', args.names]
        if args.print:
            argv2.append('--print')
    else:
        argv2 += ['--out', out]
    rc = translate.main(argv2)
    if rc != 0:
        print('translation failed (%d)' % rc, file=sys.stderr)
        return rc
    if args.deploy and not args.names:
        deploy(out, os.path.dirname(data))
    if not args.names:
        print('\ndone: %s' % out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
