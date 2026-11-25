#!/usr/bin/env python3
"""Add-on cars for the port (2026-11-20): what VltEd and Ed do on Windows, done at packaging time.

    .venv/bin/python tools/addon_cars.py --mods dist/mods_to_add --game <game folder> --out <dir>

An add-on car (Koenigsegg Regera, CCXR, Ferrari FXX-K Evo, Lamborghini Aventador SVJ, Miura SV) ships its model
(CARS/<NAME>/GEOMETRY.BIN ...), VltEd scripts (.nfsms: copy_node / update_field / resize_field ...) and an Ed
config (car type, collision, names). On Windows VltEd rewrites GLOBAL/attributes.bin and Ed GLOBAL/GLOBALB.LZC
(and the language files). Here nothing of the game is changed; this writes into <dir>:
  - ADDONCARS.BIN: an attribute vault "addon_cars" with every collection the scripts create (copy_node copies the
    source collection's fields - its layout block and its own entries, with the data and strings they point at -
    under the new key and parent, renames CollectionName, then the script's edits on it are applied). core.nfsu
    mounts it at boot after the game's car vault, the way the Police Career plugin mounts its police cars, with
    the game's own vault loader (FUN_006ac6e0);
  - GLOBALB.BUN: the game's GLOBALB bundle (GlobalB.lzc unpacked: EA RefPack) with, per car, a CarTypeInfo row
    (chunk 0x34600, 0x130 bytes, copied from the car's source and renamed), a collision (a copy of the Ed config's
    CopyFrom car's 0x3b901 chunk with its key, jhash(name), at +0x40) and its parts (chunk 0x80034602: the source
    car's part rows, 16 bytes {u8 0, u8 model index, u16 attribute list}, copied for a new model whose hash is
    inserted in the sorted model list 0x3460b; parts of other models restricted to the source by SPECIFICCARNAME get
    copies naming the new car). The game reads it in place of GLOBALB.LZC (kit RECOMP_FILE_OVERLAY, while the
    setting is on): its bundle loader takes the unpacked form under that name (checked 2026-11-20);
  - addoncars.txt: what core.nfsu does at run time - "list <class> <collection> <field> <class> <collection>"
    lines append a car to a game car list (the scripts write slots 75-133 of lists grown to 118/500 with empty
    rows; the lists hold 52-60 cars, so the cars are appended instead), "name <label> <text>" lines give the
    names Ed adds to the language files (CARNAME_<MAKER>_<MODEL>, ...);
  - files.txt: "<mod file>\t<game path>" for the car's files (all new names: CARS/<NAME>/..., CARS/DAMAGE/...,
    SOUND/ENGINE/car_<n>-*.tmx) and "overlay\t<mod file>\t<game path>" for files that replace a game file
    (the Miura's FRONTEND/LOGOTEXTURES.BUN).
Nothing of the game or the mods goes into the repository; <dir> is in the packaged app.
"""
import argparse, copy, re, struct, sys
from pathlib import Path

# ---------------------------------------------------------------------------------------------------- hashes


def jhash(s, init=0xABCDEF00):
    """Bob Jenkins' lookup2 with the attribute system's initval (attrib names, VltEd keys)"""
    k = s.encode('latin1') if isinstance(s, str) else s
    M = 0xffffffff
    a = b = 0x9e3779b9
    c = init
    n = len(k)

    def mix(a, b, c):
        a = (a - b - c) & M; a ^= c >> 13
        b = (b - c - a) & M; b ^= (a << 8) & M
        c = (c - a - b) & M; c ^= b >> 13
        a = (a - b - c) & M; a ^= c >> 12
        b = (b - c - a) & M; b ^= (a << 16) & M
        c = (c - a - b) & M; c ^= b >> 5
        a = (a - b - c) & M; a ^= c >> 3
        b = (b - c - a) & M; b ^= (a << 10) & M
        c = (c - a - b) & M; c ^= b >> 15
        return a, b, c
    i = 0
    while n - i >= 12:
        a = (a + int.from_bytes(k[i:i + 4], 'little')) & M
        b = (b + int.from_bytes(k[i + 4:i + 8], 'little')) & M
        c = (c + int.from_bytes(k[i + 8:i + 12], 'little')) & M
        a, b, c = mix(a, b, c)
        i += 12
    c = (c + n) & M
    r = k[i:]
    for j, sh, which in ((10, 24, 'c'), (9, 16, 'c'), (8, 8, 'c'), (7, 24, 'b'), (6, 16, 'b'), (5, 8, 'b'),
                         (4, 0, 'b'), (3, 24, 'a'), (2, 16, 'a'), (1, 8, 'a'), (0, 0, 'a')):
        if len(r) > j:
            v = r[j] << sh
            if which == 'a': a = (a + v) & M
            elif which == 'b': b = (b + v) & M
            else: c = (c + v) & M
    return mix(a, b, c)[2]


def binhash(s, upper=False):
    """the game's string hash (FUN_005d5cb0 uppercases a-z first; CarTypeInfo names are hashed as they are)"""
    h = 0xffffffff
    for ch in s.encode('latin1'):
        if upper and 0x61 <= ch <= 0x7a:
            ch -= 0x20
        h = (h * 33 + ch) & 0xffffffff
    return h


# ---------------------------------------------------------------------------------------------------- RefPack


def unrefpack(src, pos=0):
    flags, magic = src[pos], src[pos + 1]
    if magic != 0xfb:
        raise ValueError("not a RefPack stream")
    p = pos + 2
    p += 4 if flags & 0x80 else 3
    if flags & 0x01:
        p += 4 if flags & 0x80 else 3
    out = bytearray()
    n = len(src)
    while p < n:
        b0 = src[p]
        if b0 < 0x80:
            b1 = src[p + 1]; p += 2
            lit = b0 & 3; out += src[p:p + lit]; p += lit
            cnt = ((b0 & 0x1c) >> 2) + 3; off = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xc0:
            b1, b2 = src[p + 1], src[p + 2]; p += 3
            lit = b1 >> 6; out += src[p:p + lit]; p += lit
            cnt = (b0 & 0x3f) + 4; off = ((b1 & 0x3f) << 8) + b2 + 1
        elif b0 < 0xe0:
            b1, b2, b3 = src[p + 1], src[p + 2], src[p + 3]; p += 4
            lit = b0 & 3; out += src[p:p + lit]; p += lit
            cnt = ((b0 & 0x0c) << 6) + b3 + 5; off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xfc:
            lit = ((b0 & 0x1f) << 2) + 4; p += 1; out += src[p:p + lit]; p += lit
            continue
        else:
            lit = b0 & 3; p += 1; out += src[p:p + lit]; p += lit
            break
        s = len(out) - off
        if off >= cnt:
            out += out[s:s + cnt]
        else:
            for i in range(cnt):
                out.append(out[s + i])
    return bytes(out)


def refpack(data):
    """RefPack (EA's LZ77, the game's 'RFPK' streams): header 0x10 0xFB + 3-byte length, then the commands
    unrefpack reads - greedy matches through a hash of 3-byte runs (offsets to 128 KiB, lengths to 1028)."""
    n = len(data)
    if n >= 1 << 24:
        raise ValueError('refpack: too long')
    out = bytearray(b'\x10\xfb' + n.to_bytes(3, 'big'))
    head = {}
    i = lit = 0

    def flush(upto):
        nonlocal lit
        while upto - lit >= 4:
            k = min(112, (upto - lit) & ~3)
            out.append(0xe0 + (k >> 2) - 1)
            out.extend(data[lit:lit + k])
            lit += k
    while i + 3 <= n:
        key = data[i:i + 3]
        cand = head.get(key)
        head[key] = i
        best = 0
        if cand is not None and i - cand <= 131072:
            m, lim = 3, min(1028, n - i)
            while m < lim and data[cand + m] == data[i + m]:
                m += 1
            best = m
        off = i - cand if best else 0
        if best >= 3 and not (off > 1024 and best < 4) and not (off > 16384 and best < 5):
            flush(i)
            r = i - lit
            if off <= 1024 and best <= 10:
                out += bytes([((off - 1) >> 3 & 0x60) | ((best - 3) << 2) | r, (off - 1) & 0xff])
            elif off <= 16384 and best <= 67:
                out += bytes([0x80 | (best - 4), (r << 6) | ((off - 1) >> 8), (off - 1) & 0xff])
            else:
                out += bytes([0xc0 | ((off - 1) >> 12 & 0x10) | (((best - 5) >> 8) << 2) | r, ((off - 1) >> 8) & 0xff,
                              (off - 1) & 0xff, (best - 5) & 0xff])
            out.extend(data[lit:i])
            for j in range(i + 1, min(i + best, n - 2)):
                head[data[j:j + 3]] = j
            i += best
            lit = i
        else:
            i += 1
    flush(n)
    r = n - lit
    out.append(0xfc | r)
    out.extend(data[lit:n])
    return bytes(out)


def unjdlz(src, pos=0):
    """JDLZ (the game's other LZ stream: textures of NFS-CarToolkit cars): 'JDLZ' 02 10 00 00, out len, in len"""
    assert src[pos:pos + 4] == b'JDLZ'
    out_len, in_len = struct.unpack_from('<II', src, pos + 8)
    end = pos + in_len
    out = bytearray(out_len)
    ip, op = pos + 16, 0
    f1 = f2 = 1
    while ip < end and op < out_len:
        if f1 == 1:
            f1 = src[ip] | 0x100; ip += 1
        if f2 == 1:
            f2 = src[ip] | 0x100; ip += 1
        if f1 & 1:
            if f2 & 1:
                length = (src[ip + 1] | ((src[ip] & 0xF0) << 4)) + 3
                t = (src[ip] & 0x0F) + 1
            else:
                t = (src[ip + 1] | ((src[ip] & 0xE0) << 3)) + 17
                length = (src[ip] & 0x1F) + 3
            ip += 2
            for i in range(length):
                out[op + i] = out[op + i - t]
            op += length
            f2 >>= 1
        else:
            out[op] = src[ip]; op += 1; ip += 1
        f1 >>= 1
    return bytes(out)


def jdlz(data):
    """JDLZ encoder for unjdlz: tokens are literals or matches - near (distance 1-16, length 3-4098) or far
    (distance 17-2064, length 3-34); the flag bytes (one bit per token, one per match for its kind) are placed
    where the decoder loads them, at the start of a token when its bits ran out"""
    n = len(data)
    toks = []          # (0, byte) or (1 near / 2 far, distance, length)
    head = {}
    i = 0
    while i < n:
        best_len, best_d = 0, 0
        if i + 3 <= n:
            for d in range(1, 17):   # near: short distances, long runs
                if i - d < 0:
                    break
                m = 0
                lim = min(4098, n - i)
                while m < lim and data[i + m] == data[i + m - d]:
                    m += 1
                if m > best_len:
                    best_len, best_d = m, d
            c = head.get(data[i:i + 3])
            if c is not None and 17 <= i - c <= 2064:
                m, lim = 0, min(34, n - i)
                while m < lim and data[c + m] == data[i + m]:
                    m += 1
                if m > best_len and m >= 3:
                    best_len, best_d = m, i - c
        if best_len >= 3:
            toks.append((1 if best_d <= 16 else 2, best_d, best_len))
            for j in range(i, min(i + best_len, n - 2)):
                head[data[j:j + 3]] = j
            i += best_len
        else:
            if i + 3 <= n:
                head[data[i:i + 3]] = i
            toks.append((0, data[i]))
            i += 1
    out = bytearray()
    f1 = f2 = 0        # bits left
    for k, t in enumerate(toks):
        if f1 == 0:
            b = 0
            for j, u in enumerate(toks[k:k + 8]):
                if u[0]:
                    b |= 1 << j
            out.append(b)
            f1 = 8
        if f2 == 0:
            b, j = 0, 0
            for u in toks[k:]:
                if u[0]:
                    if u[0] == 1:
                        b |= 1 << j
                    j += 1
                    if j == 8:
                        break
            out.append(b)
            f2 = 8
        if t[0] == 0:
            out.append(t[1])
        elif t[0] == 1:
            ln, d = t[2] - 3, t[1] - 1
            out += bytes([((ln >> 4) & 0xF0) | d, ln & 0xff])
            f2 -= 1
        else:
            ln, d = t[2] - 3, t[1] - 17
            out += bytes([((d >> 3) & 0xE0) | ln, d & 0xff])
            f2 -= 1
        f1 -= 1
    return b'JDLZ' + struct.pack('<III', 0x1002, n, 16 + len(out)) + bytes(out)


# the texture formats the game builds from a car type's name (nfs.exe strings "%s_SKIN1" ...)
CAR_TEXTURE_SUFFIXES = ('BADGING BADGING_EU BADGING_N D DOOR_HANDLE DRIVER ENGINE ENGINE_N INTERIOR INTERIOR_N '
                        'KIT00_BRAKELIGHT KIT00_DAMAGE KIT00_HEADLIGHT KIT00_BRAKELIGHT_ON KIT00_HEADLIGHT_ON LC '
                        'LICENSE_PLATE LOGO MISC MISC_N NEON RIM RIM_BLUR SIDELIGHT SKIN1 SKIN1_B SKIN2 SKIN3 SKIN4 '
                        'STOCK TREAD TREAD_N WHEEL WHEEL_N WINDOW').split()


