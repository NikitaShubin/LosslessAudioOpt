#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Тесты `llao stats` и выгрузки отчёта `--report`.

`stats.json` — локальная накопительная статистика: каждым кандидатом пополняется
запись, из неё же строится порядок перебора. Команда `llao stats` показывает
сводку в терминале, а `llao stats --report=<file>` выгружает тот же рейтинг
текстовой таблицей — её удобно отдать автору кодека как есть (в отчёте только
id форматов, числа и проценты, локализации нет).

Запись в базе — один обработанный файл со всеми кандидатами (см. stats::Record),
победитель записан явно в поле winner.

Покрытие:
    S1  stats на пустой базе: rc=0, «нет статистики»
    S2  stats с фикстурой: сводка и рейтинг по выигранным файлам (порядок строк)
    S3  stats --report: файл создан, содержит рейтинг, id форматов и проценты
    S4  stats --report на пустой базе: rc=1, отчёт не создан
    S5  stats --report в несуществующий каталог: rc=1
    S6  неизвестная опция: rc=2
    S7  LLAO_STATS_FILE перекрывает путь к stats.json (иначе файл читается
        рядом с бинарником — это боевой файл пользователя)
    S8  один файл = одна запись: у выигранного файла ровно один победитель
    S9  рейтинг считает cost (файл + sidecar), сводка сходится с рейтингом
    S10 запись без winner (файл не отдан) в рейтинг не попадает
    S11 прогон optimize пишет ровно одну запись на файл и указывает победителя
    S12 lossy-исходник в рейтинг не попадает (конвертация в lossless не экономит)
    S13 гистограмма экономии: файлы раскладываются по бинам, «вырос» отдельно
    S14 гистограмма размеров: файлы по мощной шкале, бины не перекрываются
    S15 гистограммы в выгрузке --report, в том числе по каждому формату

Путь к базе переопределяется переменной LLAO_STATS_FILE, поэтому тест не
трогает настоящий stats.json рядом с бинарником.

Запуск:
    python3 tests/test_stats.py          # полный прогон
    python3 tests/test_stats.py --keep   # не удалять scratch_stats
