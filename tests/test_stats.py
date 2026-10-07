#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Тесты `llao stats`, журнала и итоговой таблицы.

Хранение статистики разделено на два файла:

  * `stats.jsonl` — журнал, append-only, одна строка на один закрытый файл.
    Это база для разбора: ошибки, тайминги кандидатов, что угодно.
  * `stats.tsv` — итоговая таблица. Выводится из журнала, когда работа
    закончена, а не пишется в процессе. Одна строка на файл, столбцы —
    свойства источника и размер каждой пары format/variant.

Команда `llao stats` читает таблицу, а не журнал: сводке, рейтингу и отчёту
размеры из журнала не нужны, а журнал на большой библиотеке на порядки больше.

Покрытие:
    S1  stats на пустой таблице: rc=0, «нет статистики»
    S2  stats с фикстурой: сводка и рейтинг по выигранным файлам (порядок строк)
    S3  stats --report: файл создан, содержит рейтинг, id форматов и проценты
    S4  stats --report на пустой таблице: rc=1, отчёт не создан
    S5  stats --report в несуществующий каталог: rc=1
    S6  неизвестная опция: rc=2
    S7  LLAO_STATS_TSV перекрывает путь к таблице (иначе читается боевой файл)
    S8  у выигранного файла ровно один победитель, и он совпадает с ячейкой
    S9  рейтинг считает cost (файл + sidecar), сводка сходится с рейтингом
    S10 файл без победителя в рейтинг не попадает
    S11 реальный прогон пишет журнал и таблицу
    S12 lossy-исходник в рейтинг не попадает
    S13 гистограмма экономии: файлы раскладываются по бинам, «вырос» отдельно
    S14 гистограмма размеров: файлы по мощной шкале, бины не перекрываются
    S15 гистограммы в выгрузке --report, в том числе по каждому формату
    S16 журнал — JSONL, ровно одна строка на закрытый файл
    S17 в таблицу попадают только завершённые файлы, ошибочные — нет
    S18 повторный прогон дополняет таблицу: пустые ячейки заполняются
    S19 NA — отсечение по ограничениям кодека, пусто — пара не запускалась
    S20 путь с обратным слэшем и табом переживает экспорт и чтение
    S21 новый формат в конфиге добавляет столбец, существующие не портит
    S22 в таблице есть размер эталонного WAV
    S23 ключ строки — путь результата, а не исходника
    S24 --dump выгружает журнал читаемым JSON
    S25 --export-tsv на пустом журнале: rc=1, файл не создан

Пути переопределяются LLAO_STATS_TSV / LLAO_STATS_JOURNAL / LLAO_STATS_FILE,
поэтому тест не трогает боевые файлы рядом с бинарником.

Запуск:
    python3 tests/test_stats.py          # полный прогон
    python3 tests/test_stats.py --keep   # не удалять scratch_stats
