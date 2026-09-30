#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Тесты `llao stats` и выгрузки отчёта `--report`.

`stats.json` — локальная накопительная статистика: каждым кандидатом пополняется
запись, из неё же строится порядок перебора. Команда `llao stats` показывает
сводку в терминале, а `llao stats --report=<file>` выгружает тот же рейтинг
текстовой таблицей — её удобно отдать автору кодека как есть (в отчёте только
id форматов, числа и проценты, локализации нет).

Покрытие:
    S1  stats на пустой базе: rc=0, «нет статистики»
    S2  stats с фикстурой: сводка и рейтинг по средней экономии (порядок строк)
    S3  stats --report: файл создан, содержит рейтинг, id форматов и проценты
    S4  stats --report на пустой базе: rc=1, отчёт не создан
    S5  stats --report в несуществующий каталог: rc=1
    S6  неизвестная опция: rc=2
    S7  LLAO_STATS_FILE перекрывает путь к stats.json (иначе файл читается
        рядом с бинарником — это боевой файл пользователя)

Путь к базе переопределяется переменной LLAO_STATS_FILE, поэтому тест не
трогает настоящий stats.json рядом с бинарником.

Запуск:
    python3 tests/test_stats.py          # полный прогон
    python3 tests/test_stats.py --keep   # не удалять scratch_stats
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LLAO_EXE = os.path.join(ROOT, "llao.exe")
LLAO_NATIVE = os.path.join(ROOT, "llao-linux")
WORK = os.path.join(ROOT, "scratch_stats")

KEEP = "--keep" in sys.argv
NATIVE_LINUX = os.path.isfile(LLAO_NATIVE) and not os.path.isfile(LLAO_EXE)
LLAO = LLAO_NATIVE if NATIVE_LINUX else LLAO_EXE

# Фикстура: четыре формата с разной экономией, плюс одна ошибка. Ожидаемый
# порядок рейтинга — по убыванию средней экономии: monkeys_audio, wavpack,
# flac. tta дешевле всех и в рейтинг не попадает (нет успешных кандидатов).
RECORDS = [
    {"format": "monkeys_audio", "status": "ok", "source_size": 10 * 1048576,
     "result_size": 1500000, "winner": True},
    {"format": "wavpack", "status": "ok", "source_size": 10 * 1048576,
     "result_size": 3000000, "winner": False},
    {"format": "flac", "status": "ok", "source_size": 20 * 1048576,
     "result_size": 10000000, "winner": True},
    {"format": "tta", "status": "error", "source_size": 1048576,
     "result_size": 0, "winner": False},
]

EXPECTED_ORDER = ["monkeys_audio", "wavpack", "flac"]


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
    assert "Total records: 4" in out, "нет числа записей:\n%s" % out
    assert "Winners (files replaced): 2" in out, "неверное число победителей:\n%s" % out
    assert "By status:" in out and "error" in out, "нет разбивки по статусам:\n%s" % out
    # Рейтинг только по успешным кандидатам, порядок — по убыванию экономии.
    ranked = [ln.split()[0] for ln in out.splitlines()
              if ln.strip().startswith(("monkeys_audio", "wavpack", "flac", "tta"))
              and "%" in ln]
    assert len(ranked) == len(EXPECTED_ORDER), \
        "в рейтинге лишние/пропущенные форматы: %r\n%s" % (ranked, out)
    assert ranked == EXPECTED_ORDER, "неверный порядок рейтинга: %r\n%s" % (ranked, out)
    # Экономия monkeys_audio = 1 - 1.5/10 = 85%.
    for ln in out.splitlines():
        if ln.strip().startswith("monkeys_audio") and "%" in ln:
            assert "85." in ln, "неверная средняя экономия:\n%s" % out
            break
    else:
        raise AssertionError("нет строки рейтинга monkeys_audio:\n%s" % out)


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
    assert "Records: 4" in text, "в отчёте нет числа записей:\n%s" % text
    assert "85." in text, "в отчёте нет средней экономии:\n%s" % text
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
    assert "Total records: 4" in out, "оверрайд не подхватился:\n%s" % out


SCENARIOS = [
    ("S1", "S1  пустая база -> rc=0, «нет статистики»", s1_empty_base),
    ("S2", "S2  сводка: записи, статусы, порядок рейтинга", s2_summary_with_records),
    ("S3", "S3  --report: файл с рейтингом, без локализации", s3_report_written),
    ("S4", "S4  --report на пустой базе -> rc=1, файла нет", s4_report_empty_base),
    ("S5", "S5  --report в несуществующий каталог -> rc=1", s5_report_unwritable),
    ("S6", "S6  неизвестная опция -> rc=2", s6_unknown_option),
    ("S7", "S7  LLAO_STATS_FILE перекрывает путь к базе", s7_env_override_used),
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
