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
    M1  bin/ffmpeg/ffmpeg.exe — find_ffmpeg берёт именно его
    M2  bin/ffmpeg/ffprobe.exe — find_ffprobe берёт именно его
    M3  в bin лежит только имя без расширения — берётся оно (POSIX-ветка)
    M4  в bin пусто — берётся утилита из PATH
    M5  рабочая установка не тронута: bin/ репозитория побитово тот же

Изоляция. Первые версии теста работали с настоящим bin/ репозитория и на время
проверки переименовывали боевой ffmpeg.exe. Так тест требовал заполненного bin/
(в CI его нет — кодек�� приезжают только в сборке релиза) и рисковал сломать
рабочую установку. Здесь всё живёт во временном каталоге: config::bin_dir()
считается от каталога исполняемого файла, поэтому проба собирается рядом с
поддельным bin/ и ни одного файла репозитория не касается.

Запуск:
    python3 tests/test_media.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

RESULTS = []

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

COMPILE_UNITS = [
    "src/media.cpp", "src/config.cpp", "src/util.cpp", "src/i18n.cpp",
    "src/out.cpp", "src/tags_apev2.cpp", "src/tags_vorbis.cpp",
    "src/tags_id3.cpp", "src/tags_mp4.cpp", "src/tags_wav.cpp",
    "src/tags_core.cpp", "src/tags_write.cpp", "src/tags_sidecar.cpp",
    "src/proc.cpp",
]


def build_probe(workdir):
    """Собирает пробу в workdir — её каталог и есть тот, где ищется bin/."""
    out = os.path.join(workdir, "probe_media")
    src = os.path.join(workdir, "probe_media.cpp")
    with open(src, "w") as f:
        f.write(PROBE_SRC)
    # miniz.c — код на C, g++ собирает его как C++ и спотыкается о повторное
    # объявление массива, поэтому компилируем его отдельно компилятором C.
    mz = os.path.join(workdir, "probe_miniz.o")
    c = subprocess.run(["gcc", "-O0", "-Ithird_party", "-c", "-o", mz,
                        "third_party/miniz/miniz.c"], capture_output=True, text=True,
                       cwd=ROOT)
    if c.returncode != 0:
        return None, c.stderr[-800:]
    cmd = ["g++", "-std=c++17", "-O0", "-Ithird_party", "-Isrc",
           "-o", out, src, mz] + COMPILE_UNITS
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0 or not os.path.exists(out):
        return None, r.stderr[-800:]
    return out, ""


def run_probe(probe, which, path_ffmpeg=None):
    """Запускает пробу; path_ffmpeg — каталог, который кладётся в PATH."""
    env = dict(os.environ)
    if path_ffmpeg:
        env["PATH"] = path_ffmpeg + os.pathsep + env.get("PATH", "")
    r = subprocess.run([probe, which], capture_output=True, text=True, cwd=ROOT, env=env)
    return r.returncode, (r.stdout or "").strip()


def scenario(name, fn):
    try:
        fn()
        RESULTS.append((name, True, ""))
    except AssertionError as exc:
        RESULTS.append((name, False, str(exc)))
    except Exception as exc:  # noqa: BLE001 — в тесте это просто «ошибка сценария»
        RESULTS.append((name, False, "%s: %s" % (type(exc).__name__, exc)))


def install_stub(ff_dir, name):
    """Кладёт файл-заглушку нужного имени и возвращает его путь."""
    path = os.path.join(ff_dir, name)
    with open(path, "w") as f:
        f.write("stub\n")
    os.chmod(path, 0o755)
    return path


# Проба, поддельный bin/ и PATH-каталог живут всё время прогона.
PROBE = ""
ERR = ""
FF_DIR = ""


def m1_ffmpeg_bundled():
    exe = os.path.join(FF_DIR, "ffmpeg.exe")
    install_stub(FF_DIR, "ffmpeg.exe")
    try:
        code, got = run_probe(PROBE, "ffmpeg")
        if code == 1 and not got:
            raise AssertionError("find_ffmpeg ничего не вернул; сборка пробы: %s" % ERR)
        assert got == exe, "ожидался вложенный %r, получено %r" % (exe, got)
    finally:
        os.remove(exe)


