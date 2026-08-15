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
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLAO = os.path.join(ROOT, "llao.exe")
WORK = os.path.join(ROOT, "scratch_err")
FIX = os.path.join(WORK, "_fixtures")
BIN = os.path.join(ROOT, "bin")

KEEP = "--keep" in sys.argv
FORCE_BUILD = "--build" in sys.argv


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
    assert "SKIP" not in out, "ofr не должен быть SKIP:\n%s" % out


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


SCENARIOS = [
    ("f1", "F1  мусорный .flac -> optimize: rc!=0, «ошибок: 1»", f1_optimize_garbage),
    ("f2", "F2  мусорный .flac -> restore: rc!=0, ERROR", f2_restore_garbage),
    ("f3", "F3  .ofr (native probe fallback) -> optimize: rc==0", f3_optimize_ofr),
    ("f4", "F4  .ofr (native probe fallback) -> restore: rc==0", f4_restore_ofr),
    ("f5", "F5  испорченный .ofr -> optimize: rc!=0, «ошибок: 1»", f5_corrupt_ofr),
    ("f6", "F6  нет Takc.exe -> optimize/restore tak: rc!=0", f6_missing_tool),
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
