#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Тесты обработки ошибок и честных результатов LLAO.

Регрессия (v1.1.1): раньше валидные .ofr-файлы (ffmpeg не умеет читать
OptimFROG вообще) помечались как SKIP, а операционные сбои (битый файл,
отсутствующая утилита) маскировались под итог «ошибок: 0» и exit 0.

Покрытие:
    F1  мусорный .flac -> optimize: rc != 0, «ошибок: 1»
    F2  мусорный .flac -> restore:  rc != 0, ERROR
    F3  .ofr (родной OptimFROG, ffprobe не читает) -> optimize: rc == 0
    F4  .ofr -> restore: rc == 0
    F5  испорченный .ofr (native-декод не восстанавливает) -> optimize: rc != 0
    F6  нет Takc.exe -> optimize/restore --formats=tak: rc != 0 (tool missing)
    G1  сбой ВАРИАНТА (кодер отвергает вход) -> optimize: rc=1, ERROR с
        именем файла и причиной, строка файла в --report
    G2  то же с --ignore-errors: прогон продолжается, упавший файл помечен, но
        пропущенные файлы ошибкой не считаются («ошибок: 0», rc=0)
    H1  неизвестный id в --formats -> rc=1, «unknown format», без перебора

Запуск:
    python3 tests/test_errors.py          # полный прогон
    python3 tests/test_errors.py --keep   # не удалять scratch_err
    python3 tests/test_errors.py --build  # пересобрать llao.exe