"""
import json
import os
import re
import shutil
import struct
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
# новую схему статистики падали на старом коде.
def _pick_native():
    if not os.path.isfile(LLAO_NATIVE):
        return False
    if not os.path.isfile(LLAO_EXE):
        return True
    return os.path.getmtime(LLAO_NATIVE) >= os.path.getmtime(LLAO_EXE)


NATIVE_LINUX = _pick_native()
LLAO = LLAO_NATIVE if NATIVE_LINUX else LLAO_EXE

MB = 1048576

FIXED_COLUMNS = ["out_path", "run_id", "ts", "runs", "source_format", "codec_name",
                 "source_size", "bits", "channels", "sample_rate", "duration_ms",
                 "has_tags", "wav_size", "winner_format", "winner_variant",
                 "winner_cost", "winner_sidecar"]


# ---------------------------------------------------------------------------
# Фикстуры
# ---------------------------------------------------------------------------

def cand(fmt, var, cost, status="ok", sidecar=0):
    c = {"format": fmt, "variant": var, "task": 0, "status": status,
         "cost": cost, "result_size": cost - sidecar, "sidecar_size": sidecar,
         "has_tags": False, "wall_ms": 10, "cpu_ms": 9,
         "prep_wall_ms": 5, "decode_wall_ms": 6, "verify": "all"}
    # Причина пишется только для не-ok: так её и кладёт движок, и проверка
    # дампа держится на этом правиле.
    if status != "ok":
        c["error"] = "encoder refused"
    return c


def record(path, size, winner_fmt=None, winner_var="", winner_cost=0,
           codec_name="flac", extra=(), status="ok", out_path=None,
           wav_size=0, excluded=(), run_id="test-run"):
    r = {"ts": "2026-10-02T10:00:00Z", "run_id": run_id, "file": path,
         "status": status,
         "source": {"format": "wav", "codec_name": codec_name, "size": size,
                    "channels": 2, "sample_rate": 48000, "bits": 16,
                    "duration": 1.0, "has_tags": False, "wav_size": wav_size},
         "candidates": list(extra),
         "excluded": [{"format": f, "variant": v} for f, v in excluded]}
    if out_path:
        r["out_path"] = out_path
    if winner_fmt:
        r["winner"] = {"format": winner_fmt, "variant": winner_var,
                       "cost": winner_cost}
    return r


# Четыре выигранных файла. Рейтинг по убыванию экономии на выигранных файлах:
# monkeys_audio выиграл два файла (85% и 70%, среднее 77.5%), wavpack один
# (70%), flac один (50%). tta ни разу не выиграла и в рейтинг не попадает.
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
    # Файл не отдан: победителя нет, в рейтинг не должен попасть ни один формат.
    record("/music/e.flac", 10 * MB, status="error",
           extra=[cand("flac", "8", 0, status="error")]),
]

EXPECTED_ORDER = ["monkeys_audio", "wavpack", "flac"]


def table(rows, pairs=None):
    """Собирает таблицу в том виде, в каком её пишет llao.

    rows — словари с ключами фиксированных столбцов (кроме out_path/run_id/ts/
    runs, их значения задаются по умолчанию) и ключом cells: {колонка: значение}.
    Пустое значение в cells — пара не запускалась, "NA" — отсечение по caps.
    """
    pairs = pairs or []
    cols = FIXED_COLUMNS + pairs
    lines = ["# llao stats export v1 | generated 2026-10-02T10:00:00Z | rows %d | runs 1"
             % len(rows), "\t".join(cols)]
    for n, row in enumerate(rows):
        cells = row.get("cells", {})
        won_fmt = row.get("winner_format")
        vals = [row.get("out_path", "/music/f%d.flac" % n),
                row.get("run_id", "test-run"),
                row.get("ts", "2026-10-02T10:00:00Z"),
                "%d" % row.get("runs", 1),
                row.get("source_format", "wav"),
                row.get("codec_name", "flac"),
                "%d" % row.get("source_size", 10 * MB),
                "%d" % row.get("bits", 16),
                "%d" % row.get("channels", 2),
                "%d" % row.get("sample_rate", 48000),
                "%d" % row.get("duration_ms", 1000),
                "1" if row.get("has_tags") else "0",
                "%d" % row.get("wav_size", 0),
                won_fmt or "",
                row.get("winner_variant") or "",
                "" if not won_fmt else "%d" % row.get("winner_cost", 0),
                "" if not won_fmt else "%d" % row.get("winner_sidecar", 0)]
        for p in pairs:
            vals.append(str(cells.get(p, "")))
        lines.append("\t".join(vals))
    return "\n".join(lines) + "\n"


def won(fmt, cost, sidecar=0, **kw):
    """Строка таблицы для выигранного файла с одним кандидатом этой пары."""
    row = {"winner_format": fmt, "winner_variant": cost[0], "winner_cost": cost[1],
           "winner_sidecar": sidecar, "source_size": 10 * MB}
    row.update(kw)
    return row


def parse_tsv(path):
    """Разбирает таблицу в (колонки, строки-словари). Столбец-пара -> значение."""
    lines = [ln for ln in open(path, encoding="utf-8").read().split("\n") if ln]
    lines = [ln for ln in lines if not ln.startswith("#")]
    cols = lines[0].split("\t")
    out = []
    for ln in lines[1:]:
        v = ln.split("\t")
        row = {"cells": {}}
        for i, name in enumerate(cols):
            if i >= len(v):
                continue
            val = v[i]
            if name == "out_path":
                row["out_path"] = unescape(val)
            elif name in ("run_id", "ts", "source_format", "codec_name",
                          "winner_format", "winner_variant"):
                row[name] = val
            elif name == "has_tags":
                row[name] = val == "1"
            elif ":" in name:
                if val == "":
                    continue
                row["cells"][name] = "NA" if val == "NA" else int(val)
            elif val == "":
                row[name] = None
            else:
                row[name] = int(val)
        out.append(row)
    return cols, out


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            c = s[i + 1]
            out.append({"t": "\t", "r": "\r", "n": "\n"}.get(c, c))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


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


def run_tool(args, tsv=None, journal=None, dump=None, timeout=600):
    cmd = [LLAO] + args if NATIVE_LINUX else ["wine", LLAO] + args
    env = dict(os.environ)
    env["WINEDEBUG"] = "-all"
    for name, value in (("LLAO_STATS_TSV", tsv), ("LLAO_STATS_JOURNAL", journal),
                        ("LLAO_STATS_FILE", dump)):
        if value:
            env[name] = value
        else:
            env.pop(name, None)
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT,
                       timeout=timeout, env=env)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def out_dir():
    d = os.path.join(WORK, "out")
    os.makedirs(d, exist_ok=True)
    return d


def write_tsv(name, text):
    p = os.path.join(out_dir(), name)
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    return p


def write_journal(name, records):
    p = os.path.join(out_dir(), name)
    with open(p, "w", encoding="utf-8") as f:
        for r in records:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    return p


def base_tsv():
    """Таблица, эквивалентная фикстуре RECORDS после экспорта."""
    return write_tsv("base.tsv", table([
        won("monkeys_audio", ("c3000", 1500000), out_path="/music/a.flac",
            cells={"monkeys_audio:c3000": 1500000, "wavpack:x1": 3000000,
                   "flac:8": 2600000}),
        won("monkeys_audio", ("c3000", 3000000), out_path="/music/b.flac",
            cells={"monkeys_audio:c3000": 3000000, "wavpack:x1": 3200000}),
        won("wavpack", ("x1", 3000000), out_path="/music/c.flac",
            cells={"wavpack:x1": 3000000, "flac:8": 5000000}),
        won("flac", ("8", 5000000), out_path="/music/d.flac",
            cells={"flac:8": 5000000}),
    ], pairs=["flac:8", "monkeys_audio:c3000", "wavpack:x1"]))


RESULTS = []


def scenario(name, fn):
    try:
        fn()
        RESULTS.append((name, True, ""))
    except AssertionError as exc:
        RESULTS.append((name, False, str(exc)))
    except Exception as exc:  # noqa: BLE001 — в тесте это просто «ошибка сценария»
        RESULTS.append((name, False, "%s: %s" % (type(exc).__name__, exc)))


# ---------------------------------------------------------------------------
# Сводка и отчёт по таблице
# ---------------------------------------------------------------------------

def s1_empty_base():
    p = write_tsv("empty.tsv", "")
    rc, out = run_tool(["stats"], tsv=p)
    assert rc == 0, "пустая таблица должна давать rc=0:\n%s" % out
    assert "No statistics yet" in out, "нет сообщения об отсутствии статистики:\n%s" % out


def s2_summary_with_records():
    p = base_tsv()
    rc, out = run_tool(["stats"], tsv=p)
    assert rc == 0, "сводка должна давать rc=0:\n%s" % out
    assert "Files: 4" in out, "нет числа файлов:\n%s" % out
    assert "Files replaced: 4" in out, "неверное число победителей:\n%s" % out
    assert "Runs: 1" in out, "прогоны не сгруппированы:\n%s" % out
    # Рейтинг только по выигранным файлам, порядок — по убыванию экономии.
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
    p = base_tsv()
    dest = os.path.join(out_dir(), "report.txt")
    if os.path.exists(dest):
        os.remove(dest)
    rc, out = run_tool(["stats", "--report=" + dest], tsv=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    assert os.path.exists(dest), "файл отчёта не создан:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    assert "Format ranking" in text, "в отчёте нет рейтинга:\n%s" % text
    for fmt in EXPECTED_ORDER:
        assert fmt in text, "в отчёте нет формата %s:\n%s" % (fmt, text)
    assert "Files: 4" in text, "в отчёте нет числа файлов:\n%s" % text
    assert "%.2f" % (expected_savings("monkeys_audio") * 100) in text, \
        "в отчёте нет средней экономии по выигранным файлам:\n%s" % text
    assert "files-won" in text, "в отчёте нет колонки выигранных файлов:\n%s" % text
    # Отчёт отдаётся наружу (автору кодека) — локализации в нём быть не должно,
    # чтобы его можно было прочитать без перевода.
    assert not any(ord(ch) > 0x400 and ord(ch) < 0x500 for ch in text), \
        "в отчёте неожиданная кириллица:\n%s" % text


def s4_report_empty_base():
    p = write_tsv("empty2.tsv", "")
    dest = os.path.join(out_dir(), "empty-report.txt")
    if os.path.exists(dest):
        os.remove(dest)
    rc, out = run_tool(["stats", "--report=" + dest], tsv=p)
    assert rc == 1, "пустая таблица с --report должна давать rc=1:\n%s" % out
    assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out
    assert not os.path.exists(dest), "отчёт без данных создан не должен быть"


def s5_report_unwritable():
    p = base_tsv()
    dest = "/llao-nonexistent-dir/report.txt"
    rc, out = run_tool(["stats", "--report=" + dest], tsv=p)
    assert rc == 1, "неписаемый путь должен давать rc=1:\n%s" % out
    assert "ERROR" in out, "в выводе нет ERROR:\n%s" % out


def s6_unknown_option():
    p = base_tsv()
    rc, out = run_tool(["stats", "--bogus"], tsv=p)
    assert rc == 2, "неизвестная опция должна давать rc=2:\n%s" % out
    assert "unknown option" in out, "в выводе нет сообщения об опции:\n%s" % out


def s7_env_override_used():
    """Без LLAO_STATS_TSV читается файл рядом с бинарником (боевой)."""
    p = base_tsv()
    rc, out = run_tool(["stats"], tsv=None)
    if not os.path.exists(os.path.join(ROOT, "stats.tsv")):
        assert rc == 0 and "No statistics yet" in out, \
            "без оверрайда и без боевого файла — пустая сводка:\n%s" % out
        return
    assert "Files:" in out, \
        "без оверрайда прочитана боевая таблица, а не фикстура — " \
        "проверь LLAO_STATS_TSV:\n%s" % out
    # С оверрайдом читается именно фикстура.
    rc, out = run_tool(["stats"], tsv=p)
    assert "Files: 4" in out, "оверрайд не подхватился:\n%s" % out


def s8_one_winner_per_file():
    """У выигранного файла ровно один победитель, и он совпадает с ячейкой пары."""
    p = base_tsv()
    rc, out = run_tool(["stats", "--report=" + os.path.join(out_dir(), "r8.txt")], tsv=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    _cols, rows = parse_tsv(p)
    won_rows = [r for r in rows if r["winner_format"]]
    assert len(won_rows) == len(rows), "не у всех строк есть победитель:\n%s" % rows
    for r in won_rows:
        assert r["winner_cost"] > 0, "победитель с нулевым cost:\n%s" % r
        key = "%s:%s" % (r["winner_format"], r["winner_variant"])
        assert r["cells"].get(key) == r["winner_cost"], \
            "cost победителя разошёлся с ячейкой %s:\n%s" % (key, r)


def s9_ranking_uses_cost_with_sidecar():
    """Рейтинг и сводка считают одно и то же: cost = файл + sidecar."""
    p = write_tsv("s9.tsv", table([
        won("wavpack", ("x1", 1000000), out_path="/music/s.flac", source_size=10 * MB,
            cells={"wavpack:x1": 1000000}),
        won("flac", ("8", 5000000), out_path="/music/t.flac", source_size=10 * MB,
            cells={"flac:8": 5000000}),
    ], pairs=["flac:8", "wavpack:x1"]))
    dest = os.path.join(out_dir(), "r9.txt")
    rc, out = run_tool(["stats", "--report=" + dest], tsv=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    # Экономия wavpack считается по cost (1.0 МБ из 10 МБ), а sidecar в неё входит.
    expect_wv = 1.0 - 1000000 / float(10 * MB)
    assert "%.2f" % (expect_wv * 100) in text, "экономия без учёта sidecar:\n%s" % text
    # Сводка обязана считать ту же величину, что и рейтинг: обе суммы по cost.
    tot_out = 1000000 + 5000000
    expect_tot = 100.0 * tot_out / (20.0 * MB)
    assert "%.2f%% of source" % expect_tot in text, \
        "сводка разошлась с рейтингом:\n%s" % text
    rc, out = run_tool(["stats"], tsv=p)
    assert "%.2f%%" % expect_tot in out, \
        "сводка в терминале разошлась с отчётом:\n%s" % out


def s10_row_without_winner_not_ranked():
    """Файл, который не отдан, не даёт формату победу."""
    p = write_tsv("s10.tsv", table([
        {"out_path": "/music/e.flac", "source_size": 10 * MB, "winner_format": None,
         "cells": {"flac:8": 1000000}},
    ], pairs=["flac:8"]))
    dest = os.path.join(out_dir(), "r10.txt")
    rc, out = run_tool(["stats", "--report=" + dest], tsv=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    assert "Format ranking" not in text, \
        "формат без побед не должен появляться в рейтинге:\n%s" % text
    assert "Files replaced: 0" in text, "неотданный файл посчитан побеждой:\n%s" % text


def s11_optimize_writes_journal_and_table():
    """Реальный прогон: журнал построчный, таблица построена, победитель указан."""
    src = os.path.join(WORK, "src")
    os.makedirs(src, exist_ok=True)
    import math
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

    rundir = os.path.join(WORK, "run")
    os.makedirs(rundir, exist_ok=True)
    journal = os.path.join(rundir, "stats.jsonl")
    tsv = os.path.join(rundir, "stats.tsv")
    for p in (journal, tsv):
        if os.path.exists(p):
            os.remove(p)
    # Формат зафиксирован: сценарий проверяет статистику, а не то, какие кодеки
    # в этой машине работают. Полный перебор вариантов добавил бы сюда отказы
    # чужих кодеков и делал бы тест зависимым от окружения.
    rc, out = run_tool(["optimize", src, "--jobs=2", "--no-download",
                        "--formats=flac,wavpack,tak"],
                       journal=journal, tsv=tsv)
    assert rc == 0, "прогон должен удаться:\n%s" % out

    assert os.path.exists(journal), "журнал не создан:\n%s" % out
    lines = [ln for ln in open(journal, encoding="utf-8").read().split("\n") if ln]
    assert len(lines) == 2, "ожидались 2 строки журнала (по одной на файл), получено %d:\n%s" % (
        len(lines), out)
    recs = [json.loads(ln) for ln in lines]
    for r in recs:
        assert r["status"] == "ok", "неуспешный статус:\n%s" % r
        assert r.get("winner"), "нет победителя:\n%s" % r
        assert r["run_id"], "нет run_id:\n%s" % r
        assert r["ts"], "нет метки времени:\n%s" % r
        assert r["source"]["size"] > 0, "нет размера исходника:\n%s" % r
        assert r["source"]["wav_size"] > 0, "не записан размер эталонного WAV:\n%s" % r
        assert r.get("out_path"), "не записан путь результата:\n%s" % r
        assert len(r["candidates"]) > 1, "кандидаты не сохранены:\n%s" % r
        best = min(c["cost"] for c in r["candidates"] if c["status"] == "ok")
        assert r["winner"]["cost"] <= best, \
            "победитель дороже лучшего кандидата:\n%s" % r

    assert os.path.exists(tsv), "итоговая таблица не создана:\n%s" % out
    cols, rows = parse_tsv(tsv)
    assert len(rows) == 2, "в таблице %d строк вместо 2:\n%s" % (len(rows), cols)
    for r in rows:
        assert r["wav_size"] > 0, "в таблице нет размера WAV:\n%s" % r
        assert r["winner_format"], "в таблице нет победителя:\n%s" % r
        assert r["winner_cost"] > 0, "нулевой cost победителя:\n%s" % r


def s12_lossy_source_not_ranked():
    """Lossy-исходник в рейтинг не попадает: конвертация в lossless не экономит."""
    p = write_tsv("s12.tsv", table([
        won("flac", ("8", 5000000), out_path="/music/l.flac", codec_name="mp3"),
        won("wavpack", ("x1", 3000000), out_path="/music/n.flac", codec_name="ape"),
    ], pairs=["flac:8", "wavpack:x1"]))
    dest = os.path.join(out_dir(), "r12.txt")
    rc, out = run_tool(["stats", "--report=" + dest], tsv=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    assert "wavpack" in text, "lossless-исходник должен попасть в рейтинг:\n%s" % text
    ranking_part = text.split("Format ranking", 1)[-1].split("Savings distribution", 1)[0]
    assert "flac" not in ranking_part, "lossy-исходник попал в рейтинг:\n%s" % text


def s13_savings_histogram():
    p = write_tsv("s13.tsv", table([
        # 90%, 80%, 70%, 60% экономии
        won("flac", ("8", 1000000), out_path="/music/h1.flac", source_size=10 * MB),
        won("flac", ("8", 2000000), out_path="/music/h2.flac", source_size=10 * MB),
        won("flac", ("8", 3000000), out_path="/music/h3.flac", source_size=10 * MB),
        won("flac", ("8", 4000000), out_path="/music/h4.flac", source_size=10 * MB),
    ], pairs=["flac:8"]))
    rc, out = run_tool(["stats"], tsv=p)
    assert rc == 0, "сводка должна давать rc=0:\n%s" % out
    h = parse_hist(out, "Savings distribution (files won):")
    assert h.get("90-100%") == 1, "90-100%%: %r\n%s" % (h, out)
    assert h.get("80-90%") == 1, "80-90%%: %r\n%s" % (h, out)
    assert h.get("70-80%") == 1, "70-80%%: %r\n%s" % (h, out)
    assert h.get("60-70%") == 1, "60-70%%: %r\n%s" % (h, out)
    assert h.get("grew", 0) == 0, "лишний класс «вырос»:\n%s" % out


def s14_size_histogram():
    p = write_tsv("s14.tsv", table([
        won("flac", ("8", 100000), out_path="/music/s1.flac", source_size=500 * 1024),
        won("flac", ("8", 200000), out_path="/music/s2.flac", source_size=2 * MB),
        won("flac", ("8", 300000), out_path="/music/s3.flac", source_size=40 * MB),
        won("flac", ("8", 400000), out_path="/music/s4.flac", source_size=200 * MB),
    ], pairs=["flac:8"]))
    rc, out = run_tool(["stats"], tsv=p)
    assert rc == 0, "сводка должна давать rc=0:\n%s" % out
    h = parse_hist(out, "Source size distribution:")
    assert h.get("<1 MB") == 1, "<1 MB: %r\n%s" % (h, out)
    assert h.get("2-3 MB") == 1, "2-3 MB: %r\n%s" % (h, out)
    assert h.get("32-63 MB") == 1, "32-63 MB: %r\n%s" % (h, out)
    assert h.get("64+ MB") == 1, "64+ MB: %r\n%s" % (h, out)


def s15_histograms_in_report():
    p = base_tsv()
    dest = os.path.join(out_dir(), "r15.txt")
    rc, out = run_tool(["stats", "--report=" + dest], tsv=p)
    assert rc == 0, "--report должен давать rc=0:\n%s" % out
    text = open(dest, encoding="utf-8").read()
    assert "Savings distribution (files won):" in text, "нет общей гистограммы:\n%s" % text
    assert "Source size distribution:" in text, "нет гистограммы размеров:\n%s" % text
    for fmt in EXPECTED_ORDER:
        assert "Savings distribution: %s" % fmt in text, \
            "нет гистограммы по формату %s:\n%s" % (fmt, text)


# ---------------------------------------------------------------------------
# Журнал и экспорт таблицы
# ---------------------------------------------------------------------------

def s16_journal_is_jsonl():
    p = write_journal("s16.jsonl", RECORDS)
    with open(p, encoding="utf-8") as f:
        lines = [ln for ln in f.read().split("\n") if ln]
    assert len(lines) == len(RECORDS), "строка журнала на одну запись, а не на файл"
    for ln in lines:
        obj = json.loads(ln)
        assert isinstance(obj, dict) and "candidates" in obj, \
            "строка журнала не разбирается как запись:\n%s" % ln


def export(name, records, extra=()):
    j = write_journal(name + ".jsonl", records)
    t = os.path.join(out_dir(), name + ".tsv")
    rc, out = run_tool(["stats", "--export-tsv=" + t], journal=j)
    assert rc == 0, "--export-tsv должен давать rc=0:\n%s" % out
    assert os.path.exists(t), "таблица не создана:\n%s" % out
    return parse_tsv(t)


def s17_only_finished_files_in_table():
    """В таблице только файлы с итоговым статусом ok: незавершённое — в журнале."""
    _cols, rows = export("s17", RECORDS)
    paths = [r["out_path"] for r in rows]
    assert "/music/e.flac" not in paths, \
        "файл с ошибкой попал в таблицу итогов:\n%s" % paths
    assert len(rows) == 4, "ожидались 4 завершённых файла, получено %d:\n%s" % (len(rows), paths)


def s18_second_run_fills_cells():
    """Повторный прогон дополняет таблицу: было «пусто», стало число."""
    first = [
        record("/music/a.flac", 10 * MB, "flac", "8", 2600000,
               extra=[cand("flac", "8", 2600000)], out_path="/music/a.flac"),
    ]
    cols, rows = export("s18a", first)
    assert rows[0]["cells"].get("flac:8") == 2600000, "первый прогон:\n%s" % rows
    assert rows[0]["cells"].get("alac:extreme") is None, \
        "незапущенная пара должна быть пустой, а не нулём:\n%s" % rows

    # Второй прогон того же файла: alac теперь отдаёт результат, flac повторён.
    second = [
        record("/music/a.flac", 10 * MB, "flac", "8", 2600000,
               extra=[cand("flac", "8", 2600000), cand("alac", "extreme", 2500000)],
               out_path="/music/a.flac", run_id="run-2"),
    ]
    _cols, rows2 = export("s18b", first + second)
    assert len(rows2) == 1, "один файл должен остаться одной строкой:\n%s" % rows2
    assert rows2[0]["cells"].get("alac:extreme") == 2500000, \
        "новая ячейка не заполнилась:\n%s" % rows2
    assert rows2[0]["cells"].get("flac:8") == 2600000, \
        "существующая ячейка затерлась:\n%s" % rows2
    assert rows2[0]["runs"] == 2, "число прогонов не посчитано:\n%s" % rows2


def s19_na_for_caps_exclusion():
    """NA — кодек отсечён по своим ограничениям, пусто — пара не запускалась."""
    recs = [
        record("/music/a.flac", 10 * MB, "flac", "8", 2600000,
               extra=[cand("flac", "8", 2600000)],
               excluded=[("la", "extreme"), ("la", "default")],
               out_path="/music/a.flac"),
    ]
    cols, rows = export("s19", recs)
    pairs = [c for c in cols if ":" in c]
    assert "la:default" in pairs and "la:extreme" in pairs, \
        "отсечённые кодеком пары должны стать столбцами: %r" % pairs
    assert "tta:default" not in pairs, "незапускавшийся кодек не должен попасть в столбцы"
    cells = rows[0]["cells"]
    assert cells.get("la:extreme") == "NA", "отсечение должно быть NA:\n%s" % cells
    assert cells.get("la:default") == "NA", "отсечение должно быть NA:\n%s" % cells
    assert cells.get("flac:8") == 2600000, "обычная ячейка должна быть числом:\n%s" % cells


def s20_escaping_in_paths():
    """Путь может содержать обратный слэш и таб: без экранирования он бы разъехался."""
    weird = "Z:\\mnt\\music\t album\\01 track.ape"
    recs = [
        record(weird, 10 * MB, "flac", "8", 2600000,
               extra=[cand("flac", "8", 2600000)], out_path=weird),
    ]
    _cols, rows = export("s20", recs)
    assert len(rows) == 1, "строка не прочиталась после экранирования:\n%s" % rows
    assert rows[0]["out_path"] == weird, \
        "путь не восстановился: %r != %r" % (rows[0]["out_path"], weird)
    assert rows[0]["winner_cost"] == 2600000, "строка съехала по столбцам:\n%s" % rows


def s21_new_column_grows():
    """Новый формат в конфиге добавляет столбец, существующие значения не портит."""
    first = [record("/music/a.flac", 10 * MB, "flac", "8", 2600000,
                    extra=[cand("flac", "8", 2600000)], out_path="/music/a.flac")]
    _cols, rows = export("s21a", first)
    assert [c for c in _cols if ":" in c] == ["flac:8"], "столбцы до расширения: %r" % _cols

    second = [record("/music/a.flac", 10 * MB, "flac", "8", 2600000,
                     extra=[cand("flac", "8", 2600000), cand("tak", "max", 2900000)],
                     out_path="/music/a.flac", run_id="run-2")]
    cols, rows2 = export("s21b", first + second)
    pairs = [c for c in cols if ":" in c]
    assert pairs == ["flac:8", "tak:max"], "порядок столбцов-пар нарушен: %r" % pairs
    assert rows2[0]["cells"].get("tak:max") == 2900000, "новая пара не записана:\n%s" % rows2
    assert rows2[0]["cells"].get("flac:8") == 2600000, \
        "старое значение не уцелело после расширения:\n%s" % rows2


def s22_variant_order_numeric():
    """Столбцы одного кодека идут по числовому порядку вариантов: 8 раньше 12."""
    recs = [
        record("/music/a.flac", 10 * MB, "flac", "12", 2000000,
               extra=[cand("flac", "12", 2000000), cand("flac", "8", 2600000),
                      cand("flac", "3", 3000000), cand("alac", "extreme", 2500000)],
               out_path="/music/a.flac"),
    ]
    cols, _rows = export("s22", recs)
    pairs = [c for c in cols if ":" in c]
    assert pairs == ["alac:extreme", "flac:3", "flac:8", "flac:12"], \
        "порядок столбцов неверный: %r" % pairs


def s23_key_is_out_path():
    """Ключ строки — путь результата: исходник после отдачи на диске не остаётся."""
    recs = [
        record("/music/01 song.ape", 10 * MB, "flac", "8", 2600000,
               extra=[cand("flac", "8", 2600000)],
               out_path="/music/01 song.flac"),
    ]
    _cols, rows = export("s23", recs)
    assert rows[0]["out_path"] == "/music/01 song.flac", \
        "ключ строки должен быть путём результата:\n%s" % rows
    # Повтор того же файла, но уже по новому пути-источнику: это другой ключ, но
    # тот же файл на диске — значит строки склеятся по out_path.
    recs2 = recs + [
        record("/music/01 song.flac", 10 * MB, "flac", "8", 2600000,
               extra=[cand("flac", "8", 2600000)],
               out_path="/music/01 song.flac"),
    ]
    _cols, rows2 = export("s23b", recs2)
    assert len(rows2) == 1, "один файл на диске должен дать одну строку:\n%s" % rows2
    assert rows2[0]["runs"] == 2, "повтор должен считаться прогоном:\n%s" % rows2


def s24_dump_readable_json():
    j = write_journal("s24.jsonl", RECORDS)
    dest = os.path.join(out_dir(), "s24-dump.json")
    rc, out = run_tool(["stats", "--dump=" + dest], journal=j)
    assert rc == 0, "--dump должен давать rc=0:\n%s" % out
    assert os.path.exists(dest), "дамп не создан:\n%s" % out
    data = json.load(open(dest, encoding="utf-8"))
    assert isinstance(data, list) and len(data) == len(RECORDS), \
        "дамп должен быть JSON-массивом всех записей журнала"
    # В дампе есть то, чего в таблице нет: ошибки кандидатов и тайминги.
    with_err = [r for r in data for c in r["candidates"] if c.get("error")]
    assert with_err, "в дампе потерялись ошибки кандидатов"
    assert any(c.get("cpu_ms") is not None for r in data for c in r["candidates"]), \
        "в дампе потерялись тайминги кандидатов"


def s25_export_empty_journal():
    j = write_journal("s25.jsonl", [])
    t = os.path.join(out_dir(), "s25.tsv")
    rc, out = run_tool(["stats", "--export-tsv=" + t], journal=j)
    assert rc == 1, "пустой журнал должен давать rc=1:\n%s" % out
    assert not os.path.exists(t), "таблица без данных создана не должна быть"


SCENARIOS = [
    ("S1 stats на пустой таблице", s1_empty_base),
    ("S2 сводка и рейтинг", s2_summary_with_records),
    ("S3 --report", s3_report_written),
    ("S4 --report на пустой таблице", s4_report_empty_base),
    ("S5 --report в несуществующий каталог", s5_report_unwritable),
    ("S6 неизвестная опция", s6_unknown_option),
    ("S7 LLAO_STATS_TSV", s7_env_override_used),
    ("S8 один победитель на файл", s8_one_winner_per_file),
    ("S9 рейтинг по cost с sidecar", s9_ranking_uses_cost_with_sidecar),
    ("S10 файл без победителя не в рейтинге", s10_row_without_winner_not_ranked),
    ("S11 реальный прогон пишет журнал и таблицу", s11_optimize_writes_journal_and_table),
    ("S12 lossy-исходник не в рейтинге", s12_lossy_source_not_ranked),
    ("S13 гистограмма экономии", s13_savings_histogram),
    ("S14 гистограмма размеров", s14_size_histogram),
    ("S15 гистограммы в отчёте", s15_histograms_in_report),
    ("S16 журнал — JSONL", s16_journal_is_jsonl),
    ("S17 в таблицу только завершённые", s17_only_finished_files_in_table),
    ("S18 повторный прогон дополняет таблицу", s18_second_run_fills_cells),
    ("S19 NA по ограничениям кодека", s19_na_for_caps_exclusion),
    ("S20 экранирование путей", s20_escaping_in_paths),
    ("S21 рост числа столбцов", s21_new_column_grows),
    ("S22 порядок вариантов в столбцах", s22_variant_order_numeric),
    ("S23 ключ строки — путь результата", s23_key_is_out_path),
    ("S24 --dump читаемым JSON", s24_dump_readable_json),
    ("S25 --export-tsv на пустом журнале", s25_export_empty_journal),
]


def main():
    if os.path.exists(WORK) and not KEEP:
        shutil.rmtree(WORK)
    os.makedirs(WORK, exist_ok=True)
    for name, fn in SCENARIOS:
        scenario(name, fn)
    ok = sum(1 for _, good, _ in RESULTS if good)
    for name, good, msg in RESULTS:
        print("%s %s" % ("PASS" if good else "FAIL", name))
        if not good:
            for ln in msg.split("\n"):
                print("    " + ln)
    print("\n%d/%d passed" % (ok, len(RESULTS)))
    if not KEEP:
        shutil.rmtree(WORK, ignore_errors=True)
    return 0 if ok == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
