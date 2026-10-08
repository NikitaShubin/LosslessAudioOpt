#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Тест выбора утилит: движок обязан брать СВОЮ копию ffmpeg/ffprobe, а не
системную.

Регресс (найден на живой библиотеке): на не-Windows сборке
media::find_ffmpeg()/find_ffprobe() искали `bin/ffmpeg/ffmpeg` — без
расширения. Вложенных файлов с таким именем не бывает: сборка кладёт
`ffmpeg.exe`/`ffprobe.exe`, в том числе на Linux, где они запускаются через
wine. Поиск проваливался, и движок молча уходил в PATH на системный ffmpeg.

Итог был расщеплением внутри одного прогона: кодирование alac/tta шло
вложенным бинарником (через tool::cached_binary, он смотрит маркер .binary), а
декодирование, профилирование и сверка тегов — системным. Два разных ffmpeg в
одной статистике.

Покрытие:
    M1  при наличии bin/ffmpeg/ffmpeg.exe find_ffmpeg берёт именно его
    M2  то же для ffprobe
    M3  если встроенного бинарника нет — берётся утилита из PATH
    M4  встроенный бинарник не переименовывается в .txt на время проверки
        (регресс: тест не должен ломать рабочую установку)

Запуск:
    python3 tests/test_media.py
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLAO_NATIVE = os.path.join(ROOT, "llao-linux")
LLAO_EXE = os.path.join(ROOT, "llao.exe")
FF_DIR = os.path.join(ROOT, "bin", "ffmpeg")

RESULTS = []


def pick_native():
    if not os.path.isfile(LLAO_NATIVE):
        return False
    if not os.path.isfile(LLAO_EXE):
        return True
    return os.path.getmtime(LLAO_NATIVE) >= os.path.getmtime(LLAO_EXE)


NATIVE = pick_native()
LLAO = LLAO_NATIVE if NATIVE else LLAO_EXE

PROBE_SRC = r'''
#include <cstdio>
#include "media.h"

int main(int argc, char** argv) {
    (void)argc;
    std::string want = argv[1];
    std::string got = want == "ffprobe" ? media::find_ffprobe() : media::find_ffmpeg();
    std::printf("%s\n", got.c_str());
    return got.empty() ? 1 : 0;
}
'''


