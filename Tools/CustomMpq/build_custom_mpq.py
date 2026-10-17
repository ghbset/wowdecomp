"""Packs everything our own exes (Wow32.exe / Wow64.exe) need and stock Wow.exe does not into one
archive, Data/custom.MPQ. The exes open it above every patch MPQ (Client.cpp OpenArchives); stock
Wow.exe never opens it (its patch search is patch-*.MPQ).

    python3 -I Tools/CustomMpq/build_custom_mpq.py --client <client> --out <client>/Data/custom.MPQ
    python3 -I Tools/CustomMpq/build_custom_mpq.py --list <archive.MPQ>

Contents:
  VulkanShaders\\...      the Vulkan backend's translated shaders (manifest.txt, index.txt,
                          vertex\\*.spv, pixel\\*.spv), from <client>/VulkanShaders as
                          Tools/VulkanShaders/translate_shaders.py --deploy leaves them (--vulkan
                          to take another folder, e.g. Tools/VulkanShaders/translated).
  Shaders\\*\\?s_4_0\\...   the D3D11 backend's SM4 pack, copied out of <client>/Data/patch-R.MPQ
                          (--d3d11-mpq to take another archive).
  Textures\\...           every file in Tools/CustomMpq/Textures beside this script (the
                          selection-circle styles from wxl-aoeindicators, selectionCircleStyle).
  Shaders\\...            our own shaders, Tools/CustomMpq/Shaders (ps_3_0 for D3D9, ps_4_0 for D3D11),
                          built from ShaderSrc/*.hlsl by ShaderSrc/build_shaders.py (VFog*: volumetric
                          fog, docs/VFOG_PORT.md). Tools/VulkanShaders translates the ps_3_0 ones with the client's.
  VFog\\...               every file in Tools/CustomMpq/VFog (fogdata.bin: the Classic/Forever fog layers,
                          coa-vfog data/fogdata.bin, read by VolumetricFogData.cpp).
Cache/vkpipelines.bin is a runtime cache and stays loose.

Needs StormLib (libstorm.so, Debian package libstorm9). Python 3.8+, standard library only.
"""
import argparse
import ctypes
import os
import sys

MPQ_CREATE_LISTFILE = 0x00100000
MPQ_CREATE_ATTRIBUTES = 0x00200000
MPQ_CREATE_ARCHIVE_V2 = 0x01000000
MPQ_OPEN_READ_ONLY = 0x00000100
MPQ_FILE_COMPRESS = 0x00000200
MPQ_FILE_REPLACEEXISTING = 0x80000000
MPQ_COMPRESSION_ZLIB = 0x02

DWORD = ctypes.c_uint32
HANDLE = ctypes.c_void_p


class FindData(ctypes.Structure):
    _fields_ = [('cFileName', ctypes.c_char * 1024), ('szPlainName', ctypes.c_char_p),
                ('dwHashIndex', DWORD), ('dwBlockIndex', DWORD), ('dwFileSize', DWORD),
                ('dwFileFlags', DWORD), ('dwCompSize', DWORD), ('dwFileTimeLo', DWORD),
                ('dwFileTimeHi', DWORD), ('lcLocale', DWORD)]


def storm():
    for name in ('libstorm.so.9', 'libstorm.so', 'StormLib.dll'):
        try:
            lib = ctypes.CDLL(name)
            break
        except OSError:
            continue
    else:
        sys.exit('StormLib not found (apt install libstorm9)')
    lib.SFileFindFirstFile.restype = HANDLE
    lib.SFileGetFileSize.restype = DWORD
    return lib


def mpq_files(lib, path):
    """{name: bytes} of every file in an archive, the internal (listfile)/(attributes) left out."""
    h = HANDLE()
    if not lib.SFileOpenArchive(path.encode(), 0, MPQ_OPEN_READ_ONLY, ctypes.byref(h)):
        sys.exit('cannot open %s' % path)
    out = {}
    fd = FindData()
    f = lib.SFileFindFirstFile(h, b'*', ctypes.byref(fd), None)
    ok = bool(f)
    while ok:
        name = fd.cFileName.decode()
        if not name.startswith('('):
            hf = HANDLE()
            if not lib.SFileOpenFileEx(h, fd.cFileName, 0, ctypes.byref(hf)):
                sys.exit('cannot open %s in %s' % (name, path))
            size = lib.SFileGetFileSize(hf, None)
            buf = ctypes.create_string_buffer(size)
            got = DWORD()
            if size and not lib.SFileReadFile(hf, buf, size, ctypes.byref(got), None) or got.value != size:
                sys.exit('short read of %s in %s' % (name, path))
            lib.SFileCloseFile(hf)
            out[name] = buf.raw
        ok = lib.SFileFindNextFile(HANDLE(f), ctypes.byref(fd))
    if f:
        lib.SFileFindClose(HANDLE(f))
    lib.SFileCloseArchive(h)
    return out


