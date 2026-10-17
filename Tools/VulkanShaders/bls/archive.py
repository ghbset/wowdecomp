"""The client's shader containers: the archives in load order, the `(listfile)` inventory, the
profile fallback the engine applies, and every record it loads.

The engine (CGxDevice::IShaderLoad) opens `Shaders\\<Vertex|Pixel>\\<profile>\\<name>.bls` for the
profile its caps report and falls back one profile at a time when the file is missing or bad:
vertex vs_3_0 -> vs_2_0 -> vs_1_1, pixel ps_3_0 -> ps_2_0 -> ps_1_4 -> ps_1_1. The Vulkan device
reports vs_3_0 / ps_3_0, so that is the chain walked here.

A `.bls` container: `uint32 magic 'GXSH'`, `uint32 version 0x10003`, `uint32 permutationCount`, then
that many records of `uint32 a; uint32 b; uint16 c; uint16 d; uint32 size; uint8 code[size]`, each
padded to 4 bytes. The record's `code` is what the engine keeps in CGxShader::m_code; its md5 is
the manifest's key.
"""
import collections
import hashlib
import os
import struct
import sys

from . import dx9ir as ir
from . import mpq_extract as mq

BS = chr(92)
VS_FALLBACK = ['vs_3_0', 'vs_2_0', 'vs_1_1']
PS_FALLBACK = ['ps_3_0', 'ps_2_0', 'ps_1_4', 'ps_1_1']


def archives(datadir):
    """Every archive of the load order present in the client's Data folder, opened."""
    if not datadir or not os.path.isfile(os.path.join(datadir, 'common.MPQ')):
        sys.exit("no client archives: %r holds no common.MPQ" % datadir)
    out = []
    for n in mq.LOAD_ORDER:
        p = os.path.join(datadir, n)
        if os.path.isdir(p):
            sys.stderr.write('-- %s is a folder patch (loose files), not an archive: not read\n' % n)
            continue
        if os.path.exists(p):
            try:
                out.append((n, mq.MPQ(p)))
            except OSError as e:
                sys.stderr.write('!! %s unreadable, skipped: %s\n' % (n, e))
    return out


def grab(arch, path):
    """The bytes of `path` from the last archive that holds it (the load order's winner)."""
    found = None
    for n, m in arch:
        if m.find(path) is not None:
            found = (n, m)
    if not found:
        return None, None
    n, m = found
    data, _ = m.read_file(path)
    return n, data


def split(data):
    """The records of one .bls container."""
    magic, ver, nperm = struct.unpack_from('<III', data, 0)
    recs, off = [], 12
    for i in range(nperm):
        a, b, c, d, size = struct.unpack_from('<IIHHI', data, off)
        off += 16
        code = data[off:off + size]
        off += size
        off = (off + 3) & ~3
        recs.append(dict(i=i, a=a, b=b, c=c, d=d, size=size, code=code))
    return magic, ver, nperm, recs, off


def entries(arch, prefix='shaders'):
    """Every listed path under `prefix`, across the whole load order, case-insensitively."""
    out = set()
    for n, m in arch:
        if m.find('(listfile)') is None:
            continue
        data, _ = m.read_file('(listfile)')
        for ln in data.decode('ascii', 'replace').replace('\r\n', '\n').split('\n'):
            ln = ln.strip()
            if ln.lower().startswith(prefix.lower()):
                out.add(ln)
    return sorted(out)


def inventory(arch):
    """{(kind, profile_lower): {name_lower: name_as_listed}} from the listfile."""
    inv = collections.defaultdict(dict)
    for n in entries(arch, prefix='shaders'):
        p = n.split(BS)
        if len(p) == 4 and p[1].lower() in ('vertex', 'pixel') and p[3].lower().endswith('.bls'):
            inv[(p[1].lower(), p[2].lower())][p[3][:-4].lower()] = p[3][:-4]
    return inv


def resolve(inv):
    """Which container the engine loads per (kind, name) under the 3_0 caps: [(kind, profile, name)]."""
    out = []
    for kind, chain in (('vertex', VS_FALLBACK), ('pixel', PS_FALLBACK)):
        names = {}
        for prof in chain:
            for low, name in inv.get((kind, prof), {}).items():
                names.setdefault(low, name)
        for low in sorted(names):
            for prof in chain:
                if low in inv.get((kind, prof), {}):
                    out.append((kind, prof, inv[(kind, prof)][low]))
                    break
    return out


class Record(object):
    __slots__ = ('kind', 'profile', 'name', 'perm', 'code', 'md5', 'sh', 'names', 'hdr')

    @property
    def key(self):
        return '%s:%d' % (self.name, self.perm)


def enumerate_records(datadir):
    """Every record the engine loads, resolved through its fallback chain (an empty or missing
    container falls back one profile, as the engine does). Byte-identical records are separate
    entries with the same md5; the caller folds them."""
    arch = archives(datadir)
    inv = inventory(arch)
    out = []
    for kind, prof, name in resolve(inv):
        chain = VS_FALLBACK if kind == 'vertex' else PS_FALLBACK
        data = None
        for prof2 in chain[chain.index(prof):]:
            low = name.lower()
            if low not in inv.get((kind, prof2), {}):
                continue
            path = BS.join(['Shaders', kind.capitalize(), prof2, inv[(kind, prof2)][low] + '.bls'])
            src, data = grab(arch, path)
            if data is None or len(data) < 12:
                data = None
                continue
            prof = prof2
            break
        if data is None:
            continue
        magic, ver, nperm, records, consumed = split(data)
        for rec in records:
            r = Record()
            r.kind, r.profile, r.name, r.perm = kind, prof, name, rec['i']
            r.code = rec['code']
            r.md5 = hashlib.md5(r.code).hexdigest()
            r.hdr = (rec['a'], rec['b'], rec['c'], rec['d'])
            try:
                r.sh = ir.decode(r.code)
            except ValueError:
                r.sh = None       # an SM1 stream: not translated
            out.append(r)
    return out