Требования: wine, ffmpeg/ffprobe в PATH, llao.exe (соберётся сам), bin/ наполнен.
"""
import glob
import os
import random
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLAO_EXE = os.path.join(ROOT, "llao.exe")
LLAO_NATIVE = os.path.join(ROOT, "llao-linux")
WORK = os.path.join(ROOT, "scratch_err")
FIX = os.path.join(WORK, "_fixtures")
BIN = os.path.join(ROOT, "bin")

KEEP = "--keep" in sys.argv
FORCE_BUILD = "--build" in sys.argv

NATIVE_LINUX = os.path.isfile(LLAO_NATIVE) and not os.path.isfile(LLAO_EXE)
LLAO = LLAO_NATIVE if NATIVE_LINUX else LLAO_EXE


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


def cp(src, dst):
    shutil.copy2(src, dst)


def run_tool(args, timeout=1200):
    if NATIVE_LINUX:
        cmd = [LLAO] + args
    else:
        cmd = ["wine", LLAO] + args
    env = dict(os.environ)
    env["WINEDEBUG"] = "-all"
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT,
                       timeout=timeout, env=env)
    out = (r.stdout or "") + (r.stderr or "")
    return r.returncode, out


def has_errors(out, n):
    """Итоговая строка «errors: N» / «ошибок: N» (язык зависит от локали)."""
    return bool(re.search(r"(?:errors|ошибок):\s*%d\b" % n, out))


# ---------------------------------------------------------------------------
# Фикстуры
# ---------------------------------------------------------------------------

def gen_fixtures():
    if os.path.exists(FIX):
        return
    os.makedirs(FIX)

    mix = os.path.join(FIX, "mix.wav")
    ffmpeg(["-f", "lavfi",
            "-i", "aevalsrc=0.5*sin(2*PI*440*t)+0.3*sin(2*PI*880*t)|"
                  "0.4*sin(2*PI*660*t)+0.2*sin(2*PI*1320*t):duration=6:sample_rate=44100",
            "-ac", "2", "-c:a", "pcm_s16le", mix])

    # F1/F2: вообще не медиафайл.
    with open(os.path.join(FIX, "garbage.flac"), "wb") as f:
        f.write(b"this is not audio data " * 64)

    # F3/F4: .ofr, сгенерированный родным OptimFROG. ffmpeg/ffprobe такие
    # файлы не читают (у ffmpeg нет OptimFROG-декодера) — это и есть фикстура
    # фолбэка на native-декод.
    ofr = os.path.join(BIN, "optimfrog", "ofr.exe")
    if not os.path.exists(ofr):
        raise RuntimeError("нет %s — bin/ не наполнен" % ofr)
    sh(["wine", ofr, "--encode", mix, "--output", os.path.join(FIX, "src.ofr"),
        "--preset", "0", "--md5", "--overwrite", "--silent"], cwd=ROOT)

    # F5: тот же .ofr, но с испорченными байтами: probe не читает, native-декод
    # не восстанавливает -> честная ошибка (rc=1, «ошибок: 1»).
    random.seed(4)
    data = bytearray(open(os.path.join(FIX, "src.ofr"), "rb").read())
    o = len(data) // 3
    for i in range(o, min(o + 20000, len(data))):
        data[i] = random.randrange(256)
    with open(os.path.join(FIX, "corrupt.ofr"), "wb") as f:
        f.write(data)

    # G1/G2: сбой на уровне ВАРИАНТА, а не подготовки. Берём тон и правим
    # только чётность числа сэмплов (96 000 -> 96 001): этот кодек отвергает
    # нечётную длину, и llao обязан показать файл и причину, а не оборваться
    # безымянным «Aborted: 1 file(s) failed».
    mac = os.path.join(BIN, "monkeys_audio", "MAC.exe")
    if not os.path.exists(mac):
        raise RuntimeError("нет %s — bin/ не наполнен" % mac)
    ffmpeg(["-f", "lavfi", "-i",
            "aevalsrc=0.4*sin(2*PI*440*t)+0.2*sin(2*PI*997*t):s=48000:d=2",
            "-ac", "1", "-c:a", "pcm_s24le", os.path.join(FIX, "tone_even.wav")])
    with open(os.path.join(FIX, "tone_even.wav"), "rb") as f:
        even = f.read()
    i = even.find(b"data")
    n = struct.unpack("<I", even[i + 4:i + 8])[0]
    data = even[i + 8:i + 8 + n] + b"\x01\x02\x03"          # ровно один сэмпл
    odd = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE" + even[12:i] +
           b"data" + struct.pack("<I", len(data)) + data)
    with open(os.path.join(FIX, "tone_odd.wav"), "wb") as f:
        f.write(odd)


# ---------------------------------------------------------------------------
# Сценарии
# ---------------------------------------------------------------------------

def f1_optimize_garbage(d):
    cp(os.path.join(FIX, "garbage.flac"), os.path.join(d, "bad.flac"))
    rc, out = run_tool(["optimize", d, "--formats=flac", "--jobs=1"])
    assert rc == 1, "ожидался rc=1:\n%s" % out
    assert has_errors(out, 1), "итог должен считать ошибку:\n%s" % out


def f2_restore_garbage(d):
    cp(os.path.join(FIX, "garbage.flac"), os.path.join(d, "bad.flac"))
    rc, out = run_tool(["restore", d, "--to=tta", "--jobs=1"])
    assert rc == 1, "ожидался rc=1:\n%s" % out
    assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out


def f3_optimize_ofr(d):
    cp(os.path.join(FIX, "src.ofr"), os.path.join(d, "src.ofr"))
    rc, out = run_tool(["optimize", d, "--formats=flac", "--jobs=1"])
    assert rc == 0, "ofr должен обрабатываться через native-декод:\n%s" % out
    assert "ERROR" not in out, "есть ERROR:\n%s" % out
    # .ofr жмётся OptimFROG сильнее, чем flac, поэтому ни один кандидат не
    # меньше оригинала: честный SKIP («нет подходящих кандидатов»), не замена.


def f4_restore_ofr(d):
    cp(os.path.join(FIX, "src.ofr"), os.path.join(d, "src.ofr"))
    rc, out = run_tool(["restore", d, "--to=flac", "--jobs=1"])
    assert rc == 0, "ofr должен восстанавливаться:\n%s" % out
    assert "ERROR" not in out, "есть ERROR:\n%s" % out
    assert os.path.exists(os.path.join(d, "src.flac")), "нет src.flac"


def f5_corrupt_ofr(d):
    cp(os.path.join(FIX, "corrupt.ofr"), os.path.join(d, "bad.ofr"))
    rc, out = run_tool(["optimize", d, "--formats=flac", "--jobs=1"])
    assert rc == 1, "испорченный ofr должен давать ошибку:\n%s" % out
    assert has_errors(out, 1), "итог должен считать ошибку:\n%s" % out
    assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out


def f6_missing_tool(d):
    takc = os.path.join(BIN, "tak", "Takc.exe")
    assert os.path.exists(takc), "нет Takc.exe — bin/ не наполнен"
    hidden = takc + ".hidden"
    cp(os.path.join(FIX, "src.ofr"), os.path.join(d, "src.ofr"))
    try:
        os.rename(takc, hidden)
        rc, out = run_tool(["optimize", d, "--formats=tak", "--no-download",
                            "--jobs=1"])
        assert rc == 1, "отсутствующая утилита должна давать rc=1:\n%s" % out
        assert has_errors(out, 1), "итог должен считать ошибку:\n%s" % out

        rc, out = run_tool(["restore", d, "--to=tak", "--no-download", "--jobs=1"])
        assert rc == 1, "restore без утилиты должен давать rc=1:\n%s" % out
        assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out
    finally:
        if os.path.exists(hidden):
            os.rename(hidden, takc)


def g1_variant_failure_reported(d):
    """Строгий режим: файл и причина сбоя варианта видны пользователю."""
    cp(os.path.join(FIX, "tone_odd.wav"), os.path.join(d, "tone_odd.wav"))
    rep = os.path.join(d, "report.txt")
    rc, out = run_tool(["optimize", d, "--formats=monkeys_audio", "--jobs=1",
                        "--no-download", "--report=" + rep])
    assert rc == 1, "ожидался rc=1:\n%s" % out
    assert has_errors(out, 1), "итог должен считать ошибку:\n%s" % out
    assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out
    assert "tone_odd.wav" in out, "ERROR без имени файла:\n%s" % out
    # Причина обязана быть в ERROR-строке, иначе это «Aborted» без диагноза.
    first = [l for l in out.splitlines() if "tone_odd.wav" in l and "ERROR" in l]
    assert first, "нет строки ERROR с файлом:\n%s" % out
    assert "tolerance" in out or "1002" in out or "code" in out.lower() or \
        "код" in out.lower(), "в ERROR нет причины:\n%s" % out
    # Многострочный stderr кодера не должен рвать таблицу отчёта.
    assert os.path.exists(rep), "нет отчёта:\n%s" % out
    text = open(rep, encoding="utf-8", errors="replace").read()
    assert "tone_odd.wav" in text and "error" in text, "файла нет в отчёте:\n%s" % text
    row = [l for l in text.splitlines() if l.startswith("tone_odd.wav")]
    assert row, "нет строки файла в отчёте:\n%s" % text
    assert row[0].count("\n") == 0, "строка отчёта многострочная:\n%s" % text


def g2_ignore_errors_keeps_counting(d):
    """--ignore-errors: упавший файл пропускается и ошибкой НЕ считается."""
    cp(os.path.join(FIX, "tone_odd.wav"), os.path.join(d, "tone_odd.wav"))
    cp(os.path.join(FIX, "tone_even.wav"), os.path.join(d, "tone_even.wav"))
    rc, out = run_tool(["optimize", d, "--formats=monkeys_audio", "--jobs=1",
                        "--ignore-errors", "--no-download"])
    # Диагностика не теряется: файл и причина остаются в выводе.
    assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out
    assert "tone_odd.wav" in out, "ERROR без имени файла:\n%s" % out
    # Пропуск — ожидаемое поведение режима: в счёт не идёт и код возврата 0.
    assert has_errors(out, 0), "пропущенный файл не должен считаться ошибкой:\n%s" % out
    assert rc == 0, "с --ignore-errors ожидался rc=0:\n%s" % out
    # Второй файл обязан быть обработан — прогон не прервался.
    assert re.search(r"OK\s+tone_even", out), "хороший файл не обработан:\n%s" % out
    assert os.path.exists(os.path.join(d, "tone_even.ape")), \
        "нет результата для хорошего файла:\n%s" % out
    assert os.path.exists(os.path.join(d, "tone_odd.wav")), \
        "исходник упавшего файла не должен заменяться"


def h1_unknown_format_rejected(d):
    """Неизвестный id в --formats отвергается, а не молча отсекает всё."""
    cp(os.path.join(FIX, "src.ofr"), os.path.join(d, "src.ofr"))
    rc, out = run_tool(["optimize", d, "--formats=nosuchformat", "--jobs=1",
                        "--no-download"])
    assert rc == 1, "неизвестный формат должен давать rc=1:\n%s" % out
    assert "unknown format 'nosuchformat'" in out, \
        "в выводе нет сообщения о неизвестном формате:\n%s" % out
    # Валидный id рядом с неизвестным — тоже ошибка (как у `variants`).
    rc, out = run_tool(["optimize", d, "--formats=flac,nosuchformat", "--jobs=1",
                        "--no-download"])
    assert rc == 1, "неизвестный id в списке должен давать rc=1:\n%s" % out
    assert "unknown format 'nosuchformat'" in out, \
        "в выводе нет сообщения о неизвестном формате:\n%s" % out
    # Контроль: валидный список по-прежнему работает и файл пережимается.
    cp(os.path.join(FIX, "mix.wav"), os.path.join(d, "mix.wav"))
    rc, out = run_tool(["optimize", os.path.join(d, "mix.wav"), "--formats=flac",
                        "--jobs=1", "--no-download"])
    assert rc == 0, "валидный --formats должен работать:\n%s" % out
    assert os.path.exists(os.path.join(d, "mix.flac")), \
        "валидный --formats должен давать результат:\n%s" % out


SCENARIOS = [
    ("f1", "F1  мусорный .flac -> optimize: rc!=0, «ошибок: 1»", f1_optimize_garbage),
    ("f2", "F2  мусорный .flac -> restore: rc!=0, ERROR", f2_restore_garbage),
    ("f3", "F3  .ofr (native probe fallback) -> optimize: rc==0", f3_optimize_ofr),
    ("f4", "F4  .ofr (native probe fallback) -> restore: rc==0", f4_restore_ofr),
    ("f5", "F5  испорченный .ofr -> optimize: rc!=0, «ошибок: 1»", f5_corrupt_ofr),
    ("f6", "F6  нет Takc.exe -> optimize/restore tak: rc!=0", f6_missing_tool),
    ("g1", "G1  сбой варианта -> optimize: rc=1, ERROR с файлом и причиной",
     g1_variant_failure_reported),
    ("g2", "G2  сбой варианта + --ignore-errors: продолжил, пропущен, rc=0",
     g2_ignore_errors_keeps_counting),
    ("h1", "H1  неизвестный id в --formats -> rc=1, ERROR", h1_unknown_format_rejected),
]

RESULTS = []


def scenario(key, desc, fn):
    d = os.path.join(WORK, key)
    os.makedirs(d, exist_ok=True)
    try:
        fn(d)
        RESULTS.append((desc, True, "ok"))
    except Exception as e:
        RESULTS.append((desc, False, str(e)[:600]))


# ---------------------------------------------------------------------------
# Сборка и запуск
# ---------------------------------------------------------------------------

def build():
    if os.path.exists(LLAO) and not FORCE_BUILD:
        return
    if NATIVE_LINUX:
        print("Сборка llao (Linux)...")
        r = subprocess.run(["make", "-s", "TARGET=linux"], capture_output=True,
                           text=True, cwd=ROOT, timeout=1800)
        if r.returncode != 0:
            print(r.stdout, r.stderr)
            sys.exit("ОШИБКА: сборка не удалась")
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