def folder_files(prefix, folder):
    out = {}
    for root, _, names in os.walk(folder):
        for n in names:
            p = os.path.join(root, n)
            rel = os.path.relpath(p, folder).replace('/', '\\')
            with open(p, 'rb') as fh:
                out[prefix + '\\' + rel] = fh.read()
    return out


def write_mpq(lib, path, files):
    if os.path.exists(path):
        os.remove(path)
    slots = 1 << max(10, (len(files) * 2).bit_length())  # hash table: power of two, half full at most
    h = HANDLE()
    if not lib.SFileCreateArchive(path.encode(), MPQ_CREATE_ARCHIVE_V2 | MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES,
                                  slots, ctypes.byref(h)):
        sys.exit('cannot create %s' % path)
    for name in sorted(files):
        data = files[name]
        hf = HANDLE()
        if not lib.SFileCreateFile(h, name.encode(), ctypes.c_uint64(0), DWORD(len(data)), DWORD(0),
                                   DWORD(MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING), ctypes.byref(hf)):
            sys.exit('cannot add %s' % name)
        if data and not lib.SFileWriteFile(hf, data, DWORD(len(data)), DWORD(MPQ_COMPRESSION_ZLIB)):
            sys.exit('cannot write %s' % name)
        lib.SFileFinishFile(hf)
    lib.SFileCompactArchive(h, None, 0)
    lib.SFileCloseArchive(h)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--client', help='the client folder (VulkanShaders/ and Data/patch-R.MPQ are read from it)')
    ap.add_argument('--vulkan', help='the translated Vulkan shader folder (default <client>/VulkanShaders)')
    ap.add_argument('--d3d11-mpq', help='the archive holding the SM4 pack (default <client>/Data/patch-R.MPQ)')
    ap.add_argument('--out', help='the archive to write')
    ap.add_argument('--list', metavar='MPQ', help='only list an archive')
    a = ap.parse_args(argv)
    lib = storm()

    if a.list:
        files = mpq_files(lib, a.list)
        for n in sorted(files):
            print('%10d  %s' % (len(files[n]), n))
        print('%d file(s)' % len(files))
        return 0
    if not a.out or not (a.client or (a.vulkan and a.d3d11_mpq)):
        ap.error('--out and --client (or both --vulkan and --d3d11-mpq) are required')

    vulkan = a.vulkan or os.path.join(a.client, 'VulkanShaders')
    d3d11 = a.d3d11_mpq or os.path.join(a.client, 'Data', 'patch-R.MPQ')
    if not os.path.isfile(os.path.join(vulkan, 'manifest.txt')):
        sys.exit('no manifest.txt in %s (run Tools/VulkanShaders/translate_shaders.py first)' % vulkan)

    files = folder_files('VulkanShaders', vulkan)
    sm4 = {n: d for n, d in mpq_files(lib, d3d11).items()
           if n.lower().split('\\')[2:3] in (['vs_4_0'], ['ps_4_0'])}
    if not sm4:
        sys.exit('no Shaders\\*\\vs_4_0 / ps_4_0 files in %s' % d3d11)
    files.update(sm4)
    textures = folder_files('Textures', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'Textures'))
    files.update(textures)
    ours = folder_files('Shaders', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'Shaders'))
    files.update(ours)
    vfog = folder_files('VFog', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'VFog'))
    files.update(vfog)

    write_mpq(lib, a.out, files)
    back = mpq_files(lib, a.out)
    bad = [n for n in files if back.get(n) != files[n]]
    if bad:
        sys.exit('verify failed for %d file(s), first %s' % (len(bad), bad[0]))
    print('%s: %d file(s) (%d Vulkan, %d D3D11, %d textures, %d own shaders, %d vfog data), %d bytes, verified' %
          (a.out, len(back), len(files) - len(sm4) - len(textures) - len(ours) - len(vfog), len(sm4), len(textures),
           len(ours), len(vfog), os.path.getsize(a.out)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