def rename_car_files(geo, tex, old, new):
    """a replacement mod's model and textures under another car type's name (2026-11-20, the Corolla: its solids
    and textures are named NIS_240_SX_89_*): every name starting with <old> gets <new> (same length), and every
    bStringHash of such a name - solid keys (0x134011 +16, the 0x134003/0x134004 tables, both kept sorted), texture
    references (0x134012, 0x134902), markers (0x13401d), the texture pack's hash list and entries (each entry's
    TextureInfo, in its JDLZ/RefPack block, re-packed as JDLZ)"""
    assert len(old) == len(new)
    ob, nb = old.encode(), new.encode()
    names = set(m.group().decode() for m in re.finditer(re.escape(ob) + rb'[A-Z0-9_]*', geo))
    names |= {old + '_' + x for x in CAR_TEXTURE_SUFFIXES} | {old}
    G = bytearray(geo)
    T = bytearray(tex)
    # the texture pack's entries: names inside the packed infos
    idx_off, rows_off, nrows = None, None, 0

    def tpk_walk(o, end):
        nonlocal idx_off, rows_off, nrows
        while o + 8 <= end:
            t, sz = struct.unpack_from('<II', T, o)
            if t & 0x80000000:
                tpk_walk(o + 8, o + 8 + sz)
            elif t == 0x33310002:
                idx_off = o
            elif t == 0x33310003:
                rows_off, nrows = o, sz // 24
            o += 8 + sz
    tpk_walk(0, len(T))
    rows = [list(struct.unpack_from('<6I', T, rows_off + 8 + 24 * i)) for i in range(nrows)]
    blobs = []
    for r in rows:
        p = r[1]
        magic, dsize, csize = struct.unpack_from('<III', T, p)
        assert magic == 0x55441122, hex(p)
        body = bytes(T[p + 24:p + csize])
        info = unjdlz(body) if body[:4] == b'JDLZ' else unrefpack(body, 16) if body[:4] == b'RFPK' else body
        nm = info[0x59:0x59 + info[0x58]].split(b'\0')[0].decode('latin1')
        names.add(nm) if nm.startswith(old) else None
        blobs.append([r, bytearray(info), nm])
    hm = {binhash(n): binhash(new + n[len(old):]) for n in names if n.startswith(old)}

    def swap(buf, start, end):
        for q in range(start, end - 3, 4):
            v = struct.unpack_from('<I', buf, q)[0]
            if v in hm:
                struct.pack_into('<I', buf, q, hm[v])

    def geo_walk(o, end):
        while o + 8 <= end:
            t, sz = struct.unpack_from('<II', G, o)
            if t & 0x80000000:
                geo_walk(o + 8, o + 8 + sz)
            elif t == 0x134011:
                swap(G, o + 8 + 16, o + 8 + 20)
            elif t in (0x134003, 0x134004, 0x134012, 0x134902, 0x13401d):
                swap(G, o + 8, o + 8 + sz)
                if t == 0x134003:
                    e = sorted(bytes(G[o + 8 + 8 * i:o + 16 + 8 * i]) for i in range(sz // 8))
                    e.sort(key=lambda b: struct.unpack_from('<I', b)[0])
                    G[o + 8:o + 8 + 8 * len(e)] = b''.join(e)
                if t == 0x134004:
                    e = [bytes(G[o + 8 + 24 * i:o + 32 + 24 * i]) for i in range(sz // 24)]
                    e.sort(key=lambda b: struct.unpack_from('<I', b)[0])
                    G[o + 8:o + 8 + 24 * len(e)] = b''.join(e)
            o += 8 + sz
    geo_walk(0, len(G))
    G = bytearray(re.sub(re.escape(ob) + rb'(?=[_\0])', nb, bytes(G)))
    # the pack: each entry's info renamed and re-packed, laid out again where the first one started
    start = min(r[1] for r in rows)
    data = bytearray()
    for b_ in blobs:
        r, info, nm = b_
        if nm.startswith(old):
            info[0x59:0x59 + len(nm)] = (new + nm[len(old):]).encode()
        swap(info, 0xc, 0x10)
        r[0] = struct.unpack_from('<I', info, 0xc)[0]
        packed = jdlz(bytes(info))
        assert unjdlz(packed) == bytes(info)
        r[1] = start + len(data)
        r[2] = 24 + len(packed)
        data += struct.pack('<6I', 0x55441122, len(info), r[2], 0, 0, 0) + packed
    rows.sort(key=lambda r: r[0])
    for i, r in enumerate(rows):
        struct.pack_into('<6I', T, rows_off + 8 + 24 * i, *r)
        struct.pack_into('<II', T, idx_off + 8 + 8 * i, r[0], 0)
    grow = len(data) - (len(T) - start)
    T = T[:start] + data
    # the chunks that reached the file's end hold the data: their sizes grow with it
    def fix(o, end_old):
        while o + 8 <= len(T):
            t, sz = struct.unpack_from('<II', T, o)
            if o + 8 + sz == end_old:      # this chunk reached the old end: it holds the data
                struct.pack_into('<I', T, o + 4, sz + grow)
                if t & 0x80000000:
                    fix(o + 8, end_old)
                return
            o += 8 + sz
    fix(0, len(tex))
    return bytes(G), bytes(T), len(hm)


def unpack_lzc(B):
    """GLOBAL/GlobalB.lzc: 'RFPK', u32 version, u32 unpacked size, u32 packed size, then one RefPack stream"""
    if B[:4] != b'RFPK':
        return B
    usize = struct.unpack_from('<I', B, 8)[0]
    out = unrefpack(B, 16)
    if len(out) != usize:
        raise ValueError("GlobalB.lzc unpacked to %d bytes, its header says %d" % (len(out), usize))
    return out


# ---------------------------------------------------------------------------------------------------- vaults
CLASS_LOAD, COLL_LOAD = 0x5e970cbc, 0x8e112eb7   # Attrib::ClassLoadData, Attrib::CollectionLoadData
COLLECTION_NAME = 0x9ca1c8f9                      # every class's own-name Text field


class Vault:
    """one vault of a VPAK file: bin (StrE + data) and vlt (Vers StrN DepN DatN ExpN PtrN)"""

    def __init__(s, B, i=0):
        s.B = B
        name_off, s.bs, s.vs, s.bo, s.vo = struct.unpack_from('<5I', B, 16 + 20 * i)
        so = struct.unpack_from('<I', B, 8)[0]
        s.name = B[so + name_off:B.index(b'\0', so + name_off)].decode('latin1')
        s.ch = {}
        o = s.vo
        while o < s.vo + s.vs:
            t, sz = struct.unpack_from('<4sI', B, o)
            s.ch[t[::-1].decode('latin1')] = (o, sz)
            o += sz
        eo = s.ch['ExpN'][0]
        s.exps = [struct.unpack_from('<4I', B, eo + 12 + 16 * j) for j in range(struct.unpack_from('<I', B, eo + 8)[0])]
        po, ps = s.ch['PtrN']
        s.bfix, s.vfix, sec = {}, {}, None
        for j in range((ps - 8) // 12):
            off, kind, idx, dst = struct.unpack_from('<IHHI', B, po + 8 + 12 * j)
            if kind == 2:
                sec = idx
            elif kind == 3:
                (s.bfix if sec == 1 else s.vfix)[off] = dst
        s.str_end = struct.unpack_from('<I', B, s.bo + 4)[0]   # StrE chunk size: bin offsets below it are strings
        # data block starts: layouts, entry data, every non-string pointer target
        starts = set(d for d in s.bfix.values() if d >= s.str_end) | set(s.vfix.values())
        starts.add(s.bs)
        s.starts = sorted(starts)

    def string(s, off):
        return s.B[s.bo + off:s.B.index(b'\0', s.bo + off)].decode('latin1')

    def block_end(s, start):
        import bisect
        i = bisect.bisect_right(s.starts, start)
        return s.starts[i] if i < len(s.starts) else s.bs


class Block:
    """a data block of the new vault's bin: bytes, its alignment (offset mod 16 it wants) and its pointers
    (offset -> ('s', text) for a string, ('b', Block, delta) for data)"""

    def __init__(s, data, align=0):
        s.data = bytearray(data)
        s.align = align
        s.fix = {}


class Coll:
    """a collection of the new vault (or one read from the game's, as a copy source)"""

    def __init__(s):
        s.key = s.cls = s.parent = 0
        s.reserve = s.x = 0
        s.types = []
        s.ents = []          # [key, value, type index, flags, Block or None]
        s.layout = None      # Block
        s.name = ''


class Source:
    """the game's classes and collections (GLOBAL/attributes.bin "db" and GLOBAL/cars_vault.BIN "cars")"""

    def __init__(s, files):
        s.classes, s.colls = {}, {}
        for f in files:
            B = Path(f).read_bytes()
            for i in range(struct.unpack_from('<I', B, 4)[0]):
                v = Vault(B, i)
                for k, t, sz, o in v.exps:
                    if t == CLASS_LOAD:
                        w = struct.unpack_from('<8I', B, v.vo + o)
                        fo = v.vfix[o + 0xc]
                        fields = {}
                        for j in range(w[2]):
                            fk, ft, off, fs, mc, fl = struct.unpack_from('<IIHHHH', B, v.bo + fo + 16 * j)
                            fields[fk] = dict(type=ft, off=off, size=fs, max=mc, flags=fl)
                        s.classes[k] = dict(layout=w[6], fields=fields)
                    elif t == COLL_LOAD:
                        ck, cc = struct.unpack_from('<II', B, v.vo + o)
                        s.colls[(cc, ck)] = (v, o)
        s._memo = {}

    def block(s, v, start):
        key = (id(v), start)
        if key in s._memo:
            return s._memo[key]
        end = v.block_end(start)
        b = Block(v.B[v.bo + start:v.bo + end], start % 16)
        s._memo[key] = b
        for off, dst in v.bfix.items():
            if start <= off < end:
                if dst < v.str_end:
                    b.fix[off - start] = ('s', v.string(dst))
                else:
                    import bisect
                    i = bisect.bisect_right(v.starts, dst) - 1
                    ts = v.starts[i]
                    b.fix[off - start] = ('b', s.block(v, ts), dst - ts)
        return b

    def coll(s, cls, key):
        """the game's collection as a Coll whose blocks are fresh copies"""
        v, o = s.colls[(cls, key)]
        B = v.B
        c = Coll()
        c.key, c.cls, c.parent, c.reserve, c.x, nent = struct.unpack_from('<6I', B, v.vo + o)
        ntypes, talloc = struct.unpack_from('<HH', B, v.vo + o + 0x18)
        c.types = list(struct.unpack_from('<%dI' % talloc, B, v.vo + o + 0x20))[:ntypes]
        s._memo = {}
        lay = v.vfix.get(o + 0x1c)
        c.layout = s.block(v, lay) if lay is not None else None
        eo = o + 0x20 + 4 * talloc
        for j in range(nent):
            k, val, ti, fl = struct.unpack_from('<IIHH', B, v.vo + eo + 12 * j)
            p = v.vfix.get(eo + 12 * j + 4)
            c.ents.append([k, val, ti, fl, s.block(v, p) if p is not None else None])
        return c


def clone_blocks(c):
    """a deep copy of a Coll whose blocks are its own (copy_node from a collection made earlier)"""
    memo = {}

    def cb(b):
        if b is None:
            return None
        if id(b) in memo:
            return memo[id(b)]
        n = Block(b.data, b.align)
        memo[id(b)] = n
        for off, f in b.fix.items():
            n.fix[off] = f if f[0] == 's' else ('b', cb(f[1]), f[2])
        return n
    n = copy.copy(c)
    n.types = list(c.types)
    n.layout = cb(c.layout)
    n.ents = [[e[0], e[1], e[2], e[3], cb(e[4])] for e in c.ents]
    return n


def write_vault(name, colls):
    """a VPAK file with one vault holding colls (in order)"""
    strings = {'': 8}
    blob = bytearray(b'\0')

    def sref(t):
        if t not in strings:
            strings[t] = 8 + len(blob)
            blob.extend(t.encode('latin1') + b'\0')
        return strings[t]
    # bin layout: every block reachable from the collections, each at its alignment
    order, seen = [], set()

    def walk(b):
        if b is None or id(b) in seen:
            return
        seen.add(id(b))
        order.append(b)
        for f in b.fix.values():
            if f[0] == 'b':
                walk(f[1])
            else:
                sref(f[1])
    for c in colls:
        walk(c.layout)
        for e in c.ents:
            walk(e[4])
    stre_size = (8 + len(blob) + 15) & ~15
    stre = b'ErtS' + struct.pack('<I', stre_size) + bytes(blob) + b'\0' * (stre_size - 8 - len(blob))
    bin_ = bytearray(stre)
    where = {}
    for b in order:
        while len(bin_) % 16 != b.align:
            bin_.append(0)
        where[id(b)] = len(bin_)
        bin_ += b.data
    while len(bin_) % 16:
        bin_.append(0)
    bfix = []
    for b in order:
        for off, f in sorted(b.fix.items()):
            at = where[id(b)] + off
            dst = strings[f[1]] if f[0] == 's' else where[id(f[1])] + f[2]
            struct.pack_into('<I', bin_, at, dst)
            bfix.append((at, dst))
    # vlt
    vlt = bytearray()

    def chunk(tag, payload, align=16):
        size = (8 + len(payload) + align - 1) & ~(align - 1)
        vlt.extend(tag[::-1] + struct.pack('<I', size) + payload + b'\0' * (size - 8 - len(payload)))
    chunk(b'Vers', bytes.fromhex('bd6a31ef9483a364'))
    chunk(b'StrN', b'\0' * 8)
    n1, n2 = (name + '.vlt').encode(), (name + '.bin').encode()
    chunk(b'DepN', struct.pack('<5I', 2, jhash(name + '.vlt'), jhash(name + '.bin'), 0, len(n1) + 1) + n1 + b'\0' + n2 + b'\0')
    dat_at = len(vlt)
    dat = bytearray()
    exps, vfix = [], []
    for c in colls:
        while (dat_at + 8 + len(dat)) % 8:
            dat.append(0)
        start = dat_at + 8 + len(dat)
        talloc = len(c.types) + (len(c.types) & 1)
        head = struct.pack('<6IHH', c.key, c.cls, c.parent, max(c.reserve, len(c.ents)), c.x, len(c.ents),
                           len(c.types), talloc)
        lay = where[id(c.layout)] if c.layout is not None else 0
        if c.layout is not None:
            vfix.append((start + 0x1c, lay))
        rec = bytearray(head + struct.pack('<I', lay) + struct.pack('<%dI' % talloc, *(c.types + [0] * (talloc - len(c.types)))))
        for e in c.ents:
            val = e[1]
            if e[4] is not None:
                val = where[id(e[4])]
                vfix.append((start + len(rec) + 4, val))
            rec += struct.pack('<IIHH', e[0], val, e[2], e[3])
        exps.append((c.key, COLL_LOAD, len(rec), start))
        dat += rec
    # padded inside the chunk to 16: the game walks the chunks by their sizes, and loose padding between two
    # (8 zero bytes when DatN ended 8 past a 16) reads as a chunk of size 0 - its loader loops on it for ever
    # (2026-11-20, the boot hang once the Porsche Pack made the vault large)
    chunk(b'DatN', bytes(dat), 16)
    chunk(b'ExpN', struct.pack('<I', len(exps)) + b''.join(struct.pack('<4I', *e) for e in exps))
    ptrs = [(0, 2, 1, 0)] + [(a, 3, 1, d) for a, d in bfix] + [(0, 2, 0, 0)] + [(a, 3, 1, d) for a, d in vfix] + [(0, 0, 0, 0)]
    chunk(b'PtrN', b''.join(struct.pack('<IHHI', *p) for p in ptrs))
    nm = name.encode() + b'\0'
    head = struct.pack('<4sIII', b'VPAK', 1, 0x24, len(nm))
    bin_off = (0x24 + len(nm) + 0x3f) & ~0x3f
    head += struct.pack('<5I', 0, len(bin_), len(vlt), bin_off, bin_off + len(bin_))
    out = bytearray(head + nm)
    out += b'\0' * (bin_off - len(out))
    return bytes(out + bin_ + vlt)


# ---------------------------------------------------------------------------------------------------- scripts
FLOAT = 0x3c16ec5e
TEXT, STRINGKEY, REFSPEC = 0xa3f0c234, 0xa502a824, 0x2b936eb7
U8_TYPES, U16_TYPES = {0x671ecbe2, 0x6f27b5bc, 0x64bec37}, {0xe51a99c1}
GRAPH = {'MinX': 0, 'MaxX': 4, 'MinY': 8, 'MaxY': 12}
MEMBERS = {
    0x34fde6bb: {'X': (0, 'f'), 'Y': (4, 'f'), 'Z': (8, 'f'), 'W': (12, 'f')},      # Vector4
    0x56937aa4: {'X': (0, 'f'), 'Y': (4, 'f')},                                       # Vector2
    REFSPEC: {'Class': (0, 'k'), 'Collection': (4, 'k')},
    0x5fde6463: {'Class': (0, 'k'), 'Collection': (4, 'k')},                          # TurboSND: a reference first
    0xb2d362a1: {m: (o, 'f') for m, o in GRAPH.items()},                             # a curve's range
    0x9df6ff3b: dict([('Front_' + m, (o, 'f')) for m, o in GRAPH.items()] + [('Rear_' + m, (48 + o, 'f')) for m, o in GRAPH.items()]),
    0x4cb36381: {'Front': (0, 'f'), 'Rear': (4, 'f')},
}


def number(t):
    t = t.strip()
    if t.lower() in ('true', 'false'):
        return 1 if t.lower() == 'true' else 0
    if t.lower().startswith('0x'):
        return int(t, 16)
    return float(t) if re.search(r'[.eE]', t) and re.fullmatch(r'[-+0-9.eE]+', t) else int(t) if re.fullmatch(r'[-+]?[0-9]+', t) else None


class Builder:
    def __init__(s, src):
        s.src = src
        s.new = {}            # (class key, coll key) -> Coll
        s.order = []
        s.lists = []          # (list class, list coll, field, class, coll) to append at run time
        s.log = []

    def get(s, cls, key):
        if (cls, key) in s.new:
            return s.new[(cls, key)]
        return None

    def copy_node(s, cls_name, src_name, parent_name, new_name):
        ck = jhash(cls_name)
        # a source given by its key ("0x1234abcd": a game collection whose name isn't known here)
        sk = int(src_name, 16) if src_name.startswith('0x') else jhash(src_name)
        nk = jhash(new_name)
        if (ck, nk) in s.new or (ck, nk) in s.src.colls:
            s.log.append('copy_node %s %s: %s exists already, skipped' % (cls_name, new_name, new_name))
            return
        base = s.new.get((ck, sk))
        if base is not None:
            c = clone_blocks(base)
        elif (ck, sk) in s.src.colls:
            c = s.src.coll(ck, sk)
        else:
            s.log.append('copy_node %s %s: no source %s' % (cls_name, new_name, src_name))
            return
        c.key, c.name = nk, new_name
        if parent_name is not None:
            c.parent = jhash(parent_name)
        s.new[(ck, nk)] = c
        s.order.append(c)
        if COLLECTION_NAME in s.src.classes[ck]['fields']:
            s.set_field(c, ck, COLLECTION_NAME, -1, '', new_name)

    def field(s, ck, fk):
        return s.src.classes[ck]['fields'].get(fk)

    def where(s, c, ck, fk):
        """(Block, offset) of the field's value (or array header), or ('entry', entry) for an inline value"""
        fd = s.field(ck, fk)
        if fd is None:
            return None, None, None
        if fd['flags'] & 2:
            return fd, c.layout, fd['off']
        for e in c.ents:
            if e[0] == fk:
                if e[3] & 0x40:
                    return fd, 'entry', e
                return fd, e[4], 0
        return fd, None, None

    def set_field(s, c, ck, fk, index, member, value):
        fd, blk, off = s.where(c, ck, fk)
        if fd is None or blk is None:
            return 'not in the collection'
        typ = fd['type']
        size = fd['size']
        if fd['flags'] & 1:
            if blk == 'entry':
                return 'inline array'
            alloc, count, esz, tfl = struct.unpack_from('<HHHH', blk.data, off)
            i = max(index, 0)
            if i >= alloc:
                return 'index %d beyond %d' % (i, alloc)
            if i >= count:
                struct.pack_into('<H', blk.data, off + 2, i + 1)
            off = off + (16 if tfl & 0x8000 else 8) + i * esz
            size = esz
        if member:
            mm = MEMBERS.get(typ, {})
            if typ == STRINGKEY and member in ('Collision', 'Visual'):
                return s.put_stringkey(blk, off, value)
            if member not in mm:
                return 'member %s of type %08x unknown' % (member, typ)
            moff, kind = mm[member]
            if kind == 'f':
                v = number(value)
                s.put(blk, off + moff, struct.pack('<f', float(v if v is not None else 0)))
            else:
                v = number(value)
                s.put(blk, off + moff, struct.pack('<I', v if isinstance(v, int) else jhash(value)))
                if typ in (REFSPEC, 0x5fde6463):
                    s.put(blk, off + 8, b'\0' * 4)
            return None
        if typ == TEXT:
            return s.put_text(blk, off, value)
        if typ == STRINGKEY:
            return s.put_stringkey(blk, off, value)
        v = number(value)
        if v is None:
            v = jhash(value)
        if typ == FLOAT or (isinstance(v, float) and size == 4):
            data = struct.pack('<f', float(v))
        elif size == 1:
            data = struct.pack('<B', int(v) & 0xff)
        elif size == 2:
            data = struct.pack('<H', int(v) & 0xffff)
        elif size == 4:
            data = struct.pack('<I', int(v) & 0xffffffff)
        else:
            return 'size %d' % size
        s.put(blk, off, data)
        return None

    def put(s, blk, off, data):
        if blk == 'entry':
            e = off
            v = bytearray(struct.pack('<I', e[1]))
            v[:len(data)] = data
            e[1] = struct.unpack('<I', v)[0]
        else:
            blk.data[off:off + len(data)] = data
            for k in range(len(data)):
                blk.fix.pop(off + k, None)

    def put_text(s, blk, off, text):
        if blk == 'entry':
            return 'inline text'
        blk.data[off:off + 4] = b'\0\0\0\0'
        blk.fix[off] = ('s', text)
        return None

    def put_stringkey(s, blk, off, text):
        if blk == 'entry':
            return 'inline string key'
        blk.data[off:off + 4] = struct.pack('<I', jhash(text))
        blk.data[off + 4:off + 8] = b'\0\0\0\0'
        blk.fix[off + 4] = ('s', text)
        blk.fix.pop(off, None)
        return None

    def resize(s, c, ck, fk, n):
        fd, blk, off = s.where(c, ck, fk)
        if fd is None or blk is None or blk == 'entry' or not fd['flags'] & 1:
            return 'not an array of the collection'
        alloc, count, esz, tfl = struct.unpack_from('<HHHH', blk.data, off)
        if n <= alloc:
            struct.pack_into('<H', blk.data, off + 2, n)
            return None
        if fd['flags'] & 2:
            return 'layout array of %d, %d asked' % (alloc, n)
        head = 16 if tfl & 0x8000 else 8
        blk.data[off + head + alloc * esz:off + head + alloc * esz] = b'\0' * ((n - alloc) * esz)
        struct.pack_into('<HH', blk.data, off, n, n)
        return None

    def run(s, lines, what):
        for raw in lines:
            p = raw.split()
            if not p or p[0].startswith('#'):
                continue
            cmd = p[0]
            try:
                if cmd == 'add_node' and len(p) == 4:
                    # a new collection under <parent>: its layout block starts as the parent's (the values it
                    # would inherit), no entries of its own
                    ck, pk, nk = jhash(p[1]), jhash(p[2]), jhash(p[3])
                    par = s.new.get((ck, pk))
                    par = clone_blocks(par) if par is not None else s.src.coll(ck, pk) if (ck, pk) in s.src.colls else None
                    if par is None or (ck, nk) in s.new or (ck, nk) in s.src.colls:
                        s.log.append('%s: %s: no parent or exists already' % (what, raw.strip()))
                        continue
                    par.key, par.name, par.parent, par.ents = nk, p[3], pk, []
                    s.new[(ck, nk)] = par
                    s.order.append(par)
                    if COLLECTION_NAME in s.src.classes[ck]['fields']:
                        s.set_field(par, ck, COLLECTION_NAME, -1, '', p[3])
                    continue
                if cmd == 'copy_node' and len(p) in (4, 5):
                    if len(p) == 4:
                        s.copy_node(p[1], p[2], None, p[3])
                    else:
                        s.copy_node(p[1], p[2], p[3], p[4])
                    continue
                if cmd in ('update_field', 'resize_field', 'delete_field') and len(p) >= 4:
                    ck, kk = jhash(p[1]), jhash(p[2])
                    m = re.fullmatch(r'([^\[]+)(?:\[(\d+)\])?', p[3])
                    fname, index = m.group(1), int(m.group(2)) if m.group(2) else -1
                    fk = jhash(fname)
                    c = s.get(ck, kk)
                    if c is None:
                        # a game collection: only the car lists are changed, at run time (appending the car)
                        if p[1] == 'car_list' and cmd == 'update_field' and len(p) >= 6 and p[4] == 'Collection':
                            s.lists.append((p[2], fname, p[5]))
                        elif not (p[1] == 'car_list'):
                            s.log.append('%s: %s on a game collection left out' % (what, raw.strip()))
                        continue
                    if cmd == 'delete_field':
                        before = len(c.ents)
                        c.ents = [e for e in c.ents if e[0] != fk]
                        if len(c.ents) == before:
                            s.log.append('%s: %s: no such entry' % (what, raw.strip()))
                        continue
                    if cmd == 'resize_field':
                        err = s.resize(c, ck, fk, int(p[4]))
                    else:
                        member, value = (p[4], p[5]) if len(p) >= 6 else ('', p[4])
                        err = s.set_field(c, ck, fk, index, member, value)
                    if err:
                        s.log.append('%s: %s: %s' % (what, raw.strip(), err))
                    continue
                if cmd in ('resize_field', 'resize_collection', 'add_field', 'update_field', 'delete_field'):
                    s.log.append('%s: %s left out' % (what, raw.strip()))
            except Exception as ex:   # one bad line must not stop the others
                s.log.append('%s: %s: %s' % (what, raw.strip(), ex))


# ---------------------------------------------------------------------------------------------------- GLOBALB
SPECIFIC_CAR = 0x8e73b5dc   # part attribute SPECIFICCARNAME (a car type's name hash)


def chunks(B, start=0, end=None):
    end = len(B) if end is None else end
    o, out = start, []
    while o + 8 <= end:
        t, sz = struct.unpack_from('<II', B, o)
        out.append([t, bytes(B[o + 8:o + 8 + sz])])
        o += 8 + sz
    return out


def pad_chunk(total):
    """a padding chunk (id 0) making `total` bytes a multiple of 0x80 again ([] when it already is)"""
    r = (-total) % 0x80
    if r == 0:
        return []
    if r < 8:
        r += 0x80
    return [[0, b'\0' * (r - 8)]]


def patch_globalb(B, cars, log):
    """cars: dicts with name, template (car type it is copied from), collide (CopyFrom), usage, memtype, skin, paint"""
    top = chunks(B)
    out = []
    for t, d in top:
        size_before = len(d)
        if t == 0x34600:          # CarTypeInfo: 8 bytes of 0x11, then 0x130-byte rows
            rows = [d[8 + 0x130 * i:8 + 0x130 * (i + 1)] for i in range((len(d) - 8) // 0x130)]
            names = [r[:32].split(b'\0')[0].decode() for r in rows]
            add = bytearray()
            for car in cars:
                if car['name'] in names:
                    log.append('car type %s exists already' % car['name'])
                    continue
                r = bytearray(rows[names.index(car['template'])])
                nm = car['name'].encode()
                r[0x00:0x20] = nm.ljust(32, b'\0')
                r[0x20:0x30] = nm.ljust(16, b'\0')
                r[0x30:0x70] = (b'CARS\\' + nm + b'\\GEOMETRY.BIN').ljust(64, b'\0')
                r[0x70:0x80] = nm.ljust(16, b'\0')
                struct.pack_into('<I', r, 0xb0, binhash(car['name']))
                struct.pack_into('<I', r, 0xf0, binhash(car['name']))
                if car.get('usage') is not None:
                    struct.pack_into('<I', r, 0xf4, car['usage'])
                if car.get('memtype'):
                    struct.pack_into('<I', r, 0xf8, binhash(car['memtype']))
                if car.get('skin') is not None:
                    r[0x127] = 1 if car['skin'] else 0
                if car.get('paint'):
                    struct.pack_into('<I', r, 0x12c, binhash(car['paint']))
                add += r
            d = d + bytes(add)
        elif t == 0x8003b900:     # collisions: one 0x3b901 chunk per car type, key jhash(name) at chunk + 0x40
            subs = chunks(d)
            keys = {struct.unpack_from('<I', sd, 0x38)[0]: i for i, (st, sd) in enumerate(subs) if st == 0x3b901}
            for car in cars:
                src = keys.get(jhash(car['collide']))
                if src is None:
                    log.append('collision of %s not found, %s has none' % (car['collide'], car['name']))
                    continue
                sd = bytearray(subs[src][1])
                struct.pack_into('<I', sd, 0x38, jhash(car['name']))
                subs.append([0x3b901, bytes(sd)])
            d = b''.join(struct.pack('<II', st, len(sd)) + sd for st, sd in subs)
        elif t == 0x80034602:     # car parts
            d = patch_parts(d, cars, log)
            for model, parts in EXTRA_PARTS:
                d = parts_add_rows(d, model, parts, log)
        out.append([t, d])
        grow = len(d) - size_before
        if grow:
            out += pad_chunk(grow)
    return b''.join(struct.pack('<II', t, len(d)) + d for t, d in out)


# A template whose part rows don't draw an add-on's model, and one whose rows do (2026-11-20): the Miura copies the
# Gallardo LP560, and with the LP560's 243 rows (its five body kits, T0 damage variants) the car lot showed no model
# at all; the Aventador SVJ's model draws with the Murcielago's rows.
PARTS_TEMPLATE = {}  # tried for the Miura: the Murcielago's rows alone did not draw it
PACK_PLAIN_MODELS = False
TYPE_FIRST = set()   # tried {'LAM_MSV_71'} (index 87 instead of 91): no model either way
SOURCE_SWAP = {}
EXTRA_PARTS = []   # (model, parts) from add-ons' Binary DBModelParts exports, added to both bundles
RENAMED = []   # (old pvehicle key, new key): career rows saved with the old one are moved over by core.nfsu  # tried {'lam_gal_560_09': 'lam_mur_lp_06'} for the Miura: no model either way


def patch_parts(d, cars, log):
    subs = chunks(d)
    ids = [t for t, _ in subs]
    get = lambda t: subs[ids.index(t)][1]
    hdr = list(struct.unpack('<16I', get(0x34603)))
    lists = bytearray(get(0x3460c))
    attrs = [struct.unpack_from('<II', get(0x34605), 8 * i) for i in range(len(get(0x34605)) // 8)]
    models = [struct.unpack_from('<I', get(0x3460b), 4 * i)[0] for i in range(len(get(0x3460b)) // 4)]
    rows = [bytearray(get(0x34604)[16 * i:16 * i + 16]) for i in range(len(get(0x34604)) // 16)]

    def lst(off):
        k = struct.unpack_from('<H', lists, 2 * off)[0]
        return list(struct.unpack_from('<%dH' % k, lists, 2 * off + 2))

    def add_list(a):
        if len(lists) % 2:
            lists.append(0)
        off = len(lists) // 2
        lists.extend(struct.pack('<%dH' % (len(a) + 1), len(a), *a))
        if off > 0xffff:
            raise ValueError("part attribute lists past 64K entries")
        return off
    new_rows = []   # (model hash, row bytes)
    for car in cars:
        if car.get('parts_own'):
            continue       # its own rows come from its Binary export (EXTRA_PARTS)
        h, th = binhash(car['name']), binhash(car.get('parts_from') or PARTS_TEMPLATE.get(car['template'], car['template']))
        if h in models:
            log.append('parts of %s exist already' % car['name'])
            continue
        if th not in models:
            log.append('no parts for template %s' % car['template'])
            continue
        ti = models.index(th)
        own = [r for r in rows if r[1] == ti]
        for r in own:
            new_rows.append((h, bytes(r)))
        # parts of other models only the template may use: copies for the new car
        sc = {i for i, (k, v) in enumerate(attrs) if k == SPECIFIC_CAR and v == th}
        if sc:
            attrs.append((SPECIFIC_CAR, h))
            na = len(attrs) - 1
            memo = {}
            for r in rows:
                if r[1] == ti:
                    continue
                off = struct.unpack_from('<H', r, 2)[0]
                if off == 0xffff:
                    continue
                a = lst(off)
                if not any(x in sc for x in a):
                    continue
                if off not in memo:
                    memo[off] = add_list([na if x in sc else x for x in a])
                nr = bytearray(r)
                struct.pack_into('<H', nr, 2, memo[off])
                new_rows.append((models[r[1]], bytes(nr)))
        models.append(h)
        log.append('parts: %s gets %d rows of %s' % (car['name'], len(own), car.get('parts_from') or PARTS_TEMPLATE.get(car['template'], car['template'])))
    old_models = [struct.unpack_from('<I', get(0x3460b), 4 * i)[0] for i in range(len(get(0x3460b)) // 4)]
    models = sorted(models)
    if len(models) > 256:
        raise ValueError("more than 256 part models")
    idx = {m: i for i, m in enumerate(models)}
    allrows = [(old_models[r[1]], bytes(r)) for r in rows] + new_rows
    allrows.sort(key=lambda mr: idx[mr[0]])     # stable: the game binary-searches rows by model index
    rows_out = bytearray()
    for m, r in allrows:
        r = bytearray(r)
        r[1] = idx[m]
        r[4:16] = b'\0' * 12
        rows_out += r
    if len(lists) % 4:
        lists.extend(b'\0' * (4 - len(lists) % 4))   # zero-length lists: the loader walks them harmlessly
    hdr[8], hdr[10], hdr[14] = len(attrs), len(models), len(allrows)
    rep = {0x34603: struct.pack('<16I', *hdr), 0x3460c: bytes(lists),
           0x34605: b''.join(struct.pack('<II', k, v) for k, v in attrs),
           0x3460b: struct.pack('<%dI' % len(models), *models), 0x34604: bytes(rows_out)}
    return b''.join(struct.pack('<II', t, len(rep.get(t, sd))) + rep.get(t, sd) for t, sd in subs)


# ---------------------------------------------------------------------------------------------------- textures, vinyls


def dds_top(dds):
    """a DDS file's first level: (width, height, fourcc, bytes)"""
    assert dds[:4] == b'DDS '
    h, w = struct.unpack_from('<II', dds, 12)
    four = dds[84:88]
    blk = 8 if four == b'DXT1' else 16
    size = max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * blk
    return w, h, four, dds[128:128 + size]


def tpk_add_textures(B, items):
    """add [(name, dds bytes)] to a texture pack file (a TPK chunk at its start; 2026-11-20, the Honda's logo and
    vinyl thumbnails): its hash list (sorted), its texture infos - plain (0x33310004 infos + 0x33310005 formats,
    pictures in 0x33320002 at the info's offset) or packed (0x33310003 rows, each picture with its info in a
    0x55441122 + JDLZ block at an offset from the file's start) - cloned from one of its own textures of the same
    format, first level only (as the game's own logos and thumbnails are). A name already there is replaced."""
    T = bytearray(B)
    loc = {}

    def walk(o, end):
        while o + 8 <= end:
            t, sz = struct.unpack_from('<II', T, o)
            loc.setdefault(t, (o, sz))
            if t & 0x80000000:
                walk(o + 8, o + 8 + sz)
            o += 8 + sz
    walk(0, len(T))
    hashes = [struct.unpack_from('<I', T, loc[0x33310002][0] + 8 + 8 * i)[0] for i in range(loc[0x33310002][1] // 8)]
    new = [(binhash(n), n, dds_top(d)) for n, d in items]

    def fill(info, h, name, w, hh, pic, off):
        info = bytearray(info)
        nb = name.encode() + b'\0'
        nb += b'\0' * ((-(0x59 + len(nb))) % 4)
        struct.pack_into('<I', info, 0xc, h)
        struct.pack_into('<I', info, 0x14, off)
        struct.pack_into('<I', info, 0x1c, len(pic))
        struct.pack_into('<I', info, 0x24, len(pic))
        struct.pack_into('<HH', info, 0x28, w, hh)
        info[0x2c], info[0x2d] = w.bit_length() - 1, hh.bit_length() - 1
        return info, nb
    if 0x33310004 in loc:
        io, isz = loc[0x33310004]
        infos, p = [], io + 8
        while p < io + 8 + isz:
            n = T[p + 0x58]
            infos.append(bytes(T[p:p + 0x59 + n]))
            p += 0x59 + n
        fo, fsz = loc[0x33310005]
        fmts = [bytes(T[fo + 8 + 24 * i:fo + 32 + 24 * i]) for i in range(fsz // 24)]
        do, dsz = loc[0x33320002]
        data = bytearray(T[do + 8:do + 8 + dsz])
        # infos and formats go together, each info naming its own hash (Binary's re-saved packs, the Miura's
        # LOGOTEXTURES.BUN, keep them in another order than the hash list)
        ents = {struct.unpack_from('<I', inf, 0xc)[0]: [inf, fmts[i]] for i, inf in enumerate(infos)}
        assert sorted(ents) == sorted(hashes)
        for h, name, (w, hh, four, pic) in new:
            ti = next(i for i, f in enumerate(fmts) if f[12:16] == four)
            data += b'\0' * ((-len(data)) % 0x80)
            info, nb = fill(infos[ti][:0x58], h, name, w, hh, pic, len(data))
            data += pic
            ents[h] = [bytes(info) + bytes([len(nb)]) + nb, fmts[ti]]
        order = sorted(ents)
        hl = b''.join(struct.pack('<II', h, 0) for h in order)
        il = b''.join(ents[h][0] for h in order)
        fl = b''.join(ents[h][1] for h in order)
        return tpk_rebuild(T, loc, {0x33310002: hl, 0x33310004: il, 0x33310005: fl, 0x33320002: bytes(data)})
    ro, rsz = loc[0x33310003]
    rows = [list(struct.unpack_from('<6I', T, ro + 8 + 24 * i)) for i in range(rsz // 24)]
    blobs = {}
    for r in rows:
        # an entry is one or more 0x55441122 pieces (a big picture in 0x8000-byte pieces with its info last);
        # copied whole
        blobs[r[0]] = [r, bytes(T[r[1]:r[1] + r[2]])]
    for h, name, (w, hh, four, pic) in new:
        tmpl = None
        for r in rows:
            if struct.unpack_from('<I', T, r[1] + 4)[0] != r[3]:
                continue        # several pieces
            body = blobs[r[0]][1][24:]
            info = unjdlz(body) if body[:4] == b'JDLZ' else unrefpack(body, 16) if body[:4] == b'RFPK' else body
            if info[0x80:0x100].find(four) >= 0 and r[3] - 0x100 >= 0:
                tmpl = (r, info[:0x100])
                break
        r0, head = tmpl
        info, nb = fill(head[:0x58], h, name, w, hh, pic, 0)
        hd = bytearray(head)
        hd[:0x58] = info
        hd[0x58] = len(nb)
        hd[0x59:0x59 + len(nb)] = nb
        fpos = head.find(four, 0x80)
        assert 0x59 + len(nb) <= fpos - 12, name
        packed = jdlz(bytes(hd) + pic)
        blk = struct.pack('<6I', 0x55441122, 0x100 + len(pic), 24 + len(packed), 0, 0, 0) + packed
        blobs[h] = [[h, 0, len(blk), 0x100 + len(pic), r0[4], r0[5]], blk]
    order = sorted(blobs)
    # the pictures keep the order they had; new ones go last
    seq = sorted(blobs, key=lambda h: (blobs[h][0][1] == 0, blobs[h][0][1]))
    hl = b''.join(struct.pack('<II', h, 0) for h in order)
    rl_len = 24 * len(order)
    # where the data will start once the info part has grown: computed by the rebuild, then the offsets set
    out = tpk_rebuild(T, loc, {0x33310002: hl, 0x33310003: b'\0' * rl_len, 0x33320002: b''})
    o2 = {}

    def walk2(o, end):
        while o + 8 <= end:
            t, sz = struct.unpack_from('<II', out, o)
            o2.setdefault(t, (o, sz))
            if t & 0x80000000:
                walk2(o + 8, o + 8 + sz)
            o += 8 + sz
    walk2(0, len(out))
    base = o2[0x33320002][0] + 8
    first = min(r[1] for r in rows)
    pad = first - (loc[0x33320002][0] + 8)    # the game's own gap before the first picture
    data = bytearray(b'\x11' * pad)
    for h in seq:
        r, blk = blobs[h]
        r[1] = base + len(data)
        data += blk
    rl = b''.join(struct.pack('<6I', *blobs[h][0]) for h in order)
    return tpk_rebuild(T, loc, {0x33310002: hl, 0x33310003: rl, 0x33320002: bytes(data)})


def tpk_rebuild(T, loc, rep):
    """the pack with some leaf chunks' contents replaced; every parent's size follows, and the zero chunk before
    the picture section (0xb3320000) grows so that the section stays 0x100-aligned"""
    def build(o, end, base, pad):
        out = bytearray()
        kids = []
        while o + 8 <= end:
            t, sz = struct.unpack_from('<II', T, o)
            kids.append((o, t, sz))
            o += 8 + sz
        for i, (o, t, sz) in enumerate(kids):
            if t & 0x80000000:
                body = build(o + 8, o + 8 + sz, base + len(out) + 8, pad)
            elif t == 0 and i + 1 < len(kids) and kids[i + 1][1] == 0xb3320000 and pad is not None:
                body = b'\0' * pad
            else:
                body = rep.get(t, bytes(T[o + 8:o + 8 + sz]))
            if t == 0xb3320000:
                build.at = base + len(out)
            out += struct.pack('<II', t, len(body)) + body
        return bytes(out)
    build.at = None
    first = build(0, len(T), 0, None)
    # the original pad: the zero chunk right before 0xb3320000
    pad0 = None
    o = 8
    t0, s0 = struct.unpack_from('<II', T, 0)
    while o + 8 <= 8 + s0:
        t, sz = struct.unpack_from('<II', T, o)
        nt = struct.unpack_from('<I', T, o + 8 + sz)[0] if o + 8 + sz + 4 <= len(T) else None
        if t == 0 and nt == 0xb3320000:
            pad0 = sz
        o += 8 + sz
    if pad0 is None or build.at is None:
        return first
    pad = pad0 + (-build.at) % 0x100
    return build(0, len(T), 0, pad)


def vinyls_add(B, chunks_):
    """CARS/VINYLS/VINYLS.BIN: vector vinyl chunks (0x8003ce00, key = the vinyl's name hash at +0x20), each
    starting on a 0x800 boundary behind a zero chunk; a key already there is replaced (Binary's import override)"""
    keys = {struct.unpack_from('<I', c, 0x20)[0]: c for c in chunks_}
    out = bytearray()
    o = 0
    while o + 8 <= len(B):
        t, sz = struct.unpack_from('<II', B, o)
        if t == 0x8003ce00 and struct.unpack_from('<I', B, o + 0x20)[0] in keys:
            pass        # replaced below
        elif t == 0x8003ce00:
            if len(out) % 0x800:
                out += vinyl_pad(len(out))
            out += B[o:o + 8 + sz]
        o += 8 + sz
    for c in keys.values():
        out += vinyl_pad(len(out))
        out += c
    return bytes(out)


def vinyl_pad(at):
    n = (-at) % 0x800
    if n == 0:
        return b''
    if n < 8:
        n += 0x800
    return struct.pack('<II', 0, n - 8) + b'\0' * (n - 8)


# Binary's DBModelParts export ("gMpB" + RAWW, 2026-11-20, the Honda's VECTORVINYL.bin): each attribute's value as
# Binary writes it, and as the game keeps it (8-byte key/value pairs; text as bStringHash or as u16 offsets/4 into
# the parts' string table 0x34606). Checked against the game's own VECTORVINYL rows (MANUFACTURER_NISSAN,
# NIS_370 2TONE1).
PART_TYPES = {0x927097f6: 'u32', 0xd90271fb: 'u32', 0xd902763c: 'u32', 0xd9027a7d: 'u32', 0xd9027ebe: 'u32',
              0xcdab2874: 'u8', 0x6b66d0e0: 'u8', 0x6bfa56da: 'u8', 0x9239cf16: 'u16',
              0x29008b14: 'hash', 0x900449d3: 'hash', 0xebb03e66: 'hash', 0x6223c6f9: 'hash', 0x65f58556: 'hash',
              0x04b39858: 'hash', 0x5412a1d9: 'hash', 0x8e73b5dc: 'hash', 0xb5548ed7: 'hash',
              0x7d65a926: 'str', 0xbb318b8f: 'str2',
              # a car's own parts (the Trueno's export, checked against the game's car rows the same way)
              0xd90f9423: 'u32', 0x643dabeb: 'u32', 0x2850a03b: 'u32', 0x46b79643: 'str', 0xfe613b98: 'str2', 0x000004b8: 'hash'}


def binary_parts(raw):
    """(model name, [[(key, kind, value)]]) from a Binary DBModelParts export"""
    assert raw[:4] == b'gMpB'
    p = 0x14
    if raw[p:p + 4] == b'RAWW':
        n = struct.unpack_from('<I', raw, p + 8)[0]
        d = raw[p + 16:p + 16 + n]
    elif raw[p:p + 4] == b'JDLZ':
        d = unjdlz(raw, p)
    else:
        raise ValueError('unknown block %r' % raw[p:p + 4])
    e = d.index(b'\0')
    model, q = d[:e].decode(), e + 1
    nparts = struct.unpack_from('<I', d, q)[0]; q += 4

    def text(q):
        e = d.index(b'\0', q)
        return d[q:e].decode('latin1'), e + 1
    parts = []
    for _ in range(nparts):
        na = struct.unpack_from('<I', d, q)[0]; q += 4
        at = []
        for _ in range(na):
            k = struct.unpack_from('<I', d, q)[0]; q += 4
            kind = PART_TYPES[k]
            if kind == 'u32':
                v = struct.unpack_from('<I', d, q)[0]; q += 4
            elif kind == 'u16':
                v = struct.unpack_from('<H', d, q)[0]; q += 2
            elif kind == 'u8':
                v = d[q]; q += 1
            elif kind == 'hash':
                v, q = text(q)
            elif kind == 'str':
                has = d[q]; q += 1
                v = None
                if has:
                    v, q = text(q)
            else:
                h1, h2 = d[q], d[q + 1]; q += 2
                a_ = b_ = None
                if h1:
                    a_, q = text(q)
                if h2:
                    b_, q = text(q)
                v = (a_, b_)
            at.append((k, kind, v))
        parts.append(at)
    assert q == len(d), (q, len(d))
    return model, parts


def parts_add_rows(d, model, parts, log):
    """the car parts chunk (0x80034602) with these parts added under <model> (an existing part model)"""
    subs = chunks(d)
    ids = [t for t, _ in subs]
    get = lambda t: subs[ids.index(t)][1]
    hdr = list(struct.unpack('<16I', get(0x34603)))
    S = bytearray(get(0x34606))
    lists = bytearray(get(0x3460c))
    attrs = [struct.unpack_from('<II', get(0x34605), 8 * i) for i in range(len(get(0x34605)) // 8)]
    aidx = {a: i for i, a in enumerate(attrs)}
    models = [struct.unpack_from('<I', get(0x3460b), 4 * i)[0] for i in range(len(get(0x3460b)) // 4)]
    rows = [bytes(get(0x34604)[16 * i:16 * i + 16]) for i in range(len(get(0x34604)) // 16)]
    if binhash(model) not in models:
        # a car's own part rows (the Trueno's): a new model, the table re-sorted (the game binary-searches rows
        # by model index)
        old = models[:]
        models = sorted(models + [binhash(model)])
        if len(models) > 256:
            raise ValueError('more than 256 part models')
        idx = {m: i for i, m in enumerate(models)}
        rows = sorted((bytes([r[0], idx[old[r[1]]]]) + r[2:] for r in rows), key=lambda r: r[1])
    mi = models.index(binhash(model))

    def sref(t):
        if t is None:
            return 0xffff
        b = t.encode('latin1') + b'\0'
        i = bytes(S).find(b)
        while i >= 0 and (i % 4 or (i and S[i - 1] != 0)):
            i = bytes(S).find(b, i + 1)
        if i < 0:
            S.extend(b'\0' * ((-len(S)) % 4))
            i = len(S)
            S.extend(b)
        S.extend(b'\0' * ((-len(S)) % 4))
        if i // 4 > 0xfffe:
            raise ValueError('part strings past 256 KiB')
        return i // 4

    def hval(t):
        return int(t, 16) if t.startswith('0x') else binhash(t)
    new = []
    for at in parts:
        ix = []
        for k, kind, v in at:
            if kind in ('u32', 'u16', 'u8'):
                val = v
            elif kind == 'hash':
                val = hval(v)
            elif kind == 'str':
                val = sref(v) if v is not None else 0xffff
            else:
                val = sref(v[0]) | (sref(v[1]) << 16)
            a = (k, val)
            if a not in aidx:
                aidx[a] = len(attrs)
                attrs.append(a)
            ix.append(aidx[a])
        if len(lists) % 2:
            lists.append(0)
        off = len(lists) // 2
        lists.extend(struct.pack('<%dH' % (len(ix) + 1), len(ix), *ix))
        if off > 0xffff:
            raise ValueError('part attribute lists past 64K entries')
        new.append(bytes([0, mi]) + struct.pack('<H', off) + b'\0' * 12)
    last = max((i for i, r in enumerate(rows) if r[1] <= mi), default=-1)
    rows[last + 1:last + 1] = new
    log.append('parts: %d %s rows added' % (len(new), model))
    if len(lists) % 4:
        lists.extend(b'\0' * (4 - len(lists) % 4))
    hdr[8], hdr[10], hdr[14] = len(attrs), len(models), len(rows)
    rep = {0x34603: struct.pack('<16I', *hdr), 0x3460c: bytes(lists), 0x34606: bytes(S),
           0x3460b: struct.pack('<%dI' % len(models), *models),
           0x34605: b''.join(struct.pack('<II', k, v) for k, v in attrs), 0x34604: b''.join(rows)}
    return b''.join(struct.pack('<II', t, len(rep.get(t, sd))) + rep.get(t, sd) for t, sd in subs)


# ---------------------------------------------------------------------------------------------------- mods


def pack_geometry(B):
    """A car model whose solids are stored plain (0x80134010 chunks straight in the solid list, as NFS-CarToolkit
    2019 wrote the Miura's) rewritten the way every playable car of the game and every add-on that draws stores
    them: each solid in a 0x55441122 block - {id, size = the solid chunk's length, stride to the next block,
    destination offset 0, 0, 0} then a compressed stream, here 'RAWW' (the game's decompressor copies it: magic,
    version 1, header 0x10, length, length + 16). Blocks start 0x80-aligned behind padding chunks (type 0), as
    the add-ons' do. None returned when the model has no plain solid (2026-11-20, the car lot drew no Miura)."""
    t, sz = struct.unpack_from('<II', B, 0)
    if t != 0x80134000:
        return None
    kids, o = [], 8
    while o + 8 <= 8 + sz:
        ct, cs = struct.unpack_from('<II', B, o)
        kids.append((ct, B[o:o + 8 + cs]))
        o += 8 + cs
    if not any(ct == 0x80134010 for ct, _ in kids):
        return None
    # where each plain solid starts in the file (the 0x134004 table names them by these offsets)
    old_at, o = {}, 8
    for ct, raw in kids:
        if ct == 0x80134010:
            old_at[o] = len(old_at)
        o += len(raw)
    out = bytearray(B[:8])
    new_at = []   # per solid: (offset, stored length, length)
    info_at = None

    def pad(to):
        need = (-(len(out) + 8)) % to
        out.extend(struct.pack('<II', 0, need) + b'\0' * need)
    for ct, raw in kids:
        if ct == 0:
            continue   # the old padding; laid out again below
        if ct != 0x80134010:
            if ct != 0x80134001 and len(out) % 16:
                pad(16)
            if ct == 0x80134001:
                info_at = len(out)
            out += raw
            continue
        # as the game's own cars (LAM_MUR_LP_06): the solid cut into 0x8000-byte pieces, each RefPack-compressed in
        # its own block {id, piece length, stride, where the piece goes in the solid, this block's distance from the
        # solid's first block, 0} + 'RFPK' {version 1, header 0x10, piece length, stream length}; the piece at 0
        # (the solid's own header) comes last; the run starts 0x80-aligned
        pad(0x80)
        first = len(out)
        pieces = [(d, raw[d:d + 0x8000]) for d in range(0, len(raw), 0x8000)]
        pieces = pieces[1:] + pieces[:1]
        for d, piece in pieces:
            stream = refpack(piece)
            body = b'RFPK' + bytes([1, 0x10, 0, 0]) + struct.pack('<II', len(piece), len(stream)) + stream
            stride = (24 + len(body) + 3) & ~3
            out += struct.pack('<6I', 0x55441122, len(piece), stride, d, len(out) - first, 0) + body
            out += b'\0' * (stride - 24 - len(body))
        new_at.append((first, len(out) - first, len(raw)))
    while len(out) % 16:
        out.append(0)
    struct.pack_into('<I', out, 4, len(out) - 8)
    # the solid list header's table (0x134004: per solid {key, offset, stored length, length, flags, 0}; flags 0x200
    # = packed) follows the new layout, or the game reads the solids where they no longer are
    q, end = info_at + 8, info_at + 8 + struct.unpack_from('<I', out, info_at + 4)[0]
    while q + 8 <= end:
        ct, cs = struct.unpack_from('<II', out, q)
        if ct == 0x134004:
            for i in range(cs // 24):
                e = q + 8 + 24 * i
                k = old_at.get(struct.unpack_from('<I', out, e + 4)[0])
                if k is None:
                    raise ValueError('solid table entry %d names no solid' % i)
                off, stored, n = new_at[k]
                struct.pack_into('<IIII', out, e + 4, off, stored, n, 0x200)
        q += 8 + cs
    return bytes(out)


def add_strings(B, names):
    """a language file with `names` (label hash -> text) added to its first string chunk (0x39000: count,
    entries offset, strings offset, a 16-byte name, (hash, offset) rows sorted by hash, then the texts); labels it
    has already keep their text. Chunks after it are kept 16-aligned with a padding chunk (type 0)."""
    out, o, done = bytearray(), 0, False
    while o + 8 <= len(B):
        t, sz = struct.unpack_from('<II', B, o)
        body = B[o + 8:o + 8 + sz]
        if t == 0x39000 and not done:
            done = True
            n, eo, so = struct.unpack_from('<III', body, 0)
            rows = [struct.unpack_from('<II', body, eo + 8 * i) for i in range(n)]
            texts = bytearray(body[so:])
            have = {h for h, _ in rows}
            for h, txt in names.items():
                if h not in have:
                    rows.append((h, len(texts)))
                    texts += (txt if isinstance(txt, bytes) else txt.encode('latin1', 'replace')) + b'\0'
            rows.sort()
            while len(texts) % 4:
                texts.append(0)
            head = struct.pack('<III', len(rows), eo, eo + 8 * len(rows)) + body[12:eo]
            body = head + b''.join(struct.pack('<II', h, off) for h, off in rows) + bytes(texts)
            out += struct.pack('<II', t, len(body)) + body
            # realign what follows the way the file had it
            nxt = o + 8 + sz
            while nxt + 8 <= len(B) and struct.unpack_from('<I', B, nxt)[0] == 0:
                nxt += 8 + struct.unpack_from('<I', B, nxt + 4)[0]
            if nxt < len(B):
                pad = (16 - (len(out) + 8) % 16) % 16
                out += struct.pack('<II', 0, pad) + b'\0' * pad
            o = nxt
            continue
        out += B[o:o + 8 + sz]
        o += 8 + sz
    out += B[o:]
    return bytes(out)


def ini(path):
    out = {}
    sec = ''
    for line in Path(path).read_text(errors='replace').splitlines():
        line = line.split('//')[0].strip()
        if line.startswith('['):
            sec = line.strip('[]').upper()
        elif '=' in line:
            k, v = line.split('=', 1)
            out[(sec, k.strip().lower())] = v.strip()
    return out


def find_cars(mods):
    """each add-on car folder: its scripts (in install order), Ed config, files"""
    cars = []
    for d in sorted(p for p in Path(mods).iterdir() if p.is_dir()):
        # Ed's configs: "Ed Config*/..." or, as the Marussia B2 ships them, "CONFIG/NFSUC/<CAR>.ini"
        cfg = list(d.glob('Ed Config*/**/*.ini')) or list(d.glob('CONFIG/**/*.ini'))
        group = 'porsche' if 'porsche pack' in d.name.lower() else 'addon'
        if len(cfg) > 1 and (d / 'CARS').is_dir():
            # a pack (2026-11-20, NFS Undercover Porsche Pack): one script for every car, each car's part headed by
            # a "/// <name> ///" line, one Ed config per car named after its folder (CARS/<NAME>)
            by_name = {c.stem.upper(): c for c in cfg}
            lines = []
            for f in sorted(d.glob('*.nfsms')):
                if 'carlistresize' not in f.name.lower():
                    lines += f.read_text(errors='replace').splitlines()
            blocks, cur = [], None
            for ln in lines:
                if ln.strip().startswith('///'):
                    cur = []
                    blocks.append(cur)
                elif cur is not None:
                    cur.append(ln)
            shared = [(f, str(f.relative_to(d))) for f in d.rglob('*') if f.is_file() and f.parts[len(d.parts)] == 'SOUND']
            for blk in blocks:
                visual = next((ln.split()[5] for ln in blk if ln.split()[:2] == ['update_field', 'pvehicle'] and
                               len(ln.split()) == 6 and ln.split()[3:5] == ['MODEL', 'Visual']), None)
                if not visual or visual.upper() not in by_name:
                    continue
                nm = visual.upper()
                files = [(f, str(f.relative_to(d))) for f in d.rglob('*') if f.is_file() and len(f.parts) > len(d.parts) + 2
                         and f.parts[len(d.parts)] == 'CARS' and f.parts[len(d.parts) + 1].upper() in (nm,)
                         and not f.name.lower().endswith('.bak')]
                files += [(f, str(f.relative_to(d))) for f in (d / 'CARS' / 'DAMAGE').glob('*')
                          if f.is_file() and f.stem.upper() == nm]
                cars.append(dict(dir=d, ini=ini(by_name[nm]), script_lines=blk, scripts=[], files=files + shared,
                                 overlays=[], group=group, label=nm))
                shared = []
            continue
        if cfg and (d / 'CARS').is_dir():
            scripts = [f for f in sorted(d.glob('*.nfsms')) if 'carlistresize' not in f.name.lower()]
            files = [(f, str(f.relative_to(d))) for f in d.rglob('*') if f.is_file() and f.parts[len(d.parts)] in ('CARS', 'SOUND')
                     and not f.name.lower().endswith('.bak')]
            cars.append(dict(dir=d, ini=ini(cfg[0]), scripts=scripts, files=files, overlays=[], group=group))
        elif (d / 'VLTInstaller.nfsms').is_file() and (d / 'DRAGnDrop').is_dir():
            # VltEd script + Binary end script + files to drop into the game (2026-11-21, the Toyota Corolla Trueno
            # and Lexus GS430 mods): DRAGnDrop/CARS and SOUND go into the game; the .end names the texts, where the
            # car type, collision and part rows are copied from, or its own part rows (a Binary export)
            dd = d / 'DRAGnDrop'
            files = [(f, str(f.relative_to(dd))) for f in dd.rglob('*') if f.is_file() and f.parts[len(dd.parts)] in ('CARS', 'SOUND')]
            labels, parts_from, collide, cartype_from, parts_own = [], None, None, None, None
            for e in sorted(d.rglob('*.end')):
                for line in e.read_text(errors='replace').splitlines():
                    m = re.match(r'add(?:_or_update)?_string\s+LANGUAGES\\ENGLISH_GLOBAL\.BIN\s+\S+\s+\S+\s+\S+\s+(CARNAME_\S+)\s+"([^"]*)"', line.strip(), re.I)
                    if m:
                        labels.append((m.group(1), m.group(2)))
                    q = line.split()
                    if len(q) == 5 and q[0] == 'copy_collection':
                        if q[2] == 'DBModelParts':
                            parts_from = q[3]
                        elif q[2] == 'Collisions':
                            collide = q[3]
                        elif q[2] == 'CarTypeInfos':
                            cartype_from = q[3]
                    if len(q) == 5 and q[0] == 'import' and q[3] == 'DBModelParts' and (e.parent / q[4].strip('"')).is_file():
                        parts_own = e.parent / q[4].strip('"')
            # the full name first (it is what the dealer row shows), short names after
            labels.sort(key=lambda x: x[0].endswith('_SHORT'))
            cars.append(dict(dir=d, ini={}, scripts=[d / 'VLTInstaller.nfsms'], files=files, overlays=[], group=group,
                             labels=labels, parts_from=parts_from, collide=collide, cartype_from=cartype_from,
                             parts_own=parts_own))
        elif (d / 'Addon' / 'Installation.nfsms').is_file():
            a = d / 'Addon'
            scripts = []
            for line in (a / 'Installation.nfsms').read_text(errors='replace').splitlines():
                p = line.split()
                if len(p) == 2 and p[0] == 'script' and 'readme' not in p[1].lower():
                    f = a / p[1].replace('\\', '/')
                    if 'files' not in f.name.lower():
                        scripts.append(f)
            files, overlays = [], []
            fl = a / 'Addon_Files' / 'Files.nfsms'
            if fl.is_file():
                for line in fl.read_text(errors='replace').splitlines():
                    p = line.split()
                    if len(p) == 3 and p[0] == 'file_copy':
                        src, dst = a / 'Addon_Files' / p[1], p[2].replace('\\', '/')
                        (overlays if dst.upper().startswith('FRONTEND/') else files).append((src, dst))
                        # its damage file under its own car's name too (the Miura's installer names it after
                        # another car, LAM_SESTO_10; the game looks for CARS/DAMAGE/<car>.damage)
                        if dst.upper().startswith('CARS/DAMAGE/') and src.suffix.lower() == '.damage':
                            files.append((src, 'CARS/DAMAGE/' + src.name.upper().replace('.DAMAGE', '.damage')))
            cars.append(dict(dir=d, ini={}, scripts=scripts, files=files, overlays=overlays, group=group))
        else:
            # a VltEd install script beside a Binary end script (2026-11-20, the Honda NSX-R GT): "script <file>"
            # lines name its parts, one of them copying the model with "file_copy <file> CARS\\<NAME>\\..."; the
            # .end script (Binary) names the car's text and where its GLOBALB part rows and collision come from
            for top in sorted(d.rglob('*.nfsms')):
                t = top.read_text(errors='replace')
                if not re.search(r'^game\s+UC', t, re.M):
                    continue
                scripts, files = [], []
                for line in t.splitlines():
                    q = line.split()
                    if len(q) == 2 and q[0] == 'script':
                        f = top.parent / q[1].replace('\\', '/')
                        if not f.is_file():
                            continue
                        ft = f.read_text(errors='replace')
                        cp = [x.split() for x in ft.splitlines() if x.split()[:1] == ['file_copy'] and len(x.split()) == 3]
                        if cp:
                            files += [(f.parent / a_, b_.replace('\\', '/')) for _, a_, b_ in cp]
                        elif 'copy_node' in ft or 'update_field' in ft:
                            scripts.append(f)
                if not scripts or not any(b_.upper().startswith('CARS/') and b_.upper().endswith('/GEOMETRY.BIN') for _, b_ in files):
                    continue
                # a car that REPLACES a game car (2026-11-21, the two Ladas: their model goes into CARS\FOR_FOC_ST_07
                # and CARS\NIS_SYL_AER_99, no pvehicle of their own): made a car of its own (CONVERT), never
                # copied over the game's car
                all_lines = [ln for f in scripts for ln in f.read_text(errors='replace').splitlines()]
                if not any(ln.split()[:2] == ['copy_node', 'pvehicle'] for ln in all_lines):
                    geo = next((a_ for a_, b_ in files if b_.upper().endswith('/GEOMETRY.BIN')), None)
                    tex = next((a_ for a_, b_ in files if b_.upper().endswith('/TEXTURES.BIN')), None)
                    stock = next(b_.split('/')[1].upper() for _, b_ in files if b_.upper().endswith('/GEOMETRY.BIN'))
                    if stock in CONVERT and geo and tex:
                        cars.append(dict(dir=d, ini={}, scripts=[], files=[], overlays=[], group=group,
                                         convert=dict(stock=stock, geo=geo, tex=tex, lines=all_lines)))
                    break
                labels, parts_from, collide = [], None, None
                # what the .end adds to game files (2026-11-20, the Honda's logo and vinyls): textures into packs,
                # vector vinyls, Binary part exports, more strings
                extras = dict(textures={}, vinyls=[], parts=[], strings=[])
                for e in sorted(d.rglob('*.end')):
                    for line in e.read_text(errors='replace').splitlines():
                        m = re.match(r'add_or_update_string\s+LANGUAGES\\ENGLISH_GLOBAL\.BIN\s+\S+\s+\S+\s+\S+\s+(\S+)\s+"([^"]*)"', line.strip())
                        if m and m.group(1).startswith('CARNAME_') and m.group(1) not in [l_ for l_, _ in labels]:
                            labels.append((m.group(1), m.group(2)))
                        elif m and m.group(1) not in [l_ for l_, _ in extras['strings']]:
                            extras['strings'].append((m.group(1), m.group(2)))
                        m = re.match(r'add_or_replace_texture\s+(\S+)\s+TPKBlocks\s+\S+\s+(\S+)\s+\S+\s+"([^"]+)"', line.strip())
                        if m:
                            f_ = e.parent / m.group(3).replace('\\', '/')
                            lst_ = extras['textures'].setdefault(m.group(1).replace('\\', '/').upper(), [])
                            if f_.is_file() and m.group(2) not in [n_ for n_, _ in lst_]:
                                lst_.append((m.group(2), f_))
                        m = re.match(r'import\s+\S+\s+CARS\\VINYLS\\VINYLS\.BIN\s+VectorVinyls\s+"([^"]+)"', line.strip(), re.I)
                        if m and (e.parent / m.group(1).replace('\\', '/')).is_file():
                            extras['vinyls'].append(e.parent / m.group(1).replace('\\', '/'))
                        m = re.match(r'import\s+\S+\s+GLOBAL\\GLOBALB\.LZC\s+DBModelParts\s+"([^"]+)"', line.strip(), re.I)
                        if m and (e.parent / m.group(1).replace('\\', '/')).is_file():
                            extras['parts'].append(e.parent / m.group(1).replace('\\', '/'))
                        q = line.split()
                        if len(q) == 5 and q[0] == 'copy_collection' and q[2] == 'DBModelParts':
                            parts_from = q[3]
                        if len(q) == 5 and q[0] == 'copy_collection' and q[2] == 'Collisions':
                            collide = q[3]
                cars.append(dict(dir=d, ini={}, scripts=scripts, files=[(a_, b_) for a_, b_ in files if a_.is_file()],
                                 overlays=[], group=group, labels=labels, parts_from=parts_from, collide=collide,
                                 extras=extras))
                break
            else:
                # a mod that REPLACES a game car (2026-11-20, the Corolla: GEOMETRY.BIN and TEXTURES.BIN beside a
                # script that edits "ecar <the game's car>"), made a car of its own (CONVERT names it)
                scr = [f for f in sorted(d.glob('*.nfsms'))]
                lines = scr[0].read_text(errors='replace').splitlines() if scr else []
                stock = next((ln.split()[2].upper() for ln in lines if ln.split()[:2] == ['update_field', 'ecar']), None)
                if (d / 'GEOMETRY.BIN').is_file() and (d / 'TEXTURES.BIN').is_file() and stock in CONVERT:
                    cars.append(dict(dir=d, ini={}, scripts=[], files=[], overlays=[], group=group,
                                     convert=dict(stock=stock, geo=d / 'GEOMETRY.BIN', tex=d / 'TEXTURES.BIN', lines=lines)))
    return cars



def src_value(src, cls, coll, field):
    """a game collection's own (or its parents') 32-bit value of a field, or None"""
    ck, fk = jhash(cls), jhash(field)
    fd = src.classes[ck]['fields'][fk]
    key = jhash(coll)
    while (ck, key) in src.colls:
        c = src.coll(ck, key)
        for e in c.ents:
            if e[0] == fk:
                v = e[1] if e[4] is None else struct.unpack_from('<I', e[4].data, 0)[0]
                return v & ((1 << (8 * min(4, fd['size']))) - 1)
        if c.layout is not None and (fd['flags'] & 2) and len(c.layout.data) >= fd['off'] + 4:
            v = struct.unpack_from('<I', c.layout.data, fd['off'])[0]
            if v:
                return v & ((1 << (8 * min(4, fd['size']))) - 1)
        if not c.parent or c.parent == key:
            break
        key = c.parent
    return None


def convert_car(car, a, src, log):
    """a replacement mod made a car of its own (see CONVERT): its model and textures renamed after the new car type,
    collections copied from the car it replaced with its own edits, a frontend under its real maker"""
    cv = car['convert']
    stock = cv['stock']
    spec = CONVERT[stock]
    new, fe_name, maker, maker_from, title = spec['new'], spec['fe'], spec['maker'], spec['maker_from'], spec['title']
    if stock not in CAR_TYPES or len(stock) != len(new):
        log.append('%s: the car it replaces (%s) is not a game car type of the same name length' % (car['dir'].name, stock))
        return
    old, nl, sl = stock, new.lower(), stock.lower()
    d = car['dir']
    G, T, n = rename_car_files(Path(cv['geo']).read_bytes(), Path(cv['tex']).read_bytes(), old, new)
    out = a.out / 'converted' / 'CARS' / new
    out.mkdir(parents=True, exist_ok=True)
    (out / 'GEOMETRY.BIN').write_bytes(G)
    (out / 'TEXTURES.BIN').write_bytes(T)
    files = [(out / 'GEOMETRY.BIN', 'CARS/%s/GEOMETRY.BIN' % new), (out / 'TEXTURES.BIN', 'CARS/%s/TEXTURES.BIN' % new)]
    dmg = next((f for f in (a.game / 'CARS' / 'DAMAGE').glob('*') if f.name.lower() == old.lower() + '.damage'), None)
    if dmg:
        files.append((dmg, 'CARS/DAMAGE/%s.damage' % new))
    PVk = jhash('pvehicle')
    pv = src.coll(PVk, jhash(sl))
    fd = src.classes[PVk]['fields'][jhash('frontend')]
    fe_key = struct.unpack_from('<I', pv.layout.data, fd['off'] + 4)[0]
    manuf = src_value(src, 'frontend', maker_from, 'manufacturer')
    script = ['copy_node pvehicle %s %s' % (sl, nl),
              'update_field pvehicle %s MODEL Collision %s' % (nl, new),
              'update_field pvehicle %s MODEL Visual %s' % (nl, new)]
    rg = REGIONS.get(spec.get('region'))
    if rg:
        # the region and the maker, made once (a second car's copies are skipped as existing)
        # the car's own `region` stays one the game lists (Europe): core.nfsu moves its shop entries into the
        # added section (see addoncars.inc)
        script += ['copy_node frontend europe vehicles %s' % spec['region'],
                   'update_field frontend %s region %d' % (spec['region'], rg['listed_as']),
                   'copy_node frontend %s %s %s' % (maker_from, spec['region'], maker),
                   'update_field frontend %s region %d' % (maker, rg['listed_as'])]
        car['region'] = spec['region']
    elif spec.get('maker_parent'):
        # a maker the game hasn't (made once; a second car's copy is skipped as existing)
        script.append('copy_node frontend %s %s %s' % (maker_from, spec['maker_parent'], maker))
    script.append('copy_node frontend 0x%08x %s %s' % (fe_key, maker, fe_name))
    if manuf is not None:
        script.append('update_field frontend %s manufacturer %d' % (fe_name, manuf))
    if rg:
        script.append('update_field frontend %s region %d' % (fe_name, rg['listed_as']))
    elif spec.get('maker_parent'):
        # the section it is listed in follows the car's own `region` (the replaced Silvia's would be Japan)
        script.append('update_field frontend %s region %d' % (fe_name, {'north_america': 0, 'europe': 1, 'japan': 2}[spec['maker_parent']]))
    script += ['update_field pvehicle %s frontend Collection %s' % (nl, fe_name),
               'copy_node car %s %s' % (sl, nl),
               'update_field car %s name %s' % (nl, new),
               'update_field car %s parts Collection %s' % (nl, nl),
               'update_field car %s pvehicle Collection %s' % (nl, nl),
               'copy_node car_parts %s %s' % (sl, nl),
               'copy_node ecar %s %s' % (sl, nl)]
    # the mod's own edits of the car it replaced, now of the new car (ecar wheels, pvehicle, car_parts)
    for ln in cv['lines']:
        q = ln.split()
        if len(q) >= 4 and q[0] in ('update_field', 'update_collection') and q[1] in ('ecar', 'pvehicle', 'car_parts') \
                and q[2] == sl:
            script.append(' '.join(q[:2] + [nl] + q[3:]))
    script += ['update_field car_list all_cars Cars[0] Collection %s' % nl,
               'update_field car_list fe_carlist StockCars[0] Collection %s' % nl,
               'update_field car_list performance_matchable Cars[0] Collection %s' % nl]
    car['script_lines'] = script
    car['files'] = files
    car['labels'] = [('CARNAME_%s_%s' % (maker.upper(), fe_name.upper()), title)]
    log.append('%s: made a car of its own, %s (was %s; %d names renamed; manufacturer %s%s)' % (
        d.name, new, old, n, manuf, ', region ' + spec['region'] if rg else ''))


# replacement mods made cars of their own: script stem -> (new car type, same length as the replaced one; frontend
# name; maker's frontend node and a car of that maker whose manufacturer number it takes; the car's name)
CONVERT = {
    'NIS_240_SX_89': dict(new='TOY_COROLLA_1', fe='corolla', maker='toyota', maker_from='supra', region=None,
                          title='Toyota Corolla'),
    # the two Ladas (2026-11-21): a maker of their own, Lada, in the Europe section. A "Russia" section of their own
    # was tried and is not possible this way: the shop's three sections are drawn by its Flash movie itself (no game
    # code builds or labels them - FUN_0058d780 and FUN_00591180 never ran in the shop, the labels were never asked
    # for), so a fourth needs the movie changed.
    'FOR_FOC_ST_07': dict(new='LAD_SAMARA_04', fe='samara2114', maker='lada', maker_from='renault', region='russia',
                          title='Lada Samara (VAZ-2114)'),
    'NIS_SYL_AER_99': dict(new='LAD_1200S_1980', fe='lada1200s', maker='lada', maker_from='renault', region='russia',
                           title='Lada 1200s (VAZ-2101)'),
}
# add-ons moved to a maker and region of their own: folder -> region, maker node (copy of maker_from), car name
MOVE_TO_REGION = {'Marussia B2': dict(region='russia', maker='marussia', maker_from='lotus', title='Marussia B2')}
# the regions the car lot can show beyond the game's three (frontend `region` value; core.nfsu adds the tab)
REGIONS = {'russia': dict(value=3, listed_as=1, label='REGION_RUSSIAN', text={'english': 'Russian',
                                                                  # the game's own Cyrillic code (Russian_Global.bin:
                                                                  # Р a8 о 81 с 88 и 83 я 92, from its labels)
                                                                  'russian': bytes.fromhex('a8 81 88 88 83 92')})}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mods', type=Path, required=True)
    ap.add_argument('--game', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    cars = find_cars(a.mods)
    if not cars:
        print('no add-on cars in', a.mods)
        return 0
    g = a.game / 'GLOBAL'
    src = Source([g / 'attributes.bin', g / 'cars_vault.BIN'])
    B = unpack_lzc((g / 'GlobalB.lzc').read_bytes()) if (g / 'GlobalB.lzc').is_file() else (g / 'GLOBALB.BUN').read_bytes()
    for t, d in chunks(B):
        if t == 0x34600:
            CAR_TYPES[:] = [d[8 + 0x130 * i:8 + 0x130 * i + 32].split(b'\0')[0].decode() for i in range((len(d) - 8) // 0x130)]
    b = Builder(src)
    PV = jhash('pvehicle')
    info = []
    a.out.mkdir(parents=True, exist_ok=True)
    for car in cars:
        if car.get('convert'):
            convert_car(car, a, src, b.log)
    for car in cars:
        before, lists_before = len(b.order), len(b.lists)
        lines = list(car.get('script_lines', []))
        for f in car['scripts']:
            lines += f.read_text(errors='replace').splitlines()
        # a source car that doesn't carry an add-on's model (2026-11-20): the Miura copies the Gallardo LP560 and the
        # car lot drew nothing for it (its model file read, no car shown; parts from another car didn't help); its
        # car, car_parts, ecar and pvehicle are taken from the Murcielago LP640, as the Aventador SVJ's are, whose
        # model shows. Its frontend (price, tier) and sounds stay as its scripts make them.
        # the game finds a car's type through its pvehicle collection's own name (lam_mur_lp_06 <-> LAM_MUR_LP_06 for
        # every car it has): the Miura's scripts call its collections lam_mr_sv_71 but its car type LAM_MSV_71, and the
        # car lot never loaded its model - not its files (the SVJ's model and textures renamed as the Miura's stayed
        # invisible too), its data (2026-11-20). Its collections are named after its car type here.
        pvl = next((ln.split() for ln in lines if ln.split()[:2] == ['copy_node', 'pvehicle'] and len(ln.split()) >= 4), None)
        mdl = next((ln.split()[5] for ln in lines if ln.split()[:2] == ['update_field', 'pvehicle'] and len(ln.split()) == 6
                    and ln.split()[3:5] == ['MODEL', 'Visual']), None)
        if pvl and mdl and pvl[-1].upper() != mdl.upper():
            old_name, new_name = pvl[-1], mdl.lower()
            lines = [re.sub(r'\b%s(?=\b|_)' % re.escape(old_name), new_name, ln) for ln in lines]
            b.log.append('%s: collections %s renamed %s after its car type' % (car['dir'].name, old_name, new_name))
            RENAMED.append((jhash(old_name), jhash(new_name)))
        # an add-on put under a maker and region of its own (MOVE_TO_REGION): its frontend record's parent becomes
        # the new maker, made (once) from a game maker, the region made like the Ladas'
        mv = MOVE_TO_REGION.get(car['dir'].name)
        if mv:
            fe_rec = next((ln.split()[5] for ln in lines if ln.split()[:2] == ['update_field', 'pvehicle'] and
                           len(ln.split()) == 6 and ln.split()[3:5] == ['frontend', 'Collection']), None)
            rg = REGIONS[mv['region']]
            pre = ['copy_node frontend europe vehicles %s' % mv['region'],
                   'update_field frontend %s region %d' % (mv['region'], rg['listed_as']),
                   'copy_node frontend %s %s %s' % (mv['maker_from'], mv['region'], mv['maker']),
                   'update_field frontend %s region %d' % (mv['maker'], rg['listed_as'])]
            out_ = []
            for ln in lines:
                q = ln.split()
                if fe_rec and len(q) == 5 and q[:2] == ['copy_node', 'frontend'] and q[4] == fe_rec:
                    out_ += [' '.join(q[:3] + [mv['maker'], fe_rec]),
                             'update_field frontend %s region %d' % (fe_rec, rg['listed_as'])]
                else:
                    out_.append(ln)
            lines = pre + out_
            car['region'] = mv['region']
            if fe_rec:
                car['extra_labels'] = [('CARNAME_%s_%s' % (mv['maker'].upper(), fe_rec.upper()), mv['title'])]
        for a_, b_ in SOURCE_SWAP.items():
            lines = [re.sub(r'^(copy_node (?:car|car_parts|ecar|pvehicle) )%s ' % a_, r'\g<1>%s ' % b_, ln) for ln in lines]
        b.run(lines, car['dir'].name + (' ' + car['label'] if car.get('label') else ''))
        for i in range(lists_before, len(b.lists)):
            b.lists[i] = b.lists[i] + (car['group'],)
        made = b.order[before:]
        # sold from the start (2026-11-20, the person did not see them): the copies keep their source car's
        # tier (UnlockedAt 6 = the CCX's, the last tier; the Miura's script sets 2), and the car lot hides a car
        # until its tier is reached
        FE = jhash('frontend')
        for c in made:
            if c.cls == FE and c.layout is not None:
                cost = src.classes[FE]['fields'][jhash('Cost')]
                if struct.unpack_from('<I', c.layout.data, cost['off'])[0] > 0:
                    b.set_field(c, FE, jhash('UnlockedAt'), -1, '', '0')
        pv = [c for c in made if c.cls == PV]
        if not pv:
            b.log.append('%s: no pvehicle made' % car['dir'].name)
            continue
        pv = pv[0]
        # the car type: MODEL's string (what Collision/Visual name), and the car type it was copied from
        fd = src.classes[PV]['fields'][jhash('MODEL')]
        mdl = pv.layout.fix.get(fd['off'] + 4)
        name = mdl[1] if mdl and mdl[0] == 's' else None
        srcpv = next((ln.split()[2] for ln in lines if ln.split()[:2] == ['copy_node', 'pvehicle']), None)
        tmpl = None
        # the source pvehicle: the game's, or one an earlier car of a pack made
        c0 = None
        if srcpv:
            c0 = b.new.get((PV, jhash(srcpv))) or (src.coll(PV, jhash(srcpv)) if (PV, jhash(srcpv)) in src.colls else None)
        if c0 is not None and c0.layout is not None:
            k = struct.unpack_from('<I', c0.layout.data, fd['off'])[0]
            # the source pvehicle's MODEL hash names its car type (jhash of the CarTypeInfo name)
            tmpl = {jhash(n): n for n in CAR_TYPES}.get(k)
            if tmpl is None:  # a pack car copied from an earlier one: that one's own template
                tmpl = next((c['template'] for c in info if jhash(c['name']) == k), None)
        if car.get('cartype_from') and car['cartype_from'] in CAR_TYPES:
            tmpl = car['cartype_from']      # the .end copies its car type from another car than its pvehicle
        if not name or not tmpl:
            b.log.append('%s: car type unknown (name %s, template %s)' % (car['dir'].name, name, tmpl))
            continue
        if car.get('parts_own'):
            # its own part rows, under its own car type (the export may name another model: rows are what count)
            m_, rows_ = binary_parts(Path(car['parts_own']).read_bytes())
            EXTRA_PARTS.append((name, rows_))
        ini_ = car['ini']
        labels = []
        for i in range(1, 9):
            lab, txt = ini_.get(('RESOURCE%d' % i, 'label')), ini_.get(('RESOURCE%d' % i, 'name'))
            if lab and txt:
                labels.append((lab, txt))
        fe = [c for c in made if c.cls == jhash('frontend')]
        if not labels:
            labels = list(car.get('labels', []))
        labels = list(car.get('extra_labels', [])) + labels
        labels += [x for x in car.get('extras', {}).get('strings', []) if x not in labels]
        if not labels and fe:
            # no Ed config (the Miura): the label the game builds, CARNAME_<maker>_<frontend name>
            maker = {jhash('lamborghini'): 'LAMBORGHINI', jhash('koenigsegg'): 'KOENIGSEGG', jhash('ferrari'): 'FERRARI'}.get(fe[0].parent)
            if maker:
                labels.append(('CARNAME_%s_%s' % (maker, fe[0].name.upper()), car['dir'].name.replace(' for NFS Undercover', '')))
        info.append(dict(name=name, template=tmpl, collide=ini_.get(('COLLISION', 'copyfrom'), car.get('collide') or tmpl), parts_from=car.get('parts_from'), parts_own=car.get('parts_own'),
                         usage=int(ini_[('INFO', 'usagetype')]) if ('INFO', 'usagetype') in ini_ else None,
                         memtype=ini_.get(('INFO', 'carmemtype')), skin=int(ini_[('INFO', 'skinnable')]) if ('INFO', 'skinnable') in ini_ else None,
                         paint=ini_.get(('INFO', 'defaultbasepaint')), labels=labels, car=car, pv=pv))
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / 'ADDONCARS.BIN').write_bytes(write_vault('addon_cars', b.order))
    # what the cars add to game files (the Honda's logo and vinyls): part rows into both bundles; textures, vector
    # vinyls into copies of the game's files (or of a file another car replaces) read through the overlay
    extra_tex, extra_vinyls = {}, []
    for c in info:
        ex = c['car'].get('extras') or {}
        for f in ex.get('parts', []):
            EXTRA_PARTS.append(binary_parts(f.read_bytes()))
        for k, v in ex.get('textures', {}).items():
            extra_tex.setdefault(k, []).extend(v)
        extra_vinyls += ex.get('vinyls', [])
    made = []      # (file, its game path)
    for gp, items in sorted(extra_tex.items()):
        base = next((s_ for c in info for s_, d_ in c['car']['overlays'] if d_.upper() == gp), None)
        base = base or next((f for f in (a.game / gp.split('/')[0]).glob('*') if f.name.upper() == gp.split('/')[-1]), None)
        if base is None:
            b.log.append('%s not found: its textures left out' % gp)
            continue
        f = a.out / 'made' / gp
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(tpk_add_textures(Path(base).read_bytes(), [(n, Path(p_).read_bytes()) for n, p_ in items]))
        made.append((f, gp))
        for c in info:     # the other car's replacement of this file is in the new one
            c['car']['overlays'] = [(s_, d_) for s_, d_ in c['car']['overlays'] if d_.upper() != gp]
        b.log.append('%s: %d textures added' % (gp, len(items)))
    if extra_vinyls:
        vb = a.game / 'CARS' / 'VINYLS' / 'VINYLS.BIN'
        if vb.is_file():
            f = a.out / 'made' / 'CARS' / 'VINYLS' / 'VINYLS.BIN'
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(vinyls_add(vb.read_bytes(), [Path(v).read_bytes() for v in extra_vinyls]))
            made.append((f, 'CARS/VINYLS/VINYLS.BIN'))
            b.log.append('CARS/VINYLS/VINYLS.BIN: %d vector vinyls added' % len(extra_vinyls))
    # GLOBALB.BUN: the add-on cars' types only (read while mod_addon_cars is on and the pack is off);
    # GLOBALB_ALL.BUN: with the Porsche Pack's too (read while mod_porsche_pack is on) - so the pack's 26 car types
    # are in the game only when its switch is
    order = sorted(info, key=lambda c: 0 if c['name'] in TYPE_FIRST else 1)   # stable: TYPE_FIRST first
    info_t = order
    (a.out / 'GLOBALB.BUN').write_bytes(patch_globalb(B, [c for c in info_t if c['car']['group'] != 'porsche'], b.log))
    if any(c['car']['group'] == 'porsche' for c in info):
        (a.out / 'GLOBALB_ALL.BUN').write_bytes(patch_globalb(B, info_t, []))
    with open(a.out / 'addoncars.txt', 'w') as f:
        # "in <group>": the lines after it belong to that group's switch (addon: mod_addon_cars, porsche:
        # mod_porsche_pack)
        for coll, field, car, grp in b.lists:
            f.write('in %s\nlist car_list %s %s pvehicle %s\n' % (grp, coll, field, car))
        # the dealer rows the car lot shows (the career keeps them in the save, so a loaded career lacks them):
        # "dealer <pvehicle> <frontend> <name label>": the label is the car's full name (its first Ed string), which
        # core.nfsu answers for the frontend key when the game builds the name (FUN_00525070)
        FEK = jhash('frontend')
        fdf = src.classes[PV]['fields'][jhash('frontend')]
        for c in info:
            fe_key = struct.unpack_from('<I', c['pv'].layout.data, fdf['off'] + 4)[0]
            lab = binhash(c['labels'][0][0], upper=True) if c['labels'] else 0
            f.write('in %s\ndealer %08x %08x %08x\n' % (c['car']['group'], c['pv'].key, fe_key, lab))
        for o_, n_ in RENAMED:
            f.write('in addon\nrekey %08x %08x\n' % (o_, n_))
        # a region of the add-on cars' own (core.nfsu adds its car lot tab)
        for rn in sorted({c['car'].get('region') for c in info if c['car'].get('region')}):
            f.write('in addon\nregion %d %08x\n' % (REGIONS[rn]['value'], binhash(REGIONS[rn]['label'], upper=True)))
            for c in info:
                if c['car'].get('region') == rn:
                    f.write('in addon\nregioncar %08x\n' % c['pv'].key)
        for c in info:
            f.write('in %s\n' % c['car']['group'])
            for lab, txt in c['labels']:
                f.write('name %08x %s\n' % (binhash(lab, upper=True), txt))
    # the names as the game's own strings (2026-11-20: the car lot reads its text past every hook, so it showed
    # MISSING STRING): every language's LANGUAGES/<Language>_Global.bin with the cars' labels added, as Ed adds
    # them on Windows; read in place of the game's while a switch is on (launch.c, a folder overlay)
    names = {}
    for c in info:
        for lab, txt in c['labels']:
            names.setdefault(binhash(lab, upper=True), txt)
    # region labels, each language's own text where known (the Russian file's own letter code)
    lang_names = {}
    for rn in sorted({c['car'].get('region') for c in info if c['car'].get('region')}):
        h = binhash(REGIONS[rn]['label'], upper=True)
        names[h] = REGIONS[rn]['text']['english']
        lang_names[h] = REGIONS[rn]['text']
    lang = a.game / 'LANGUAGES'
    if lang.is_dir() and names:
        (a.out / 'languages').mkdir(exist_ok=True)
        for p in sorted(lang.glob('*_Global.bin')):
            ln = p.name.lower().split('_global')[0]
            these = {h: lang_names.get(h, {}).get(ln, t) for h, t in names.items()}
            (a.out / 'languages' / p.name.lower()).write_bytes(add_strings(p.read_bytes(), these))
    with open(a.out / 'files.txt', 'w') as f:
        for c in info:
            for s_, d_ in c['car']['files']:
                # a model with plain solids (the Miura's) packed as the game's cars are (2026-11-20: as stored, and as
                # uncompressed 'RAWW' blocks, the car lot never looked its solids up)
                if PACK_PLAIN_MODELS and d_.upper().endswith('/GEOMETRY.BIN'):
                    pk = pack_geometry(Path(s_).read_bytes())
                    if pk is not None:
                        s_ = a.out / 'packed' / d_
                        s_.parent.mkdir(parents=True, exist_ok=True)
                        s_.write_bytes(pk)
                        print('model %s: plain solids packed (%d -> %d bytes)' % (d_, Path(c['car']['files'][0][0]).stat().st_size if False else len(pk), len(pk)))
                f.write('%s\t%s\n' % (s_, d_))
            for s_, d_ in c['car']['overlays']:
                f.write('overlay\t%s\t%s\n' % (s_, d_))
        for s_, d_ in made:
            f.write('overlay\t%s\t%s\n' % (s_, d_))
    for c in info:
        print('add-on car %s (%s): copied from %s, collision of %s, %d names' % (c['name'], c['car']['dir'].name, c['template'], c['collide'], len(c['labels'])))
    print('vault: %d collections; car lists: %d entries' % (len(b.order), len(b.lists)))
    for line in b.log:
        print('  ' + line)
    return 0


# the game's car types (GLOBALB CarTypeInfo names) - filled from the bundle before use
CAR_TYPES = []

if __name__ == '__main__':
    _a = sys.argv
    sys.exit(main())
