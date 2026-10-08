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
    G5  зависший кодировщик -> optimize: убит детектором зависания, rc=1
    G6  режим прав исходного файла не меняется (chmod по симлинку бил цель)
    G7  read-only .flac конвертируется: кандидат не наследует 0444
    G2  то же с --ignore-errors: прогон продолжается, упавший файл помечен, но
        пропущенные файлы ошибкой не считаются («ошибок: 0», rc=0)
    H1  неизвестный id в --formats -> rc=1, «unknown format», без перебора
    H2  24-битное моно с нечётным числом сэмплов кодируется: файл должен
        сжаться, а не отвергнуться. Пропускается, пока в
        formats/monkeys_audio.json не выставлен признак features.odd_sample_count

Запуск:
    python3 tests/test_errors.py          # полный прогон
    python3 tests/test_errors.py --keep   # не удалять scratch_err
    python3 tests/test_errors.py --build  # пересобрать llao.exe

Требования: wine, ffmpeg/ffprobe в PATH, llao.exe (соберётся сам), bin/ наполнен.
"""
import glob
import json
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

# Тестируем то, что собрано последним: нативный linux-бинарник приоритетнее
# windows-сборки. Раньше выбор был «llao.exe есть — значит wine», и устаревший
# llao.exe (собранный до правок) молча перебивал свежий llao-linux: тесты про
# новую схему статистики падали на старом коде, а S11 насчитывал 152 записи
# вместо двух.
def _pick_native():
    if not os.path.isfile(LLAO_NATIVE):
        return False
    if not os.path.isfile(LLAO_EXE):
        return True
    return os.path.getmtime(LLAO_NATIVE) >= os.path.getmtime(LLAO_EXE)


NATIVE_LINUX = _pick_native()
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
    # Статистика пишется рядом с бинарником, то есть в рабочую базу
    # пользователя. Тестовые прогоны (в том числе с подменой кодировщика,
    # которая портит рейтинг форматов) не должны туда попадать. С 2.4.0 их три:
    # журнал stats.jsonl, итоговая таблица stats.tsv и отладочный дамп.
    env["LLAO_STATS_FILE"] = os.path.join(WORK, "stats.json")
    env["LLAO_STATS_JOURNAL"] = os.path.join(WORK, "stats.jsonl")
    env["LLAO_STATS_TSV"] = os.path.join(WORK, "stats.tsv")
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


def stub_encoder(d, enabled=True):
    """Подменяет кодировщик APE на Takc.exe, чтобы вызвать отказ варианта.

    Раньше триггером был известный дефект кодировщика APE (24-битное моно с
    нечётным числом сэмплов), и тесты G1/G2 держались на нём. Опора на чужой
    дефект не годится: когда он исчезает, тесты проходят вхолостую, проверяя не
    то. Отказ варианта вызывается подменой бинарника — независимо от версии
    кодека. Takc.exe на аргументах кодера печатает в stderr «Command line
    error: invalid mode» и выходит с кодом 1, не создавая файл, — ровно то, что
    делает отказавший кодировщик.

    Возвращает функцию восстановления (безопаснее try/finally вручную).
    """
    mac = os.path.join(BIN, "monkeys_audio", "MAC.exe")
    takc = os.path.join(BIN, "tak", "Takc.exe")
    backup = os.path.join(d, "MAC.exe.backup")
    if not enabled:
        return lambda: None
    assert os.path.exists(mac), "нет %s — bin/ не наполнен" % mac
    assert os.path.exists(takc), "нет %s — bin/ не наполнен" % takc
    cp(mac, backup)
    cp(takc, mac)

    def restore():
        if os.path.exists(backup):
            shutil.copy2(backup, mac)
            os.remove(backup)
    return restore


def g1_variant_failure_reported(d):
    """Строгий режим: файл и причина сбоя варианта видны пользователю."""
    cp(os.path.join(FIX, "tone_even.wav"), os.path.join(d, "tone.wav"))
    rep = os.path.join(d, "report.txt")
    restore = stub_encoder(d)
    try:
        rc, out = run_tool(["optimize", d, "--formats=monkeys_audio", "--jobs=1",
                            "--no-download", "--report=" + rep])
    finally:
        restore()
    assert rc == 1, "ожидался rc=1:\n%s" % out
    assert has_errors(out, 1), "итог должен считать ошибку:\n%s" % out
    assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out
    assert "tone.wav" in out, "ERROR без имени файла:\n%s" % out
    # Причина обязана быть в ERROR-строке, иначе это «Aborted» без диагноза.
    first = [l for l in out.splitlines() if "tone.wav" in l and "ERROR" in l]
    assert first, "нет строки ERROR с файлом:\n%s" % out
    assert "Command line error" in out, "в выводе нет причины от кодировщика:\n%s" % out
    # Многострочный stderr кодера не должен рвать таблицу отчёта.
    assert os.path.exists(rep), "нет отчёта:\n%s" % out
    text = open(rep, encoding="utf-8", errors="replace").read()
    assert "tone.wav" in text and "error" in text, "файла нет в отчёте:\n%s" % text
    row = [l for l in text.splitlines() if l.startswith("tone.wav")]
    assert row, "нет строки файла в отчёте:\n%s" % text
    assert row[0].count("\n") == 0, "строка отчёта многострочная:\n%s" % text


def g2_ignore_errors_keeps_counting(d):
    """--ignore-errors: упавший файл пропускается и ошибкой НЕ считается.

    Кодировщик подменён заглушкой, поэтому отказуются все варианты обоих файлов.
    Проверяется именно контракт режима: прогон не прерывается на первом отказе
    (оба файла обработаны, а не только первый), пропуск не считается ошибкой,
    исходники не заменяются, код возврата 0.
    """
    cp(os.path.join(FIX, "tone_odd.wav"), os.path.join(d, "tone_odd.wav"))
    cp(os.path.join(FIX, "tone_even.wav"), os.path.join(d, "tone_even.wav"))
    restore = stub_encoder(d)
    try:
        rc, out = run_tool(["optimize", d, "--formats=monkeys_audio", "--jobs=1",
                            "--ignore-errors", "--no-download"])
    finally:
        restore()
    # Диагностика не теряется: каждый файл и причина остаются в выводе.
    for name in ("tone_odd.wav", "tone_even.wav"):
        assert name in out, "ERROR без имени файла %s:\n%s" % (name, out)
    assert out.count("Command line error") >= 2, \
        "причина от кодировщика должна быть у обоих файлов:\n%s" % out
    # Оба файла обработаны — прогон не прервался на первом отказе.
    assert "Files: 2" in out, "обработано не 2 файла:\n%s" % out
    assert "Done: 2 files processed" in out, "итог не 2 файла:\n%s" % out
    # Пропуск — ожидаемое поведение режима: в счёт не идёт и код возврата 0.
    assert has_errors(out, 0), "пропущенные файлы не должны считаться ошибкой:\n%s" % out
    assert rc == 0, "с --ignore-errors ожидался rc=0:\n%s" % out
    # Исходники не заменяются.
    for name in ("tone_odd.wav", "tone_even.wav"):
        assert os.path.exists(os.path.join(d, name)), \
            "исходник %s не должен заменяться:\n%s" % (name, out)


def g3_strict_one_bad_variant_fails_file(d):
    """Строгий режим: сбой ОДНОГО варианта из нескольких перечёркивает файл.

    Регрессия, найденная на живой библиотеке: сбой варианта считался
    блокирующим только при verify=all, поэтому при verify=winner (дефолт
    демона) файл выходил ok с красной точкой на сломанном варианте. Смысл
    режима без --ignore-errors: дыры в проверке быть не должно — пока хоть
    один вариант не проверен, файл отдавать нельзя (сломанный мог бы оказаться
    победителем).

    Берём два формата: один кодер отказывает (подмена бинарника), второй
    отрабатывает штатно. Проверяем, что файл ушёл в ошибку, хотя пригодный
    вариант был и был меньше исходника.
    """
    cp(os.path.join(FIX, "tone_even.wav"), os.path.join(d, "mix.wav"))
    restore = stub_encoder(d)
    try:
        rc, out = run_tool(["optimize", d, "--formats=monkeys_audio,flac",
                            "--jobs=1", "--no-download", "--verify=winner"])
    finally:
        restore()
    assert rc == 1, "сбой одного варианта должен перечеркнуть файл, rc=1:\n%s" % out
    assert has_errors(out, 1), "файл должен посчитан как ошибка:\n%s" % out
    assert "mix.wav" in out, "ERROR без имени файла:\n%s" % out
    assert "Command line error" in out, "нет причины от кодировщика:\n%s" % out
    # Исходник на месте: файл не отдан.
    assert os.path.exists(os.path.join(d, "mix.wav")), \
        "при отказе файла исходник должен остаться:\n%s" % out
    # Главное: результата НЕТ ни от одного формата.
    leftovers = [f for f in os.listdir(d)
                 if f.lower().endswith((".ape", ".flac", ".ofr", ".wav")) and f != "mix.wav"]
    assert not leftovers, "при сбое варианта файл не должен отдаваться: %s\n%s" % (leftovers, out)


def g4_strict_does_not_stop_other_files(d):
    """Граница режима: сбой относится к файлу, а не к прогону.

    Два разных контракта, оба зафиксированы:

    * CLI `optimize` (без флага --mode=daemon) по первой ошибке варианта
      останавливает прогон целиком. Это историческое поведение optimize, на
      нём держатся скрипты: упал кодер — дальше идти бессмысленно, про
      остальные файлы не известно ничего. Ошибка при этом видна и разборчива:
      в отчёте и в выводе есть имя файла и причина, а не только «Aborted: N».
    * Демон (--mode=daemon) НИКОГДА не роняет очередь: ошибка помечает один
      файл, остальные обязаны доехать. Проверяется в test_daemon_*.

    Здесь фиксируется именно CLI-контракт: ошибка видна пофайлово, а прогон
    после неё действительно прекращается.
    """
    cp(os.path.join(FIX, "tone_odd.wav"), os.path.join(d, "odd.wav"))
    cp(os.path.join(FIX, "tone_even.wav"), os.path.join(d, "even.wav"))
    rep = os.path.join(d, "report.txt")
    restore = stub_encoder(d)
    try:
        rc, out = run_tool(["optimize", d, "--formats=monkeys_audio", "--jobs=1",
                            "--no-download", "--report=" + rep])
    finally:
        restore()
    assert rc != 0, "CLI в строгом режиме обязан вернуть ненулевой код:\n%s" % out
    assert has_errors(out, 1), "ошибка должна быть посчитана:\n%s" % out
    # Пофайловая диагностика обязана сохраниться: «Aborted» без имени файла
    # и без причины — ровно тот дефект, который этот течет поймал.
    first = [l for l in out.splitlines() if ".wav" in l and "ERROR" in l]
    assert first, "нет строки ERROR с именем файла:\n%s" % out
    assert "Command line error" in out, "в выводе нет причины от кодировщика:\n%s" % out
    text = open(rep, encoding="utf-8", errors="replace").read()
    assert ".wav" in text and "error" in text, \
        "файла нет в отчёте (диагностика не потеряна при аборте):\n%s" % text


def codec_accepts_odd_length(fmt_id="monkeys_audio"):
    """Читает из formats/<id>.json признак «принимает нечётное число сэмплов».

    Признак ставится разработчиком по факту проверки кодека, а не выводится из
    номера версии: он отвечает на вопрос о свойстве, а не о том, где мы были.
    Пока признак не выставлен, сценарий H2 честно пропускается.
    """
    cfg = os.path.join(ROOT, "formats", fmt_id + ".json")
    try:
        with open(cfg, encoding="utf-8") as f:
            fmt = json.load(f)
    except (OSError, ValueError):
        return False
    return bool(fmt.get("features", {}).get("odd_sample_count", False))


def h2_odd_length_compresses(d):
    """24-битное моно с нечётным числом сэмплов обязано кодироваться.

    Регрессия на известный дефект энкодера: он отвергал такой файл с
    Error: 1002. Дефект сообщён автору кодека, признак в formats/<id>.json
    выставляется, когда версия кодека его больше не воспроизводит, — тогда
    сценарий начинает проверять по-настоящему.
    """
    if not codec_accepts_odd_length():
        return "пропущен: в formats/monkeys_audio.json не выставлен признак " \
               "features.odd_sample_count — кодек ещё отвергает нечётную длину"
    cp(os.path.join(FIX, "tone_odd.wav"), os.path.join(d, "tone_odd.wav"))
    rc, out = run_tool(["optimize", d, "--formats=monkeys_audio", "--jobs=1",
                        "--no-download"])
    assert rc == 0, "нечётная длина обязана кодироваться:\n%s" % out
    assert "ERROR" not in out, "ошибка на файле с нечётной длиной:\n%s" % out
    assert has_errors(out, 0), "ошибок быть не должно:\n%s" % out
    assert os.path.exists(os.path.join(d, "tone_odd.ape")), \
        "нет результата для файла с нечётной длиной:\n%s" % out


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


def g5_hung_encoder_is_killed(d):
    """Зависший кодировщик должен быть убит, а не висеть вечно.

    Регресс: детектор зависания мерял CPU через getrusage(RUSAGE_CHILDREN),
    то есть по СУММЕ всех детей процесса. Демон запускает кодировщики пачками,
    поэтому сумма всегда росла, счётчик обнулялся на каждом опросе, и зависший
    кодировщик жил бесконечно: на живой библиотеке это 52 процесса la.exe по
    11 минут, файл навсегда в running, очередь стоит.

    Триггер — настоящая заглушка вместо кодека: она не пишет выходной файл и
    не жжёт CPU. Ставится через PATH (без маркера .binary), поэтому бинарники в
    bin/ не трогаются.

    Сценарий только для нативной сборки. Заглушка — скрипт /bin/sh, а
    tool::in_path() на Windows-сборке отбрасывает всё, что не PE, и встроенный
    flac.exe подменять нельзя: он нужен остальным сценариям и в bin/ не
    возвращается надёжно. Ветка Windows в proc.cpp не менялась (там
    GetProcessTimes по своему процессу), так что её проверяет другой сценарий.
    """
    if not NATIVE_LINUX:
        return ("пропуск на Windows-сборке: заглушка-kill нельзя подсунуть через "
                "PATH — in_path() требует PE, а бинарники в bin/ тест не трогает")
    src = os.path.join(FIX, "tone_even.wav")
    assert os.path.exists(src), "нет фикстуры tone_even.wav"
    stub_dir = os.path.join(d, "stubs")
    os.makedirs(stub_dir, exist_ok=True)
    # Имя — как у настоящей утилиты формата, иначе ensure() её не найдёт.
    flac_real = os.path.join(BIN, "flac", "flac.exe")
    assert os.path.exists(flac_real), "нет %s — bin/ не наполнен" % flac_real
    stub = os.path.join(stub_dir, "flac")
    with open(stub, "w") as f:
        f.write("#!/bin/sh\nexec sleep 100000\n")
    os.chmod(stub, 0o755)

    marker = os.path.join(BIN, "flac", ".binary")
    marker_backup = marker + ".hidden"
    had_marker = os.path.exists(marker)
    cp(src, os.path.join(d, "hung.wav"))
    try:
        if had_marker:
            os.rename(marker, marker_backup)
        env_path = stub_dir + os.pathsep + os.environ.get("PATH", "")
        # Таймаут прогона заведомо меньше hard_timeout кодировщика (1800 с): если
        # детектор откажет, тест упадёт по таймауту subprocess, а не провисит
        # полчаса.
        cmd = [LLAO, "optimize", d, "--formats=flac", "--jobs=1", "--no-download"]
        env = dict(os.environ)
        env["WINEDEBUG"] = "-all"
        env["PATH"] = env_path
        env["LLAO_STATS_FILE"] = os.path.join(WORK, "stats.json")
        env["LLAO_STATS_JOURNAL"] = os.path.join(WORK, "stats.jsonl")
        env["LLAO_STATS_TSV"] = os.path.join(WORK, "stats.tsv")
        if not NATIVE_LINUX:
            cmd = ["wine"] + cmd
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT,
                           timeout=420, env=env)
        out = (r.stdout or "") + (r.stderr or "")
        assert "hung.wav" in out, "файл должен быть назван в выводе:\n%s" % out
        assert "stalled" in out.lower(), \
            "зависший кодировщик должен быть убит детектором зависания:\n%s" % out
    finally:
        if had_marker and os.path.exists(marker_backup):
            os.rename(marker_backup, marker)


def g6_source_mode_untouched(d):
    """Движок не имеет права менять права исходного файла.

    Регресс: для декодирования заводится алиас src_link.<ext>, и на него
    вызывался chmod(0444). chmod следует по симлинкам, поэтому права менялись
    у ЦЕЛИ, то есть у файла библиотеки: пользователю оставались файлы, которыми
    он больше не мог управлять (в прогоне на 5000 файлов таких оказалось 19).
    """
    src = os.path.join(FIX, "tone_even.wav")
    assert os.path.exists(src), "нет фикстуры tone_even.wav"
    target = os.path.join(d, "modes.wav")
    cp(src, target)
    os.chmod(target, 0o444)
    before = os.stat(target).st_mode & 0o777
    rc, out = run_tool(["optimize", target, "--formats=flac", "--jobs=1"])
    after = os.stat(target).st_mode & 0o777
    # Режим проверяется на УСПЕШНОЙ конвертации: если кодирование отвалилось,
    # исходник и так останется нетронутым, и проверка станет пустой.
    # Показываем хвост вывода: вердикт («Done: …, errors: …») в конце, а в
    # начале — строки проверки вариантов, которые ничего не объясняют.
    assert rc == 0 and "ERROR" not in out, \
        "нужна успешная конвертация, а не отказ (rc=%d):\n%s" % (rc, out[-1200:])
    assert oct(before) == oct(after), \
        "режим исходника изменился: %s -> %s\n%s" % (oct(before), oct(after), out)


def g7_readonly_flac_converts(d):
    """Read-only .flac должен конвертироваться, а не падать на записи тегов.

    Тот же регресс, но с последствием: flac.exe переносит режим входного файла
    на выходной, кандидат получался 0444, и write_group (O_TRUNC по тому же
    пути) получал EACCES — «could not write FLAC tags» на каждом варианте.
    """
    src = os.path.join(FIX, "src.ofr")
    assert os.path.exists(src), "нет фикстуры src.ofr"
    # Свой каталог сценария надо наполнить самому: restore ищет аудио в d, и
    # на пустом каталоге он честно отвечает «no audio files found». Раньше
    # сценарий рассчитывал на то, что в d что-то осталось от предыдущих
    # прогонов, — на чистом CI такого не было.
    cp(src, os.path.join(d, "seed.ofr"))
    # Готовим read-only FLAC: кодируем заглушкой seed.ofr и убираем права.
    rc, out = run_tool(["restore", d, "--to=flac", "--jobs=1"])
    flacs = [f for f in os.listdir(d) if f.endswith(".flac")]
    assert flacs, "restore должен был дать .flac:\n%s" % out
    target = os.path.join(d, flacs[0])
    os.chmod(target, 0o444)
    out_dir = os.path.join(d, "conv")
    os.makedirs(out_dir, exist_ok=True)
    shutil.copy2(target, os.path.join(out_dir, flacs[0]))
    cp(src, os.path.join(out_dir, "src2.ofr"))
    rc, out = run_tool(["optimize", out_dir, "--formats=flac", "--jobs=1"])
    assert "could not write FLAC tags" not in out, \
        "read-only исходник роняет запись тегов в кандидат:\n%s" % out
    assert "ERROR" not in out, "read-only .flac должен конвертироваться:\n%s" % out


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
    ("g3", "G3  строгий режим: сбой ОДНОГО варианта перечёркивает файл",
     g3_strict_one_bad_variant_fails_file),
    ("g4", "G4  CLI: сбой останавливает прогон, но пофайловая диагностика не теряется",
     g4_strict_does_not_stop_other_files),
    ("h1", "H1  неизвестный id в --formats -> rc=1, ERROR", h1_unknown_format_rejected),
    ("h2", "H2  24-бит моно с нечётной длиной -> кодируется (по признаку кодека)",
     h2_odd_length_compresses),
    ("g5", "G5  зависший кодировщик убивается детектором, а не живёт вечно",
     g5_hung_encoder_is_killed),
    ("g6", "G6  режим прав исходного файла не меняется", g6_source_mode_untouched),
    ("g7", "G7  read-only .flac конвертируется (регресс на права файла)",
     g7_readonly_flac_converts),
]

RESULTS = []


def scenario(key, desc, fn):
    d = os.path.join(WORK, key)
    os.makedirs(d, exist_ok=True)
    try:
        # Сценарий возвращает строку — это пометка о пропуске, а не ошибка.
        skipped = fn(d)
        RESULTS.append((desc, True, skipped or "ok"))
    except Exception as e:
        # Обрезка была 600 символов — ровно столько, сколько нужно, чтобы
        # потерять вердикт в конце вывода. Символов 2500 хватает, чтобы увидеть
        # и причину, и итог.
        RESULTS.append((desc, False, str(e)[:2500]))


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
        mark = "OK " if ok else "FAIL"
        if ok and detail and detail != "ok":
            mark = "SKIP"
        print("%-72s %s" % (name, mark))
        if mark == "FAIL":
            failed += 1
            print("        " + detail.replace("\n", "\n        "))
        elif mark == "SKIP":
            print("        " + detail)
    print("Итого: %d/%d прошло" % (len(RESULTS) - failed, len(RESULTS)))
    if not KEEP:
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