"""
import json
import re
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLAO_EXE = os.path.join(ROOT, "llao.exe")
LLAO_NATIVE = os.path.join(ROOT, "llao-linux")
WORK = os.path.join(ROOT, "scratch_stats")

KEEP = "--keep" in sys.argv
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

def cand(fmt, var, cost, status="ok", sidecar=0):
    return {"format": fmt, "variant": var, "task": 0, "status": status,
            "cost": cost, "result_size": cost - sidecar, "sidecar_size": sidecar,
            "has_tags": False, "wall_ms": 10, "cpu_ms": 9,
            "prep_wall_ms": 5, "decode_wall_ms": 6, "verify": "all"}


def record(path, size, winner_fmt=None, winner_var="", winner_cost=0,
           codec_name="flac", extra=(), status="ok"):
    r = {"ts": "2026-10-02T10:00:00Z", "run_id": "test-run", "file": path,
         "status": status,
         "source": {"format": "wav", "codec_name": codec_name, "size": size,
                    "channels": 2, "sample_rate": 48000, "bits": 16,
                    "duration": 1.0, "has_tags": False},
         "candidates": list(extra)}
    if winner_fmt:
        r["winner"] = {"format": winner_fmt, "variant": winner_var,
                       "cost": winner_cost}
    return r


MB = 1048576

# Фикстура: четыре выигранных файла. Рейтинг по убыванию экономии на выигранных
# файлах: monkeys_audio выиграл два файла (85% и 70%, среднее 77.5%), wavpack
# один (70%), flac один (50%). tta ни разу не выиграла и в рейтинг не попадает.
RECORDS = [
    record("/music/a.flac", 10 * MB, "monkeys_audio", "c3000", 1500000,
           extra=[cand("monkeys_audio", "c3000", 1500000),
                  cand("wavpack", "x1", 3000000),
                  cand("flac", "8", 2600000),
                  cand("tta", "default", 0, status="error")]),
    record("/music/b.flac", 10 * MB, "monkeys_audio", "c3000", 3000000,
           extra=[cand("monkeys_audio", "c3000", 3000000),
                  cand("wavpack", "x1", 3200000)]),
    record("/music/c.flac", 10 * MB, "wavpack", "x1", 3000000,
           extra=[cand("wavpack", "x1", 3000000),
                  cand("flac", "8", 5000000)]),
    record("/music/d.flac", 10 * MB, "flac", "8", 5000000,
           extra=[cand("flac", "8", 5000000)]),
    # Файл не отдан: победителя нет, в рейтинг не должен попасть ни один формат
    record("/music/e.flac", 10 * MB, status="error",
           extra=[cand("flac", "8", 0, status="error")]),
]

EXPECTED_ORDER = ["monkeys_audio", "wavpack", "flac"]


def parse_hist(text, title):
    """Разбирает блок гистограммы в словарь {метка бина: число файлов}."""
    rows = {}
    started = False
    for ln in text.splitlines():
        if ln.strip() == title:
            started = True
            continue
        if not started:
            continue
        if not ln.strip():
            break
        m = re.match(r"^\s+(\S[^ ]*(?: [^ ]+)?)\s+(\d+)\s*(#*)\s*$", ln)
        if m:
            rows[m.group(1).strip()] = int(m.group(2))
        elif rows:
            break
    return rows


def expected_savings(fmt):
    """Средняя экономия формата по выигранным файлам — эталон для тестов."""
    tot = 0.0
    n = 0
    for r in RECORDS:
        w = r.get("winner")
        if not w or w["format"] != fmt:
            continue
        tot += 1.0 - w["cost"] / float(r["source"]["size"])
        n += 1
    return tot / n if n else 0.0


def run_tool(args, stats_file=None, timeout=600):
    cmd = [LLAO] + args if NATIVE_LINUX else ["wine", LLAO] + args
    env = dict(os.environ)
    env["WINEDEBUG"] = "-all"
    if stats_file:
        env["LLAO_STATS_FILE"] = stats_file
    else:
        env.pop("LLAO_STATS_FILE", None)
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT,
                       timeout=timeout, env=env)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def base(records):
    d = os.path.join(WORK, "base")
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "stats.json")
    with open(p, "w", encoding="utf-8") as f:
        json.dump(records, f)
    return p


def out_dir():
    d = os.path.join(WORK, "out")
    os.makedirs(d, exist_ok=True)
    return d


RESULTS = []


def scenario(name, fn):
    try:
        fn()
        RESULTS.append((name, True, ""))
    except AssertionError as exc:
        RESULTS.append((name, False, str(exc)))
    except Exception as exc:  # noqa: BLE001 — в тесте это просто «ошибка сценария»
        RESULTS.append((name, False, "%s: %s" % (type(exc).__name__, exc)))


def s1_empty_base():
    empty = os.path.join(out_dir(), "empty.json")
    open(empty, "w").close()
    rc, out = run_tool(["stats"], stats_file=empty)
    assert rc == 0, "пустая база должна давать rc=0:\n%s" % out
    assert "No statistics yet" in out, "нет сообщения об отсутствии статистики:\n%s" % out


def s2_summary_with_records():
    p = base(RECORDS)
    rc, out = run_tool(["stats"], stats_file=p)
    assert rc == 0, "сводка должна давать rc=0:\n%s" % out
    assert "Total records: 5" in out, "нет числа записей:\n%s" % out
    assert "Files replaced: 4" in out, "неверное число победителей:\n%s" % out
    assert "By status:" in out and "error" in out, "нет разбивки по статусам:\n%s" % out
    assert "Runs: 1" in out, "прогоны не сгруппированы:\n%s" % out
    # Рейтинг только по успешным кандидатам, порядок — по убыванию экономии.
    ranked = [ln.split()[0] for ln in out.splitlines()
              if ln.strip().startswith(("monkeys_audio", "wavpack", "flac", "tta"))
              and "%" in ln]
    assert len(ranked) == len(EXPECTED_ORDER), \
        "в рейтинге лишние/пропущенные форматы: %r\n%s" % (ranked, out)
    assert ranked == EXPECTED_ORDER, "неверный порядок рейтинга: %r\n%s" % (ranked, out)
    # monkeys_audio выиграла два файла, средняя экономия считается по ним.
    # Ожидаемое значение вычисляем из фикстуры, а не вписываем: иначе смена
    # чисел в фикстуре тихо ломала бы тест или, наоборот, проходила не по делу.
    expect = expected_savings("monkeys_audio")
    for ln in out.splitlines():
        if ln.strip().startswith("monkeys_audio") and "%" in ln:
            assert "%.2f" % (expect * 100) in ln, \
                "неверная средняя экономия по выигранным файлам (ждали %.2f%%):\n%s" % (
                    expect * 100, out)
            break
    else:
        raise AssertionError("нет строки рейтинга monkeys_audio:\n%s" % out)
    for ln in out.splitlines():
        if ln.strip().startswith("tta") and "%" in ln:
            raise AssertionError("tta без побед не должна быть в рейтинге:\n%s" % out)


def s3_report_written():
    p = base(RECORDS)
    dest = os.path.join(out_dir(), "report.txt")
    if os.path.exists(dest):
        os.remove(dest)
    rc, out = run_tool(["stats", "--report=" + dest], stats_file=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    assert os.path.exists(dest), "файл отчёта не создан:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    assert "Format ranking" in text, "в отчёте нет рейтинга:\n%s" % text
    for fmt in EXPECTED_ORDER:
        assert fmt in text, "в отчёте нет формата %s:\n%s" % (fmt, text)
    assert "Records: 5" in text, "в отчёте нет числа записей:\n%s" % text
    assert "%.2f" % (expected_savings("monkeys_audio") * 100) in text, \
        "в отчёте нет средней экономии по выигранным файлам:\n%s" % text
    assert "files-won" in text, "в отчёте нет колонки выигранных файлов:\n%s" % text
    # Отчёт отдаётся наружу (автору кодека) — локализации в нём быть не должно,
    # чтобы его можно было прочитать без перевода.
    assert not any(ord(ch) > 0x400 and ord(ch) < 0x500 for ch in text), \
        "в отчёте неожиданная кириллица:\n%s" % text


def s4_report_empty_base():
    empty = os.path.join(out_dir(), "empty2.json")
    open(empty, "w").close()
    dest = os.path.join(out_dir(), "empty-report.txt")
    if os.path.exists(dest):
        os.remove(dest)
    rc, out = run_tool(["stats", "--report=" + dest], stats_file=empty)
    assert rc == 1, "пустая база с --report должна давать rc=1:\n%s" % out
    assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out
    assert not os.path.exists(dest), "отчёт без данных создан не должен быть"


def s5_report_unwritable():
    p = base(RECORDS)
    dest = "/llao-nonexistent-dir/report.txt"
    rc, out = run_tool(["stats", "--report=" + dest], stats_file=p)
    assert rc == 1, "неписаемый путь должен давать rc=1:\n%s" % out
    assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out


def s6_unknown_option():
    p = base(RECORDS)
    rc, out = run_tool(["stats", "--bogus"], stats_file=p)
    assert rc == 2, "неизвестная опция должна давать rc=2:\n%s" % out
    assert "unknown option" in out, "в выводе нет сообщения об опции:\n%s" % out


def s7_env_override_used():
    """Без LLAO_STATS_FILE читается файл рядом с бинарником (боевой)."""
    p = base(RECORDS)
    rc, out = run_tool(["stats"], stats_file=None)
    if not os.path.exists(os.path.join(ROOT, "stats.json")):
        assert rc == 0 and "No statistics yet" in out, \
            "без оверрайда и без боевого файла — пустая сводка:\n%s" % out
        return
    assert "Total records" in out, \
        "без оверрайда прочитан боевой stats.json, а не фикстура — " \
        "проверь LLAO_STATS_FILE:\n%s" % out
    # С оверрайдом читается именно фикстура.
    rc, out = run_tool(["stats"], stats_file=p)
    assert "Total records: 5" in out, "оверрайд не подхватился:\n%s" % out


def s8_one_record_per_file():
    """У выигранного файла ровно один победитель, и он совпадает с cost кандидата."""
    p = base(RECORDS)
    rc, out = run_tool(["stats", "--report=" + os.path.join(out_dir(), "r8.txt")],
                       stats_file=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    with open(p, encoding="utf-8") as f:
        recs = json.load(f)
    won = [r for r in recs if r.get("winner")]
    assert len(won) == 4, "победитель должен быть ровно у %d файлов, а не у %d" % (
        len(won), sum(1 for r in recs if r.get("winner")))
    for r in won:
        assert r["winner"]["cost"] > 0, "победитель с нулевым cost:\n%s" % r
        match = [c for c in r["candidates"]
                 if c["format"] == r["winner"]["format"]
                 and c["variant"] == r["winner"]["variant"]]
        assert match, "победитель %s/%s не лежит среди кандидатов:\n%s" % (
            r["winner"]["format"], r["winner"]["variant"], r)
        assert match[0]["cost"] == r["winner"]["cost"], \
            "cost победителя разошёлся с кандидатом:\n%s" % r


def s9_ranking_uses_cost_with_sidecar():
    """Рейтинг и сводка считают одно и то же: cost = файл + sidecar."""
    recs = [
        record("/music/s.flac", 10 * MB, "wavpack", "x1", 1000000,
               extra=[cand("wavpack", "x1", 1000000, sidecar=900000)]),
        record("/music/t.flac", 10 * MB, "flac", "8", 5000000,
               extra=[cand("flac", "8", 5000000)]),
    ]
    p = base(recs)
    dest = os.path.join(out_dir(), "r9.txt")
    rc, out = run_tool(["stats", "--report=" + dest], stats_file=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    # Экономия wavpack считается по cost (1.0 МБ из 10 МБ), а sidecar в неё входит.
    # Проверяем именно это: если бы рейтинг считал по result_size, получилось бы
    # 10%, потому что result_size здесь 100 000 байт.
    expect_wv = 1.0 - 1000000 / float(10 * MB)
    assert "%.2f" % (expect_wv * 100) in text, \
        "экономия без учёта sidecar:\n%s" % text
    # Сводка обязана считать ту же величину, что и рейтинг: обе суммы по cost.
    tot_out = 1000000 + 5000000
    expect_tot = 100.0 * tot_out / (20.0 * MB)
    assert "%.2f%% of source" % expect_tot in text, \
        "сводка разошлась с рейтингом:\n%s" % text
    rc, out = run_tool(["stats"], stats_file=p)
    assert "%.2f%%" % expect_tot in out, \
        "сводка в терминале разошлась с отчётом:\n%s" % out


def s10_record_without_winner_not_ranked():
    """Файл, который не отдан, не даёт формату победу."""
    recs = [record("/music/e.flac", 10 * MB, status="error",
                   extra=[cand("flac", "8", 0, status="error")])]
    p = base(recs)
    dest = os.path.join(out_dir(), "r10.txt")
    rc, out = run_tool(["stats", "--report=" + dest], stats_file=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    assert "Format ranking" not in text, \
        "формат без побед не должен появляться в рейтинге:\n%s" % text
    assert "Files replaced: 0" in text, "неотданный файл посчитан побеждой:\n%s" % text


def s11_optimize_writes_one_record_per_file():
    """Реальный прогон: одна запись на файл, победитель указан."""
    import math
    import struct
    src = os.path.join(WORK, "src")
    os.makedirs(src, exist_ok=True)
    n, ch, bps = 24000, 2, 2
    data = bytearray()
    for i in range(n * ch):
        v = int(9000 * math.sin(2 * math.pi * 440 * i / 48000))
        data += int(v).to_bytes(bps, "little", signed=True)
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " +
           struct.pack("<IHHIIHH", 16, 1, ch, 48000, 48000 * ch * bps, ch * bps, 16) +
           b"data" + struct.pack("<I", len(data)))
    for name in ("a.wav", "b.wav"):
        with open(os.path.join(src, name), "wb") as f:
            f.write(hdr + bytes(data))

    stats_file = os.path.join(WORK, "run", "stats.json")
    os.makedirs(os.path.dirname(stats_file), exist_ok=True)
    rc, out = run_tool(["optimize", src, "--jobs=2", "--no-download"], stats_file=stats_file)
    assert rc == 0, "прогон должен удаться:\n%s" % out
    with open(stats_file, encoding="utf-8") as f:
        recs = json.load(f)
    assert len(recs) == 2, "ожидались 2 записи (по одной на файл), получено %d:\n%s" % (
        len(recs), out)
    for r in recs:
        assert r["status"] == "ok", "неуспешный статус:\n%s" % r
        assert r.get("winner"), "нет победителя:\n%s" % r
        assert r["run_id"], "нет run_id:\n%s" % r
        assert r["ts"], "нет метки времени:\n%s" % r
        assert r["source"]["size"] > 0, "нет размера исходника:\n%s" % r
        assert len(r["candidates"]) > 1, "кандидаты не сохранены:\n%s" % r
        best = min(c["cost"] for c in r["candidates"] if c["status"] == "ok")
        assert r["winner"]["cost"] <= best, \
            "победитель дороже лучшего кандидата:\n%s" % r


def s12_lossy_source_not_ranked():
    """Lossy-исходник в рейтинг не попадает: конвертация в lossless не экономит."""
    recs = [
        record("/music/l.mp3", 10 * MB, "flac", "8", 12000000, codec_name="mp3",
               extra=[cand("flac", "8", 12000000)]),
        record("/music/n.flac", 10 * MB, "flac", "8", 5000000, codec_name="flac",
               extra=[cand("flac", "8", 5000000)]),
    ]
    p = base(recs)
    dest = os.path.join(out_dir(), "r12.txt")
    rc, out = run_tool(["stats", "--report=" + dest], stats_file=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    # Учитывается только flac-исходник: 5.0 МБ из 10 МБ.
    expect = 100.0 * 5000000 / (10.0 * MB)
    assert "Files replaced: 1" in text, "посчитан файл без отданного победителя:\n%s" % text
    assert "%.2f%% of source" % expect in text, \
        "сводка не должна учитывать mp3-исходник:\n%s" % text
    assert "files-won" in text, "нет заголовка рейтинга:\n%s" % text
    expect_rank = 1.0 - 5000000 / float(10 * MB)
    for ln in text.splitlines():
        if ln.strip().startswith("flac") and "%" in ln:
            assert "%.2f" % (expect_rank * 100) in ln, \
                "в рейтинг попал mp3-исходник:\n%s" % text


def s13_savings_histogram():
    """Экономия раскладывается по бинам; выросшие файлы — отдельный бин."""
    # 15% → 10-20%, 35% → 30-40%, 95% → 90-100%, 120% → «grew».
    recs = [
        record("/music/a.flac", 100, "flac", "8", 85, extra=[cand("flac", "8", 85)]),
        record("/music/b.flac", 100, "flac", "8", 65, extra=[cand("flac", "8", 65)]),
        record("/music/c.flac", 100, "flac", "8", 5, extra=[cand("flac", "8", 5)]),
        record("/music/d.flac", 100, "flac", "8", 120,
               extra=[cand("flac", "8", 120)]),
    ]
    p = base(recs)
    dest = os.path.join(out_dir(), "r13.txt")
    rc, out = run_tool(["stats", "--report=" + dest], stats_file=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    assert "Savings distribution (files won)" in text, "нет гистограммы экономии:\n%s" % text
    rows = parse_hist(text, "Savings distribution (files won):")
    assert rows["10-20%"] == 1, "15%% должен лежать в 10-20%%: %r\n%s" % (rows, text)
    assert rows["30-40%"] == 1, "35%% должен лежать в 30-40%%: %r\n%s" % (rows, text)
    assert rows["90-100%"] == 1, "95%% должен лежать в 90-100%%: %r\n%s" % (rows, text)
    assert rows["grew"] == 1, "выросший файл должен быть в отдельном бине: %r\n%s" % (rows, text)
    assert sum(rows.values()) == 4, "в бинах не все файлы: %r" % rows
    # Без локализации: отчёт идёт автору кодека.
    assert not any(ord(ch) > 0x400 and ord(ch) < 0x500 for ch in text), \
        "в гистограмме неожиданная кириллица:\n%s" % text


def s14_size_histogram():
    """Размеры по мощной шкале; границы бинов не перекрываются."""
    recs = [
        record("/music/a.flac", 512 * 1024, "flac", "8", 100,
               extra=[cand("flac", "8", 100)]),
        record("/music/b.flac", 3 * MB, "flac", "8", 100, extra=[cand("flac", "8", 100)]),
        record("/music/c.flac", 20 * MB, "flac", "8", 100,
               extra=[cand("flac", "8", 100)]),
        record("/music/d.flac", 500 * MB, "flac", "8", 100,
               extra=[cand("flac", "8", 100)]),
    ]
    p = base(recs)
    dest = os.path.join(out_dir(), "r14.txt")
    rc, out = run_tool(["stats", "--report=" + dest], stats_file=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    assert "Source size distribution" in text, "нет гистограммы размеров:\n%s" % text
    rows = parse_hist(text, "Source size distribution:")
    assert rows["<1 MB"] == 1, "0.5 МБ в первый бин: %r" % rows
    assert rows["2-3 MB"] == 1, "3 МБ в бин 2-3: %r" % rows
    assert rows["16-31 MB"] == 1, "20 МБ в бин 16-31: %r" % rows
    assert rows["64+ MB"] == 1, "500 МБ в открытый бин: %r" % rows
    assert sum(rows.values()) == 4, "в бинах не все файлы: %r" % rows
    # Шкала удваивается без разрыва и без наложения: следующий бин начинается там,
    # где предыдущий кончается. Раньше тут был бин «64-127 MB», накрывавшийся
    # следующим, и файлы от 100 МБ попадали в два бина сразу.
    # Шкала удваивается без разрыва и без наложения. Раньше тут был бин
    # «64-127 MB», накрывавшийся следующим, и файлы от 100 МБ попадали в два
    # бина сразу. Сверяем весь набор меток, а не наличие отдельных.
    expect_labels = ["<1 MB", "2-3 MB", "4-7 MB", "8-15 MB", "16-31 MB",
                     "32-63 MB", "64+ MB"]
    assert list(rows) == expect_labels, \
        "набор бинов размеров разошёлся: %r" % (list(rows),)


def s15_histograms_in_report_per_format():
    """В выгрузке гистограммы по всем файлам и отдельно по каждому формату."""
    recs = [
        record("/music/a.flac", 10 * MB, "flac", "8", 2000000,
               extra=[cand("flac", "8", 2000000)]),
        record("/music/b.wav", 10 * MB, "wavpack", "x1", 8000000,
               extra=[cand("wavpack", "x1", 8000000)]),
    ]
    p = base(recs)
    dest = os.path.join(out_dir(), "r15.txt")
    rc, out = run_tool(["stats", "--report=" + dest], stats_file=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    assert "Savings distribution: flac" in text, "нет гистограммы по flac:\n%s" % text
    assert "Savings distribution: wavpack" in text, \
        "нет гистограммы по wavpack:\n%s" % text
    # 2.0 МБ из 10 = 80%, 8.0 МБ из 10 = 20%. Граница бина включается в верхний:
    # ровно 80% — это «80-90», а не «70-80».
    fl = parse_hist(text, "Savings distribution: flac")
    wv = parse_hist(text, "Savings distribution: wavpack")
    assert fl["80-90%"] == 1, "flac: ровно 80%% должен лежать в 80-90%%: %r" % fl
    assert fl["70-80%"] == 0, "flac: ровно 80%% не должен попасть в 70-80%%: %r" % fl
    assert wv["20-30%"] == 1, "wavpack: ровно 20%% должен лежать в 20-30%%: %r" % wv
    assert sum(fl.values()) == 1 and sum(wv.values()) == 1, \
        "по формату должен быть ровно один файл: %r %r" % (fl, wv)


SCENARIOS = [
    ("S1", "S1  пустая база -> rc=0, «нет статистики»", s1_empty_base),
    ("S2", "S2  сводка: записи, статусы, порядок рейтинга", s2_summary_with_records),
    ("S3", "S3  --report: файл с рейтингом, без локализации", s3_report_written),
    ("S4", "S4  --report на пустой базе -> rc=1, файла нет", s4_report_empty_base),
    ("S5", "S5  --report в несуществующий каталог -> rc=1", s5_report_unwritable),
    ("S6", "S6  неизвестная опция -> rc=2", s6_unknown_option),
    ("S7", "S7  LLAO_STATS_FILE перекрывает путь к базе", s7_env_override_used),
    ("S8", "S8  один файл = одна запись, один победитель", s8_one_record_per_file),
    ("S9", "S9  рейтинг и сводка считают cost (с sidecar)", s9_ranking_uses_cost_with_sidecar),
    ("S10", "S10 файл без победителя не попадает в рейтинг", s10_record_without_winner_not_ranked),
    ("S11", "S11 прогон optimize: одна запись на файл", s11_optimize_writes_one_record_per_file),
    ("S12", "S12 lossy-исходник не попадает в рейтинг", s12_lossy_source_not_ranked),
    ("S13", "S13 гистограмма экономии по бинам", s13_savings_histogram),
    ("S14", "S14 гистограмма размеров, бины не перекрываются", s14_size_histogram),
    ("S15", "S15 гистограммы в отчёте, в том числе по формату",
     s15_histograms_in_report_per_format),
]


def main():
    if not os.path.exists(LLAO):
        sys.exit("ОШИБКА: нет %s (соберите: make && make TARGET=linux)" % LLAO)
    os.makedirs(WORK, exist_ok=True)
    for _, _, fn in SCENARIOS:
        scenario(_, fn)

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
