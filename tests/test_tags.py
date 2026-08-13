#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Тесты переноса тегов LLAO (групповая модель TagSet).

Покрывают сценарии optimize (allow_merge=false) и restore (allow_merge=true)
для разных наборов тегов: одиночная группа, несколько групп с конфликтом,
sidecar v1/v2, картинки, ReplayGain, cuesheet, лимиты caps.

Запуск:
    python3 tests/test_tags.py          # полный прогон
    python3 tests/test_tags.py --keep   # не удалять scratch_test
    python3 tests/test_tags.py --build  # пересобрать llao.exe

Требования: wine, ffmpeg/ffprobe в PATH, llao.exe (соберётся сам).
"""
import glob
import json
import os
import shutil
import struct
import subprocess
import sys
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLAO = os.path.join(ROOT, "llao.exe")
WORK = os.path.join(ROOT, "scratch_test")
FIX = os.path.join(WORK, "_fixtures")

KEEP = "--keep" in sys.argv
FORCE_BUILD = "--build" in sys.argv

# ---------------------------------------------------------------------------
# Хелперы
# ---------------------------------------------------------------------------

def sh(cmd, cwd=None, timeout=1200):
    env = dict(os.environ)
    env["WINEDEBUG"] = "-all"
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd,
                       timeout=timeout, env=env)
    if r.returncode != 0:
        raise RuntimeError("cmd=%s rc=%s\n%s\n%s" % (cmd, r.returncode,
                                                     r.stdout, r.stderr))
    return r


def ffmpeg(args, cwd=None, **kw):
    return sh(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y"] + args,
              cwd=cwd, **kw)


def rd(b, o):
    return struct.unpack_from("<I", b, o)[0]


# Канонические ключи — повторяют src/tags.cpp::canonical_key().
_CANON = {
    "title": "title", "artist": "artist", "album": "album",
    "albumartist": "album_artist", "album_artist": "album_artist",
    "composer": "composer", "genre": "genre",
    "date": "date", "year": "date", "originaldate": "date",
    "track": "track", "tracknumber": "track",
    "disc": "disc", "discnumber": "disc",
    "comment": "comment", "isrc": "isrc", "encoder": "encoder",
    "lyrics": "lyrics", "unsyncedlyrics": "lyrics",
    "copyright": "copyright", "copyrightmessage": "copyright",
    "cuesheet": "cue_sheet",
    "replaygaintrackgain": "replaygain_track_gain",
    "replaygaintrackpeak": "replaygain_track_peak",
    "replaygainalbumgain": "replaygain_album_gain",
    "replaygainalbumpeak": "replaygain_album_peak",
}


def canon(k):
    c = "".join(ch for ch in k.lower() if ch not in " _-")
    return _CANON.get(c, c)


def assert_subset(expect, got, what=""):
    for k, vals in expect.items():
        assert k in got, "%s: нет ключа %r в %s" % (what, k, sorted(got))
        for v in vals:
            assert v in got[k], "%s: нет значения %r в %s=%s" % (what, v, k, got[k])


def cp(src, dst):
    shutil.copy2(src, dst)


def run_tool(args, timeout=1200):
    cmd = ["wine", LLAO] + args
    env = dict(os.environ)
    env["WINEDEBUG"] = "-all"
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT,
                       timeout=timeout, env=env)
    out = (r.stdout or "") + (r.stderr or "")
    return r.returncode, out


# ---------------------------------------------------------------------------
# Чтение записанных тегов (независимые парсеры)
# ---------------------------------------------------------------------------

def decode_text(b, enc):
    if enc == 0:
        return b.decode("latin-1").rstrip("\x00")
    if enc == 1:
        return b.decode("utf-16", "replace").rstrip("\x00")
    if enc == 2:
        return b.decode("utf-16-be", "replace").rstrip("\x00")
    return b.decode("utf-8", "replace").rstrip("\x00")


def _split_comm(b, enc):
    if enc in (1, 2):
        i = b.find(b"\x00\x00")
        return "" if i < 0 else decode_text(b[i + 2:], enc)
    i = b.find(b"\x00")
    return "" if i < 0 else decode_text(b[i + 1:], enc)


def _split_txxx(b, enc):
    if enc in (1, 2):
        i = b.find(b"\x00\x00")
        if i < 0:
            return ("", "")
        return (decode_text(b[:i], enc), decode_text(b[i + 2:], enc))
    i = b.find(b"\x00")
    if i < 0:
        return ("", "")
    return (b[:i].decode("latin-1"), b[i + 1:].decode("latin-1"))


def read_id3v2(path):
    """ID3v2 в начале файла -> {'fields': {...}, 'pictures': int}."""
    d = open(path, "rb").read()
    if d[:3] != b"ID3":
        return None
    ss = lambda b: (b[0] << 21) | (b[1] << 14) | (b[2] << 7) | b[3]
    size = ss(d[6:10])
    body = d[10:10 + size]
    pos = 0
    if (d[5] & 0x40) and len(body) >= 4:
        pos = 4 + ss(body[0:4])
    fid_map = {
        "TIT2": "title", "TPE1": "artist", "TPE2": "album_artist",
        "TALB": "album", "TCOM": "composer", "TCON": "genre",
        "TDRC": "date", "TRCK": "track", "TPOS": "disc", "TSRC": "isrc",
        "TENC": "encoder", "TCOP": "copyright",
    }
    out = {}
    pics = 0

    def put(k, v):
        out.setdefault(k, [])
        if v not in out[k]:
            out[k].append(v)

    while pos + 10 <= len(body):
        fid = body[pos:pos + 4].decode("latin1")
        fsz = ss(body[pos + 4:pos + 8])
        f = body[pos + 10:pos + 10 + fsz]
        pos += 10 + fsz
        if not f or fid[0] == "\x00":
            continue
        if fid in fid_map:
            t = decode_text(f[1:], f[0])
            if t:
                put(fid_map[fid], t)
        elif fid == "COMM":
            t = _split_comm(f[4:], f[0])
            if t:
                put("comment", t)
        elif fid == "TXXX":
            desc, val = _split_txxx(f[1:], f[0])
            if desc and val:
                put(canon(desc), val)
        elif fid == "USLT":
            t = decode_text(f[4:], f[0])
            if t:
                put("lyrics", t)
        elif fid == "APIC":
            pics += 1
    return {"fields": out, "pictures": pics}


def read_apev2(path):
    """APEv2 в хвосте -> {'fields': {...}, 'pictures': int}."""
    d = open(path, "rb").read()
    if d[-32:-24] != b"APETAGEX":
        return None
    tag_size = rd(d, len(d) - 20)
    item_count = rd(d, len(d) - 16)
    if tag_size > len(d):
        return None
    items = len(d) - tag_size
    if items < 0:
        return None
    if d[items:items + 8] == b"APETAGEX":
        items += 32
    end = len(d) - 32
    out = {}
    pics = 0
    for _ in range(item_count):
        if items + 8 > end:
            break
        vsz = rd(d, items)
        items += 8
        kend = d.index(b"\x00", items)
        key = d[items:kend].decode("latin1")
        items = kend + 1
        val = d[items:items + vsz]
        items += vsz
        if key.lower().startswith("cover art"):
            pics += 1
        else:
            out.setdefault(canon(key), []).append(val.decode("utf-8", "replace"))
    return {"fields": out, "pictures": pics}


def read_flac(path):
    """VORBIS_COMMENT/PICTURE блоки FLAC -> {'fields': {...}, 'pictures': int}."""
    d = open(path, "rb").read()
    if d[:4] != b"fLaC":
        return None
    o = 4
    vcs = []
    pics = 0
    while o + 4 <= len(d):
        h = d[o]
        btype = h & 0x7f
        last = h & 0x80
        blen = (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]
        body = d[o + 4:o + 4 + blen]
        if btype == 4:
            vcs.append(body)
        elif btype == 6:
            pics += 1
        o += 4 + blen
        if last:
            break
    out = {}
    for vc in vcs:
        vlen = rd(vc, 0)
        pos = 4 + vlen
        cnt = rd(vc, pos)
        pos += 4
        for _ in range(cnt):
            clen = rd(vc, pos)
            pos += 4
            kv = vc[pos:pos + clen]
            pos += clen
            if b"=" not in kv:
                continue
            k, v = kv.split(b"=", 1)
            ck = canon(k.decode("utf-8", "replace"))
            val = v.decode("utf-8", "replace")
            if ck == "cue_sheet":
                out["cue_sheet"] = val
            else:
                out.setdefault(ck, []).append(val)
    return {"fields": out, "pictures": pics}


def read_sidecar_groups(zp):
    z = zipfile.ZipFile(zp)
    doc = json.loads(z.read("tags.json"))
    out = []
    for g in doc.get("groups", []):
        out.append({
            "type": g.get("type"),
            "fields": {k: list(v) for k, v in g.get("fields", {}).items()},
            "cue_sheet": g.get("cue_sheet", ""),
            "pictures": len(g.get("pictures", [])),
        })
    return out


# ---------------------------------------------------------------------------
# Создание фикстур
# ---------------------------------------------------------------------------

def make_png1x1():
    sig = b"\x89PNG\r\n\x1a\n"

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + \
               struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

    return sig + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00")) + chunk(b"IEND", b"")


_JPG_BYTES = None


def make_jpeg1x1():
    """1x1 JPEG, генерируется ffmpeg (нужен для Cover Art в APEv2)."""
    global _JPG_BYTES
    if _JPG_BYTES is None:
        p = os.path.join(FIX, "pix1x1.jpg")
        ffmpeg(["-f", "lavfi", "-i", "color=black:s=1x1",
                "-frames:v", "1", p], cwd=FIX)
        with open(p, "rb") as f:
            _JPG_BYTES = f.read()
    return _JPG_BYTES


def build_id3v2(frames, enc=1, pictures=()):
    """ID3v2.4: frames = [(frame_id, text)]. TIT2/TPE1 — enc=1 (UTF-16 с BOM),
    COMM — как пишет LLAO: enc=3 (UTF-8) + lang(3) + desc\\0 + text.
    pictures = [(type, mime, description, data)] -> APIC-фреймы (enc=3)."""
    def ss(n):
        return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f, n & 0x7f])

    body = b""
    for fid, text in frames:
        if fid == "COMM":
            payload = b"\x03" + b"eng" + b"\x00" + text.encode("utf-8")
        elif enc == 1:
            payload = b"\x01" + b"\xff\xfe" + text.encode("utf-16-le")
        else:
            payload = b"\x00" + text.encode("latin-1")
        body += fid.encode() + ss(len(payload)) + b"\x00\x00" + payload
    for ptype, mime, desc, data in pictures:
        payload = b"\x03" + mime.encode() + b"\x00" + bytes([ptype]) + \
            desc.encode("utf-8") + b"\x00" + data
        body += b"APIC" + ss(len(payload)) + b"\x00\x00" + payload
    return b"ID3\x04\x00\x00" + ss(len(body)) + body


def inject_id3_chunk(wav_path, tag):
    """Вставляет 'id3 ' чанк перед data-чанком WAV."""
    d = open(wav_path, "rb").read()
    assert d[:4] == b"RIFF" and d[8:12] == b"WAVE"
    o = 12
    out = bytearray(d[:12])
    inserted = False
    while o + 8 <= len(d):
        cid = d[o:o + 4]
        sz = struct.unpack_from("<I", d, o + 4)[0]
        nxt = o + 8 + sz + (sz & 1)
        if cid == b"data" and not inserted:
            pad = b"\x00" if len(tag) % 2 else b""
            out += b"id3 " + struct.pack("<I", len(tag)) + tag + pad
            inserted = True
        out += d[o:nxt]
        o = nxt
    assert inserted, "data-чанк не найден"
    struct.pack_into("<I", out, 4, len(out) - 8)
    open(wav_path, "wb").write(bytes(out))


def build_apev2(items, pictures=()):
    """APEv2-тег (header + items + footer), как пишет LLAO (src/tags.cpp:
    build_apev2). items = [(key, value)], pictures = [(description, data)] ->
    Cover Art (Back), флаги 0x2, значение desc\\0 + data."""
    body = b""
    for k, v in items:
        val = v.encode("utf-8")
        body += struct.pack("<II", len(val), 0) + k.encode() + b"\x00" + val
    for desc, data in pictures:
        val = desc.encode("utf-8") + b"\x00" + data
        body += struct.pack("<II", len(val), 0x2) + \
            b"Cover Art (Back)\x00" + val
    tag_size = len(body) + 32
    item_count = len(items) + len(pictures)

    def hdr(flags):
        return b"APETAGEX" + struct.pack("<IIII", 2000, tag_size, item_count,
                                         flags) + b"\x00" * 8

    return hdr(0xA0000000) + body + hdr(0xC0000000)


def append_apev2(wav_path, tag):
    """Дописывает APEv2 в хвост WAV (за пределами RIFF) — LLAO разбирает его
    обобщённым сканером хвоста, ffprobe игнорирует лишние байты."""
    with open(wav_path, "ab") as f:
        f.write(tag)


def rewrite_vorbis_body(body, rename):
    """Переписывает VORBIS_COMMENT: rename: {'DESCRIPTION': 'COMMENT'}."""
    vlen = rd(body, 0)
    pos = 4 + vlen
    vendor = body[4:4 + vlen]
    cnt = rd(body, pos)
    pos += 4
    items = []
    for _ in range(cnt):
        clen = rd(body, pos)
        pos += 4
        item = body[pos:pos + clen]
        pos += clen
        if b"=" in item:
            k, v = item.split(b"=", 1)
            kd = k.decode("utf-8", "replace")
            k = rename.get(kd, kd).encode()
            item = k + b"=" + v
        items.append(item)
    out = struct.pack("<I", vlen) + vendor + struct.pack("<I", len(items))
    for it in items:
        out += struct.pack("<I", len(it)) + it
    return out


def build_picture_block(png):
    """METADATA_BLOCK_PICTURE (type 3 = front cover), как в FLAC-спеце."""
    mime = b"image/png"
    body = struct.pack(">I", 3) + struct.pack(">I", len(mime)) + mime + \
        struct.pack(">I", 0) + struct.pack(">IIIII", 1, 1, 8, 0, len(png)) + png
    return (6, body)


def rewrite_flac_metadata(path, rename=None, strip_tags=False, picture=None):
    """Правит блоки FLAC: переименование ключей VORBIS_COMMENT и/или удаление
    VORBIS_COMMENT/PICTURE (для 'пустого' источника) и/или вставка картинки."""
    d = open(path, "rb").read()
    assert d[:4] == b"fLaC"
    o = 4
    blocks = []
    while o + 4 <= len(d):
        h = d[o]
        btype = h & 0x7f
        last = h & 0x80
        blen = (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]
        blocks.append((btype, last, d[o + 4:o + 4 + blen]))
        o += 4 + blen
        if last:
            break
    audio = d[o:]
    kept = []
    for btype, last, body in blocks:
        if btype == 4:
            if strip_tags:
                continue
            body = rewrite_vorbis_body(body, rename or {})
        elif btype == 6 and strip_tags:
            continue
        kept.append((btype, body))
    if picture is not None:
        kept.append(build_picture_block(picture))
    out = bytearray(d[:4])
    for i, (btype, body) in enumerate(kept):
        hdr = btype | (0x80 if i == len(kept) - 1 else 0)
        out.append(hdr)
        out.append((len(body) >> 16) & 0xff)
        out.append((len(body) >> 8) & 0xff)
        out.append(len(body) & 0xff)
        out += body
    out += audio
    open(path, "wb").write(bytes(out))


def write_zip(path, entries):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            z.writestr(name, data)


def write_sidecar_v1(base_path, fields, pictures=()):
    doc = {
        "version": 1,
        "format": "llao-sidecar",
        "fields": {k: list(v) for k, v in fields.items()},
    }
    entries = {"tags.json": json.dumps(doc).encode()}
    pics = []
    for i, (mime, data) in enumerate(pictures):
        ext = "png" if mime.endswith("png") else "jpg"
        pics.append({"type": 3, "mime": mime, "description": "",
                     "file": "pictures/%d.%s" % (i, ext)})
        entries["pictures/%d.%s" % (i, ext)] = data
    doc["pictures"] = pics
    entries["tags.json"] = json.dumps(doc).encode()
    write_zip(base_path + ".tags.zip", entries)


def write_sidecar_v2(base_path, groups):
    doc = {"version": 2, "format": "llao-sidecar", "groups": []}
    entries = {}
    picidx = 0
    for g in groups:
        jg = {"type": g["type"], "fields": g.get("fields", {})}
        pics = []
        for mime, data in g.get("pictures", []):
            ext = "png" if mime.endswith("png") else "jpg"
            name = "pictures/%d.%s" % (picidx, ext)
            pics.append({"type": 3, "mime": mime, "description": "", "file": name})
            entries[name] = data
            picidx += 1
        jg["pictures"] = pics
        doc["groups"].append(jg)
    entries["tags.json"] = json.dumps(doc).encode()
    write_zip(base_path + ".tags.zip", entries)


def gen_fixtures():
    if os.path.exists(FIX):
        return
    os.makedirs(FIX)
    mix = os.path.join(FIX, "mix.wav")
    ffmpeg(["-f", "lavfi",
            "-i", "aevalsrc=0.5*sin(2*PI*440*t)+0.3*sin(2*PI*880*t)+"
                  "0.2*sin(2*PI*1760*t)+0.1*sin(2*PI*293*t)|"
                  "0.5*sin(2*PI*440*t)+0.3*sin(2*PI*660*t)+"
                  "0.2*sin(2*PI*1320*t):d=8:s=44100",
            "-ac", "2", "-c:a", "pcm_s16le", mix])

    # flac уровня 0 — намеренно крупнее целей (tta/wavpack), чтобы замена
    # на месте (best_cost < probe.size) срабатывала.
    ffmpeg(["-i", mix, "-c:a", "flac", "-compression_level", "0",
            "-metadata", "title=Test Title",
            "-metadata", "artist=Test Artist",
            "-metadata", "album=Test Album",
            "-metadata", "comment=Test Comment",
            "-metadata", "date=2020",
            "-metadata", "track=3",
            "-metadata", "genre=Test Genre",
            "-metadata", "replaygain_track_gain=-11.70 dB",
            "-metadata", "replaygain_track_peak=0.999969",
            os.path.join(FIX, "single.flac")], cwd=FIX)
    # ffmpeg пишет общий 'comment' как DESCRIPTION — приводим к честному COMMENT,
    # чтобы проверить перенос комментария (COMM в ID3v2, Comment в APEv2).
    rewrite_flac_metadata(os.path.join(FIX, "single.flac"),
                          rename={"DESCRIPTION": "COMMENT"})

    ffmpeg(["-i", mix, "-c:a", "flac", "-compression_level", "0",
            "-metadata", "title=Cue Title",
            "-metadata", "CUESHEET=PERFORMER Test Artist\r\nTITLE Test\r\nFILE x.wav\r\n  TRACK 01 AUDIO\r\n    TITLE Track\r\n    INDEX 01 00:00:00",
            os.path.join(FIX, "single_cue.flac")], cwd=FIX)

    # single_full.flac: полный набор текстовых тегов + ReplayGain + CUESHEET +
    # картинка — для round-trip flac -> wavpack -> flac.
    ffmpeg(["-i", mix, "-c:a", "flac", "-compression_level", "0",
            "-metadata", "title=Full Title",
            "-metadata", "artist=Full Artist",
            "-metadata", "album=Full Album",
            "-metadata", "album_artist=Full Album Artist",
            "-metadata", "composer=Full Composer",
            "-metadata", "comment=Full Comment",
            "-metadata", "date=2020",
            "-metadata", "track=3",
            "-metadata", "disc=1",
            "-metadata", "genre=Full Genre",
            "-metadata", "replaygain_track_gain=-11.70 dB",
            "-metadata", "replaygain_track_peak=0.999969",
            "-metadata", "CUESHEET=PERFORMER Full Artist\r\nTITLE Full Album\r\nFILE x.wav\r\n  TRACK 01 AUDIO\r\n    TITLE Track\r\n    INDEX 01 00:00:00",
            os.path.join(FIX, "single_full.flac")], cwd=FIX)
    rewrite_flac_metadata(os.path.join(FIX, "single_full.flac"),
                          rename={"DESCRIPTION": "COMMENT"},
                          picture=make_png1x1())

    ffmpeg(["-i", mix, "-c:a", "flac", "-map_metadata", "-1",
            os.path.join(FIX, "tagless.flac")], cwd=FIX)
    # ffmpeg всегда добавляет ENCODER — полностью удаляем теги.
    rewrite_flac_metadata(os.path.join(FIX, "tagless.flac"), strip_tags=True)

    # dual.wav: LIST INFO (from ffmpeg) + встроенный ID3v2 (UTF-16) с
    # конфликтующими значениями.
    ffmpeg(["-i", mix, "-c:a", "pcm_s16le",
            "-metadata", "title=LIST Title",
            "-metadata", "artist=LIST Artist",
            "-metadata", "album=LIST Album",
            "-metadata", "comment=LIST Comment",
            os.path.join(FIX, "dual.wav")], cwd=FIX)
    inject_id3_chunk(os.path.join(FIX, "dual.wav"),
                     build_id3v2([("TIT2", "ID3 Title"),
                                  ("TPE1", "ID3 Artist"),
                                  ("COMM", "ID3 Comment")]))

    # triple_*.wav: три типа тегов в одном файле — LIST INFO (riff) + 'id3 '
    # чанк (id3v2) + APEv2-хвост. Общие ключи двух групп совпадают, у каждой
    # группы есть уникальное поле и своя картинка.
    for name, ape_title in (("triple_match", "Merge Title"),
                            ("triple_conflict", "Ape Title")):
        triple = os.path.join(FIX, name + ".wav")
        ffmpeg(["-i", mix, "-c:a", "pcm_s16le",
                "-metadata", "title=Merge Title",
                "-metadata", "artist=Riff Artist",
                "-metadata", "album=Merge Album",
                triple], cwd=FIX)
        inject_id3_chunk(triple,
                         build_id3v2([("TIT2", "Merge Title"),
                                      ("TALB", "Merge Album"),
                                      ("TCOM", "Id3 Composer")],
                                     pictures=[(3, "image/png", "front",
                                                make_png1x1())]))
        append_apev2(triple,
                     build_apev2([("Title", ape_title),
                                  ("Album", "Merge Album"),
                                  ("Genre", "Ape Genre")],
                                 pictures=[("back", make_jpeg1x1())]))

    ffmpeg(["-i", mix, "-c:a", "wavpack",
            "-metadata", "title=WV Title",
            "-metadata", "artist=WV Artist",
            os.path.join(FIX, "wv_single.wv")], cwd=FIX)

    ffmpeg(["-i", mix, "-c:a", "libmp3lame", "-write_id3v1", "0",
            "-metadata", "title=MP3 Title",
            "-metadata", "artist=MP3 Artist",
            os.path.join(FIX, "mp3_single.mp3")], cwd=FIX)

    png = make_png1x1()
    write_sidecar_v1(os.path.join(FIX, "v1"),
                     {"title": ["Restored Title"], "artist": ["Restored Artist"],
                      "album": ["Restored Album"], "comment": ["Restored Comment"],
                      "track": ["7"]},
                     pictures=[("image/png", png)])
    shutil.move(os.path.join(FIX, "v1.tags.zip"), os.path.join(FIX, "v1.tags.zip"))

    write_sidecar_v2(os.path.join(FIX, "v2_id3"),
                     [{"type": "id3v2", "fields": {"title": ["ID3 Single Title"],
                                                    "artist": ["ID3 Single Artist"]}}])
    write_sidecar_v2(os.path.join(FIX, "v2_dual"),
                     [{"type": "id3v2", "fields": {"title": ["G1 Title"],
                                                    "artist": ["G1 Artist"]}},
                      {"type": "riff", "fields": {"title": ["G2 Title"],
                                                   "artist": ["G2 Artist"]}}])
    write_sidecar_v2(os.path.join(FIX, "v2_vorbis"),
                     [{"type": "vorbis", "fields": {"title": ["V Title"],
                                                     "artist": ["V Artist"],
                                                     "comment": ["V Comment"]}}])


# ---------------------------------------------------------------------------
# Сценарии
# ---------------------------------------------------------------------------

RESULTS = []


def scenario(key, desc, fn):
    d = os.path.join(WORK, key)
    os.makedirs(d, exist_ok=True)
    try:
        fn(d)
        RESULTS.append((desc, True, "ok"))
    except Exception as e:
        RESULTS.append((desc, False, str(e)[:400]))


def check_ok(name, out):
    assert out.count("OK") > 0, "нет OK в выводе:\n" + out[-1500:]
    assert "SKIP" not in out, "есть SKIP в выводе:\n" + out[-1500:]


# --- Optimize (allow_merge=false) ---

def o1_flac_to_tta(d):
    cp(os.path.join(FIX, "single.flac"), os.path.join(d, "src.flac"))
    rc, out = run_tool(["optimize", d, "--formats=tta", "--jobs=1"])
    assert rc == 0, out
    check_ok("O1", out)
    assert os.path.exists(os.path.join(d, "src.tta")), "нет src.tta"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), "неожиданный sidecar"
    id3 = read_id3v2(os.path.join(d, "src.tta"))
    assert id3, "в tta нет ID3v2"
    assert_subset({"title": ["Test Title"], "artist": ["Test Artist"],
                   "album": ["Test Album"], "comment": ["Test Comment"],
                   "date": ["2020"], "track": ["3"], "genre": ["Test Genre"],
                   "replaygain_track_gain": ["-11.70 dB"],
                   "replaygain_track_peak": ["0.999969"]},
                  id3["fields"], "O1 tta")
    assert id3["pictures"] == 0, "в tta не должно быть картинок"


def o2_flac_to_wavpack(d):
    cp(os.path.join(FIX, "single.flac"), os.path.join(d, "src.flac"))
    rc, out = run_tool(["optimize", d, "--formats=wavpack", "--jobs=1"])
    assert rc == 0, out
    check_ok("O2", out)
    assert os.path.exists(os.path.join(d, "src.wv")), "нет src.wv"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), "неожиданный sidecar"
    ape = read_apev2(os.path.join(d, "src.wv"))
    assert ape, "в wv нет APEv2"
    assert_subset({"title": ["Test Title"], "artist": ["Test Artist"],
                   "comment": ["Test Comment"], "replaygain_track_gain": ["-11.70 dB"]},
                  ape["fields"], "O2 wv")


def o3_wv_to_flac(d):
    cp(os.path.join(FIX, "wv_single.wv"), os.path.join(d, "src.wv"))
    rc, out = run_tool(["optimize", d, "--formats=flac", "--jobs=1"])
    assert rc == 0, out
    check_ok("O3", out)
    assert os.path.exists(os.path.join(d, "src.flac")), "нет src.flac"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), "неожиданный sidecar"
    fl = read_flac(os.path.join(d, "src.flac"))
    assert fl, "в flac нет VORBIS_COMMENT"
    assert_subset({"title": ["WV Title"], "artist": ["WV Artist"]},
                  fl["fields"], "O3 flac")


def o4_mp3_to_flac(d):
    cp(os.path.join(FIX, "mp3_single.mp3"), os.path.join(d, "src.mp3"))
    rc, out = run_tool(["restore", d, "--to=flac", "--jobs=1", "--allow-lossy"])
    assert rc == 0, out
    check_ok("O4", out)
    assert os.path.exists(os.path.join(d, "src.flac")), "нет src.flac"
    fl = read_flac(os.path.join(d, "src.flac"))
    assert fl, "в flac нет VORBIS_COMMENT"
    assert_subset({"title": ["MP3 Title"], "artist": ["MP3 Artist"]},
                  fl["fields"], "O4 flac")


def o5_wav_conflict_to_flac(d):
    cp(os.path.join(FIX, "dual.wav"), os.path.join(d, "src.wav"))
    rc, out = run_tool(["optimize", d, "--formats=flac", "--jobs=1"])
    assert rc == 0, out
    check_ok("O5", out)
    assert os.path.exists(os.path.join(d, "src.flac")), "нет src.flac"
    fl = read_flac(os.path.join(d, "src.flac"))
    assert fl, "в flac нет VORBIS_COMMENT"
    # flac.exe сам копирует LIST INFO в VORBIS_COMMENT при кодировании — это
    # артефакт кодера, главное, что ID3v2 (второй конфликтующей группы) не встроен.
    assert "ID3 Title" not in fl["fields"].get("title", []), \
        "ID3v2-значения встроены в flac: %s" % fl["fields"]
    assert os.path.exists(os.path.join(d, "src.tags.zip")), "нет sidecar"
    g = read_sidecar_groups(os.path.join(d, "src.tags.zip"))
    types = [x["type"] for x in g]
    assert "riff" in types and "id3v2" in types, "группы: %s" % types
    for x in g:
        if x["type"] == "riff":
            assert_subset({"title": ["LIST Title"]}, x["fields"], "O5 riff")
        if x["type"] == "id3v2":
            assert_subset({"title": ["ID3 Title"]}, x["fields"], "O5 id3v2")


def o6_wav_conflict_to_tta(d):
    cp(os.path.join(FIX, "dual.wav"), os.path.join(d, "src.wav"))
    rc, out = run_tool(["optimize", d, "--formats=tta", "--jobs=1"])
    assert rc == 0, out
    check_ok("O6", out)
    assert os.path.exists(os.path.join(d, "src.tta")), "нет src.tta"
    id3 = read_id3v2(os.path.join(d, "src.tta"))
    assert id3, "в tta нет ID3v2"
    # id3v2-группа встроена (TIT2/TPE1 — UTF-16, COMM — UTF-8), riff — в sidecar
    assert_subset({"title": ["ID3 Title"], "artist": ["ID3 Artist"],
                   "comment": ["ID3 Comment"]}, id3["fields"], "O6 tta")
    assert os.path.exists(os.path.join(d, "src.tags.zip")), "нет sidecar"
    g = read_sidecar_groups(os.path.join(d, "src.tags.zip"))
    assert [x["type"] for x in g] == ["riff"], "группы: %s" % g
    assert_subset({"title": ["LIST Title"]}, g[0]["fields"], "O6 riff")


def o7_cue_to_tta_caps(d):
    cp(os.path.join(FIX, "single_cue.flac"), os.path.join(d, "src.flac"))
    rc, out = run_tool(["optimize", d, "--formats=tta", "--jobs=1"])
    assert rc == 0, out
    check_ok("O7", out)
    assert os.path.exists(os.path.join(d, "src.tta")), "нет src.tta"
    id3 = read_id3v2(os.path.join(d, "src.tta"))
    # tta-кодер (ffmpeg) свои теги может не писать вообще; главное, что наши
    # поля в tta не встраиваются (caps cue_sheet=false) — весь набор в sidecar.
    if id3:
        assert "title" not in id3["fields"] and "cue_sheet" not in id3["fields"], \
            "в tta встроены наши теги: %s" % id3["fields"]
    assert os.path.exists(os.path.join(d, "src.tags.zip")), "нет sidecar"
    g = read_sidecar_groups(os.path.join(d, "src.tags.zip"))
    assert len(g) == 1, "групп: %s" % g
    assert g[0]["cue_sheet"], "в sidecar нет cue_sheet: %s" % g[0]
    assert g[0]["fields"].get("title") == ["Cue Title"], g[0]["fields"]


# --- Restore (allow_merge=true) ---

def r1_v1_sidecar_to_wavpack(d):
    cp(os.path.join(FIX, "tagless.flac"), os.path.join(d, "src.flac"))
    cp(os.path.join(FIX, "v1.tags.zip"), os.path.join(d, "src.tags.zip"))
    rc, out = run_tool(["restore", d, "--to=wavpack", "--jobs=1"])
    assert rc == 0, out
    check_ok("R1", out)
    assert os.path.exists(os.path.join(d, "src.wv")), "нет src.wv"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), "старый sidecar не удалён"
    ape = read_apev2(os.path.join(d, "src.wv"))
    assert ape, "в wv нет APEv2"
    assert_subset({"title": ["Restored Title"], "artist": ["Restored Artist"],
                   "album": ["Restored Album"], "comment": ["Restored Comment"],
                   "track": ["7"]}, ape["fields"], "R1 wv")
    assert ape["pictures"] == 1, "картинка из v1-sidecar не сохранена: %d" % ape["pictures"]


def r2_v2_single_to_tta(d):
    cp(os.path.join(FIX, "tagless.flac"), os.path.join(d, "src.flac"))
    cp(os.path.join(FIX, "v2_id3.tags.zip"), os.path.join(d, "src.tags.zip"))
    rc, out = run_tool(["restore", d, "--to=tta", "--jobs=1"])
    assert rc == 0, out
    check_ok("R2", out)
    assert os.path.exists(os.path.join(d, "src.tta")), "нет src.tta"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), "sidecar не удалён"
    id3 = read_id3v2(os.path.join(d, "src.tta"))
    assert id3, "в tta нет ID3v2"
    assert_subset({"title": ["ID3 Single Title"], "artist": ["ID3 Single Artist"]},
                  id3["fields"], "R2 tta")


def r3_v2_conflict_to_tta(d):
    cp(os.path.join(FIX, "tagless.flac"), os.path.join(d, "src.flac"))
    cp(os.path.join(FIX, "v2_dual.tags.zip"), os.path.join(d, "src.tags.zip"))
    rc, out = run_tool(["restore", d, "--to=tta", "--jobs=1"])
    assert rc == 0, out
    check_ok("R3", out)
    assert os.path.exists(os.path.join(d, "src.tta")), "нет src.tta"
    id3 = read_id3v2(os.path.join(d, "src.tta"))
    assert id3, "в tta нет ID3v2"
    assert_subset({"title": ["G1 Title"]}, id3["fields"], "R3 tta")
    g = read_sidecar_groups(os.path.join(d, "src.tags.zip"))
    assert [x["type"] for x in g] == ["riff"], "группы: %s" % g
    assert_subset({"title": ["G2 Title"]}, g[0]["fields"], "R3 riff")


def r4_v2_vorbis_to_tta(d):
    cp(os.path.join(FIX, "tagless.flac"), os.path.join(d, "src.flac"))
    cp(os.path.join(FIX, "v2_vorbis.tags.zip"), os.path.join(d, "src.tags.zip"))
    rc, out = run_tool(["restore", d, "--to=tta", "--jobs=1"])
    assert rc == 0, out
    check_ok("R4", out)
    assert os.path.exists(os.path.join(d, "src.tta")), "нет src.tta"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), "sidecar не удалён"
    id3 = read_id3v2(os.path.join(d, "src.tta"))
    assert id3, "в tta нет ID3v2"
    assert_subset({"title": ["V Title"], "artist": ["V Artist"],
                   "comment": ["V Comment"]}, id3["fields"], "R4 tta")


def o8_v1_sidecar_to_optimfrog(d):
    cp(os.path.join(FIX, "tagless.flac"), os.path.join(d, "src.flac"))
    cp(os.path.join(FIX, "v1.tags.zip"), os.path.join(d, "src.tags.zip"))
    rc, out = run_tool(["restore", d, "--to=optimfrog", "--jobs=1"], timeout=1800)
    assert rc == 0, out
    check_ok("O8", out)
    assert os.path.exists(os.path.join(d, "src.ofr")), "нет src.ofr"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), "sidecar не удалён"
    ape = read_apev2(os.path.join(d, "src.ofr"))
    assert ape, "в ofr нет APEv2"
    assert_subset({"title": ["Restored Title"], "comment": ["Restored Comment"]},
                  ape["fields"], "O8 ofr")
    assert ape["pictures"] == 1, "картинка не сохранена в ofr"


def e1_round_trip_wav_tta_flac(d):
    cp(os.path.join(FIX, "dual.wav"), os.path.join(d, "src.wav"))
    rc, out = run_tool(["optimize", d, "--formats=tta", "--jobs=1"])
    assert rc == 0, out
    check_ok("E1a", out)
    assert os.path.exists(os.path.join(d, "src.tta")), "нет src.tta"
    assert os.path.exists(os.path.join(d, "src.tags.zip")), "нет sidecar после optimize"
    g = read_sidecar_groups(os.path.join(d, "src.tags.zip"))
    assert [x["type"] for x in g] == ["riff"], "группы: %s" % g
    assert_subset({"title": ["LIST Title"]}, g[0]["fields"], "E1 optimize riff")
    id3 = read_id3v2(os.path.join(d, "src.tta"))
    assert id3, "в tta нет ID3v2"
    assert_subset({"title": ["ID3 Title"]}, id3["fields"], "E1 optimize tta")

    rc, out = run_tool(["restore", d, "--to=flac", "--jobs=1"])
    assert rc == 0, out
    check_ok("E1b", out)
    assert os.path.exists(os.path.join(d, "src.flac")), "нет src.flac"
    fl = read_flac(os.path.join(d, "src.flac"))
    assert fl, "в flac нет VORBIS_COMMENT"
    assert not fl["fields"], "flac не должен иметь встроенных тегов: %s" % fl["fields"]
    g = read_sidecar_groups(os.path.join(d, "src.tags.zip"))
    types = sorted(x["type"] for x in g)
    assert "id3v2" in types and "riff" in types, "группы: %s" % types
    found = {"id3v2": False, "riff": False}
    for x in g:
        if x["type"] == "id3v2":
            found["id3v2"] = True
            assert_subset({"title": ["ID3 Title"]}, x["fields"], "E1 id3v2")
        elif x["type"] == "riff":
            found["riff"] = True
            assert_subset({"title": ["LIST Title"]}, x["fields"], "E1 riff")
        elif x["type"] == "apev2":
            # encoder-only APEv2 — шум ffmpeg-муксера tta, данных не содержит
            assert set(x["fields"]) <= {"encoder"}, "apev2: %s" % x["fields"]
    assert found["id3v2"] and found["riff"], "группы: %s" % types


def e2_round_trip_full_flac(d):
    cp(os.path.join(FIX, "single_full.flac"), os.path.join(d, "src.flac"))
    rc, out = run_tool(["optimize", d, "--formats=wavpack", "--jobs=1"])
    assert rc == 0, out
    check_ok("E2a", out)
    assert os.path.exists(os.path.join(d, "src.wv")), "нет src.wv"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), \
        "неожиданный sidecar после optimize"
    ape = read_apev2(os.path.join(d, "src.wv"))
    assert ape, "в wv нет APEv2"
    assert_subset({"title": ["Full Title"], "artist": ["Full Artist"],
                   "album": ["Full Album"], "album_artist": ["Full Album Artist"],
                   "composer": ["Full Composer"], "comment": ["Full Comment"],
                   "date": ["2020"], "track": ["3"], "genre": ["Full Genre"],
                   "replaygain_track_gain": ["-11.70 dB"],
                   "replaygain_track_peak": ["0.999969"]},
                  ape["fields"], "E2 optimize wv")
    assert ape["fields"].get("cue_sheet"), "нет cue_sheet в wv"
    assert ape["pictures"] == 1, "картинка не в APEv2"

    rc, out = run_tool(["restore", d, "--to=flac", "--jobs=1"])
    assert rc == 0, out
    check_ok("E2b", out)
    assert os.path.exists(os.path.join(d, "src.flac")), "нет src.flac"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), "sidecar не удалён"
    fl = read_flac(os.path.join(d, "src.flac"))
    assert fl, "в flac нет VORBIS_COMMENT"
    assert_subset({"title": ["Full Title"], "artist": ["Full Artist"],
                   "album": ["Full Album"], "album_artist": ["Full Album Artist"],
                   "composer": ["Full Composer"], "comment": ["Full Comment"],
                   "date": ["2020"], "track": ["3"], "genre": ["Full Genre"],
                   "replaygain_track_gain": ["-11.70 dB"],
                   "replaygain_track_peak": ["0.999969"]},
                  fl["fields"], "E2 restore flac")
    assert fl["fields"].get("cue_sheet"), "нет cue_sheet в flac"
    assert fl["pictures"] == 1, "картинка не сохранена в flac"


def e3_triple_match_merge_to_tta(d):
    # 3 типа тегов в одном файле, все значения согласованы: restore мёржит всё
    # в один нативный ID3v2, уникальные поля каждой группы и обе картинки
    # (APIC из id3v2 + Cover Art из apev2) сохраняются, sidecar не создаётся.
    cp(os.path.join(FIX, "triple_match.wav"), os.path.join(d, "src.wav"))
    rc, out = run_tool(["restore", d, "--to=tta", "--jobs=1"])
    assert rc == 0, out
    check_ok("E3", out)
    assert os.path.exists(os.path.join(d, "src.tta")), "нет src.tta"
    assert not os.path.exists(os.path.join(d, "src.tags.zip")), "неожиданный sidecar"
    id3 = read_id3v2(os.path.join(d, "src.tta"))
    assert id3, "в tta нет ID3v2"
    assert_subset({"title": ["Merge Title"], "artist": ["Riff Artist"],
                   "album": ["Merge Album"], "composer": ["Id3 Composer"],
                   "genre": ["Ape Genre"]}, id3["fields"], "E3 tta")
    assert id3["pictures"] == 2, \
        "обе картинки должны сохраниться при merge: %d" % id3["pictures"]


def e4_triple_conflict_optimize_tta(d):
    # 3 типа тегов, APEv2 конфликтует по title: optimize не мёржит группы —
    # id3v2 встраивается в tta, конфликтующие riff+apev2 уходят в sidecar.
    cp(os.path.join(FIX, "triple_conflict.wav"), os.path.join(d, "src.wav"))
    rc, out = run_tool(["optimize", d, "--formats=tta", "--jobs=1"])
    assert rc == 0, out
    check_ok("E4", out)
    assert os.path.exists(os.path.join(d, "src.tta")), "нет src.tta"
    id3 = read_id3v2(os.path.join(d, "src.tta"))
    assert id3, "в tta нет ID3v2"
    assert_subset({"title": ["Merge Title"], "album": ["Merge Album"],
                   "composer": ["Id3 Composer"]}, id3["fields"], "E4 tta")
    assert "Ape Title" not in id3["fields"].get("title", []), \
        "конфликтующее значение встроено: %s" % id3["fields"]
    assert id3["pictures"] == 1, "в tta только id3v2-картинка: %d" % id3["pictures"]
    g = read_sidecar_groups(os.path.join(d, "src.tags.zip"))
    assert sorted(x["type"] for x in g) == ["apev2", "riff"], "группы: %s" % g
    for x in g:
        if x["type"] == "riff":
            assert_subset({"title": ["Merge Title"], "artist": ["Riff Artist"]},
                          x["fields"], "E4 riff")
        if x["type"] == "apev2":
            assert_subset({"title": ["Ape Title"], "genre": ["Ape Genre"]},
                          x["fields"], "E4 apev2")
            assert x["pictures"] == 1, "ape-картинка в sidecar: %d" % x["pictures"]


def e5_triple_conflict_roundtrip(d):
    # Конфликтующий набор из 3 типов через optimize(wavpack)->restore(flac):
    # apev2 встраивается в wv, riff+id3v2 (с картинкой) в sidecar; при restore
    # конфликт сохраняется, группы не мёржатся, обе картинки на месте.
    cp(os.path.join(FIX, "triple_conflict.wav"), os.path.join(d, "src.wav"))
    rc, out = run_tool(["optimize", d, "--formats=wavpack", "--jobs=1"])
    assert rc == 0, out
    check_ok("E5a", out)
    assert os.path.exists(os.path.join(d, "src.wv")), "нет src.wv"
    ape = read_apev2(os.path.join(d, "src.wv"))
    assert ape, "в wv нет APEv2"
    assert_subset({"title": ["Ape Title"], "genre": ["Ape Genre"]},
                  ape["fields"], "E5a wv")
    assert ape["pictures"] == 1, "ape-картинка в wv: %d" % ape["pictures"]
    g = read_sidecar_groups(os.path.join(d, "src.tags.zip"))
    assert sorted(x["type"] for x in g) == ["id3v2", "riff"], "группы: %s" % g
    for x in g:
        if x["type"] == "riff":
            assert_subset({"title": ["Merge Title"], "artist": ["Riff Artist"]},
                          x["fields"], "E5a riff")
        if x["type"] == "id3v2":
            assert_subset({"title": ["Merge Title"], "album": ["Merge Album"],
                           "composer": ["Id3 Composer"]},
                          x["fields"], "E5a id3v2")
            assert x["pictures"] == 1, "id3-картинка в sidecar: %d" % x["pictures"]

    rc, out = run_tool(["restore", d, "--to=flac", "--jobs=1"])
    assert rc == 0, out
    check_ok("E5b", out)
    assert os.path.exists(os.path.join(d, "src.flac")), "нет src.flac"
    fl = read_flac(os.path.join(d, "src.flac"))
    assert fl, "нет VORBIS_COMMENT"
    assert not fl["fields"], "конфликт -> ничего не встраивается в flac: %s" % fl["fields"]
    g = read_sidecar_groups(os.path.join(d, "src.tags.zip"))
    assert sorted(x["type"] for x in g) == ["apev2", "id3v2", "riff"], \
        "группы: %s" % g
    titles = []
    for x in g:
        titles += x["fields"].get("title", [])
    assert "Merge Title" in titles and "Ape Title" in titles, \
        "конфликтующие значения потеряны: %s" % titles
    pics = {x["type"]: x["pictures"] for x in g}
    assert pics.get("id3v2") == 1 and pics.get("apev2") == 1, \
        "картинки в sidecar: %s" % pics


SCENARIOS = [
    ("o1", "O1  flac(vorbis) -> tta: ID3v2 embed, без sidecar, все поля+ReplayGain", o1_flac_to_tta),
    ("o2", "O2  flac(vorbis) -> wavpack: APEv2 embed, без sidecar", o2_flac_to_wavpack),
    ("o3", "O3  wv(apev2) -> flac: vorbis embed, без sidecar", o3_wv_to_flac),
    ("o4", "O4  mp3(id3v2) -> flac (restore, lossy): vorbis embed", o4_mp3_to_flac),
    ("o5", "O5  wav(riff+id3v2 конфликт) -> flac: всё в sidecar, embed нет", o5_wav_conflict_to_flac),
    ("o6", "O6  wav(riff+id3v2 конфликт) -> tta: id3v2 embed (UTF-16), riff в sidecar", o6_wav_conflict_to_tta),
    ("o7", "O7  flac(cue_sheet) -> tta: caps не влезают -> весь набор в sidecar", o7_cue_to_tta_caps),
    ("o8", "O8  v1-sidecar -> optimfrog: APEv2 embed, валидация без ffprobe", o8_v1_sidecar_to_optimfrog),
    ("r1", "R1  v1-sidecar -> wavpack: APEv2 embed + картинка, sidecar удалён", r1_v1_sidecar_to_wavpack),
    ("r2", "R2  v2-sidecar (id3v2) -> tta: embed, без sidecar", r2_v2_single_to_tta),
    ("r3", "R3  v2-sidecar (id3v2+riff конфликт) -> tta: id3v2 embed, riff в sidecar", r3_v2_conflict_to_tta),
    ("r4", "R4  v2-sidecar (vorbis) -> tta: конвертация в id3v2, без sidecar", r4_v2_vorbis_to_tta),
    ("e1", "E1  round trip wav->tta->flac: данные не теряются (embed+sidecar)", e1_round_trip_wav_tta_flac),
    ("e2", "E2  round trip flac->wv->flac: полный набор тегов (текст+RG+cue_sheet+картинка)", e2_round_trip_full_flac),
    ("e3", "E3  3 типа тегов (riff+id3v2+apev2) согласованы -> restore tta: merge в ID3v2, уникальные поля + 2 картинки, без sidecar", e3_triple_match_merge_to_tta),
    ("e4", "E4  3 типа тегов, apev2 конфликтует -> optimize tta: id3v2 embed, riff+apev2 в sidecar", e4_triple_conflict_optimize_tta),
    ("e5", "E5  3 типа тегов конфликт -> wv->flac round trip: конфликт и обе картинки сохраняются", e5_triple_conflict_roundtrip),
]


# ---------------------------------------------------------------------------
# Сборка и запуск
# ---------------------------------------------------------------------------

def build():
    if os.path.exists(LLAO) and not FORCE_BUILD:
        return
    tc = None
    for p in sorted(glob.glob(os.path.expanduser("~") + "/opt/llvm-mingw*/bin"))[::-1]:
        if os.path.exists(os.path.join(p, "x86_64-w64-mingw32-g++")):
            tc = p
            break
    env = dict(os.environ)
    if tc:
        env["PATH"] = tc + os.pathsep + env.get("PATH", "")
    print("Сборка llao.exe...")
    r = subprocess.run(["make", "-s"], capture_output=True, text=True,
                       cwd=ROOT, timeout=1800, env=env)
    if r.returncode != 0:
        print(r.stdout, r.stderr)
        sys.exit("ОШИБКА: сборка не удалась")


def main():
    build()
    os.makedirs(WORK, exist_ok=True)
    gen_fixtures()
    for key, desc, fn in SCENARIOS:
        scenario(key, desc, fn)

    print()
    print("%-72s %s" % ("Сценарий", "Результат"))
    print("-" * 85)
    failed = 0
    for name, ok, detail in RESULTS:
        print("%-72s %s" % (name, "OK " if ok else "FAIL"))
        if not ok:
            failed += 1
            print("        " + detail.replace("\n", "\n        "))
    print("-" * 85)
    print("Итого: %d/%d прошло" % (len(RESULTS) - failed, len(RESULTS)))
    if not KEEP:
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