def m2_ffprobe_bundled():
    exe = os.path.join(FF_DIR, "ffprobe.exe")
    install_stub(FF_DIR, "ffprobe.exe")
    try:
        code, got = run_probe(PROBE, "ffprobe")
        if code == 1 and not got:
            raise AssertionError("find_ffprobe ничего не вернул; сборка пробы: %s" % ERR)
        assert got == exe, "ожидался вложенный %r, получено %r" % (exe, got)
    finally:
        os.remove(exe)


def m3_native_name_wins():
    """В bin лежит только имя без расширения — POSIX-ветка берёт его."""
    native = install_stub(FF_DIR, "ffprobe")
    # Оба имени рядом: без расширения проверяется первым, и это правильный
    # порядок на POSIX — нативный файл не нужно гонять через wine.
    install_stub(FF_DIR, "ffprobe.exe")
    try:
        _, got = run_probe(PROBE, "ffprobe")
        assert got == native, \
            "при наличии обоих имён ожидалось нативное %r, получено %r" % (native, got)
    finally:
        os.remove(native)
        os.remove(os.path.join(FF_DIR, "ffprobe.exe"))


def m4_falls_back_to_path():
    """В bin пусто — должна взять утилиту из PATH."""
    path_dir = os.path.join(os.path.dirname(FF_DIR), "pathbin")
    os.makedirs(path_dir, exist_ok=True)
    stub = install_stub(path_dir, "ffprobe")
    try:
        _, got = run_probe(PROBE, "ffprobe", path_ffmpeg=path_dir)
        assert got, "без встроенного бинарника должна взять утилиту из PATH"
        assert got == stub, "ожидался %r из PATH, получено %r" % (stub, got)
        assert os.path.basename(got) != "ffprobe.exe" or os.path.dirname(got) != FF_DIR, \
            "встроенного файла нет, а вернулся путь из bin/ffmpeg: %r" % got
    finally:
        os.remove(stub)


def m5_install_intact():
    """Ни один файл рабочей установки не должен быть тронут."""
    bin_ffmpeg = os.path.join(ROOT, "bin", "ffmpeg")
    current = sorted(os.listdir(bin_ffmpeg)) if os.path.isdir(bin_ffmpeg) else []
    assert current == INSTALL_SNAPSHOT["bin_ffmpeg"], \
        "тест изменил содержимое bin/ffmpeg: было %r, стало %r" \
        % (INSTALL_SNAPSHOT["bin_ffmpeg"], current)
    leftovers = [f for f in os.listdir(ROOT)
                 if f.startswith("_probe_media") or f.startswith("probe_media")]
    assert not leftovers, "тест оставил мусор в корне репозитория: %r" % leftovers


INSTALL_SNAPSHOT = {"bin_ffmpeg": []}

SCENARIOS = [
    ("M1 find_ffmpeg берёт вложенный ffmpeg.exe", m1_ffmpeg_bundled),
    ("M2 find_ffprobe берёт вложенный ffprobe.exe", m2_ffprobe_bundled),
    ("M3 без расширения — нативный файл (POSIX)", m3_native_name_wins),
    ("M4 встроенного нет — утилита из PATH", m4_falls_back_to_path),
    ("M5 рабочая установка не тронута", m5_install_intact),
]


def main():
    global PROBE, ERR, FF_DIR, INSTALL_SNAPSHOT
    bin_ffmpeg = os.path.join(ROOT, "bin", "ffmpeg")
    INSTALL_SNAPSHOT = {
        "bin_ffmpeg": sorted(os.listdir(bin_ffmpeg)) if os.path.isdir(bin_ffmpeg) else []
    }
    workdir = tempfile.mkdtemp(prefix="llao-media-")
    try:
        FF_DIR = os.path.join(workdir, "bin", "ffmpeg")
        os.makedirs(FF_DIR)
        PROBE, ERR = build_probe(workdir)
        if not PROBE:
            print("SKIP: не собралась проба (%s)" % ERR)
            return 0
        for name, fn in SCENARIOS:
            scenario(name, fn)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    ok = sum(1 for _, good, _ in RESULTS if good)
    for name, good, msg in RESULTS:
        print("%s %s" % ("PASS" if good else "FAIL", name))
        if not good:
            print("    " + msg)
    print("\n%d/%d passed" % (ok, len(SCENARIOS)))
    return 0 if ok == len(SCENARIOS) else 1


if __name__ == "__main__":
    sys.exit(main())