def build_probe():
    out = os.path.join(ROOT, "_probe_media")
    src = os.path.join(ROOT, "_probe_media.cpp")
    with open(src, "w") as f:
        f.write(PROBE_SRC)
    # miniz.c — код на C, g++ собирает его как C++ и спотыкается о повторное
    # объявление массива, поэтому компилируем его отдельно компилятором C.
    mz = os.path.join(ROOT, "_probe_miniz.o")
    c = subprocess.run(["gcc", "-O0", "-Ithird_party", "-c", "-o", mz,
                        "third_party/miniz/miniz.c"], capture_output=True, text=True,
                       cwd=ROOT)
    if c.returncode != 0:
        return None, c.stderr[-800:]
    cmd = ["g++", "-std=c++17", "-O0", "-Ithird_party", "-Isrc",
           "-o", out, src, mz,
           "src/media.cpp", "src/config.cpp", "src/util.cpp", "src/i18n.cpp",
           "src/out.cpp", "src/tags_apev2.cpp", "src/tags_vorbis.cpp",
           "src/tags_id3.cpp", "src/tags_mp4.cpp", "src/tags_wav.cpp",
           "src/tags_core.cpp", "src/tags_write.cpp", "src/tags_sidecar.cpp",
           "src/proc.cpp"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    os.remove(src)
    if os.path.exists(mz):
        os.remove(mz)
    if r.returncode != 0 or not os.path.exists(out):
        return None, r.stderr[-800:]
    return out, ""


def run_probe(probe, which):
    r = subprocess.run([probe, which], capture_output=True, text=True, cwd=ROOT)
    return r.returncode, (r.stdout or "").strip()


def scenario(name, fn):
    try:
        fn()
        RESULTS.append((name, True, ""))
    except AssertionError as exc:
        RESULTS.append((name, False, str(exc)))
    except Exception as exc:  # noqa: BLE001 — в тесте это просто «ошибка сценария»
        RESULTS.append((name, False, "%s: %s" % (type(exc).__name__, exc)))


PROBE = None
ERR = ""


def m1_ffmpeg_bundled():
    code, got = run_probe(PROBE, "ffmpeg")
    if code == 1 and not got:
        raise AssertionError("find_ffmpeg ничего не вернул; сборка пробы: %s" % ERR)
    exe = os.path.join(FF_DIR, "ffmpeg.exe")
    if not os.path.exists(exe):
        raise AssertionError("нет %s — bin/ не наполнен, проверять нечего" % exe)
    assert os.path.basename(got) == "ffmpeg.exe", \
        "нативная сборка должна брать вложенный ffmpeg.exe, а не %r" % got


def m2_ffprobe_bundled():
    code, got = run_probe(PROBE, "ffprobe")
    if code == 1 and not got:
        raise AssertionError("find_ffprobe ничего не вернул; сборка пробы: %s" % ERR)
    exe = os.path.join(FF_DIR, "ffprobe.exe")
    if not os.path.exists(exe):
        raise AssertionError("нет %s — bin/ не наполнен, проверять нечего" % exe)
    assert os.path.basename(got) == "ffprobe.exe", \
        "нативная сборка должна брать вложенный ffprobe.exe, а не %r" % got


def m3_falls_back_to_path():
    """Встроенного бинарника нет — должна взять утилиту из PATH."""
    exe = os.path.join(FF_DIR, "ffprobe.exe")
    if not os.path.exists(exe):
        return  # нечего прятать
    hidden = exe + ".hidden-by-test"
    os.rename(exe, hidden)
    try:
        code, got = run_probe(PROBE, "ffprobe")
        assert got, "без встроенного бинарника должна взять утилиту из PATH"
        assert "bin/ffmpeg" not in got, \
            "встроенного файла нет, а вернулся путь из bin/ffmpeg: %r" % got
    finally:
        os.rename(hidden, exe)


def m4_install_intact():
    exe = os.path.join(FF_DIR, "ffmpeg.exe")
    assert os.path.exists(exe), "ffmpeg.exe должен лежать на месте после тестов"
    leftovers = [f for f in os.listdir(FF_DIR) if ".hidden" in f or f.endswith(".txt")]
    assert not leftovers, "тест оставил мусор в bin/ffmpeg: %r" % leftovers
    probe = os.path.join(ROOT, "_probe_media")
    if os.path.exists(probe):
        os.remove(probe)


SCENARIOS = [
    ("M1 find_ffmpeg берёт вложенный ffmpeg.exe", m1_ffmpeg_bundled),
    ("M2 find_ffprobe берёт вложенный ffprobe.exe", m2_ffprobe_bundled),
    ("M3 без встроенного бинарника — утилита из PATH", m3_falls_back_to_path),
    ("M4 установка не тронута", m4_install_intact),
]


def main():
    global PROBE, ERR
    PROBE, ERR = build_probe()
    if not PROBE:
        print("SKIP: не собралась проба (%s)" % ERR)
        return 0
    for name, fn in SCENARIOS:
        scenario(name, fn)
    ok = sum(1 for _, good, _ in RESULTS if good)
    for name, good, msg in RESULTS:
        print("%s %s" % ("PASS" if good else "FAIL", name))
        if not good:
            print("    " + msg)
    if os.path.exists(PROBE):
        os.remove(PROBE)
    print("\n%d/%d passed" % (ok, len(SCENARIOS)))
    return 0 if ok == len(SCENARIOS) else 1


if __name__ == "__main__":
    sys.exit(main())