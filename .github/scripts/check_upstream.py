#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Проверка актуальности закреплённых версий кодеков в formats/*.json.

Сверяет версию, закреплённую в конфиге, с последней доступной:
  flac, wavpack     — GitHub API (последний релиз), обновляется автоматически
  tak, optimfrog,
  monkeys_audio     — ручная таблица UPSTREAM_MANUAL: страницы загрузки не имеют
                      стабильного API, версию поддерживает агент/разработчик

Новая версия доступна, а кодека нет в skip-списке
(.github/cli-baselines/skip-upstream.json) -> exit 1 (релиз блокируется).
Сознательное игнорирование — запись в skip-список с причиной.

Режимы:
  --check    (по умолчанию) проверить, вернуть ненулевой код при устаревших версиях
  --report   вывести сводку (для описания релиза), не блокируя

Примеры:
  python3 .github/scripts/check_upstream.py --check
  python3 .github/scripts/check_upstream.py --report
"""
import glob
import json
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FORMATS_DIR = os.path.join(ROOT, "formats")
SKIP_PATH = os.path.join(ROOT, ".github", "cli-baselines", "skip-upstream.json")

# Ручной источник актуальных версий (без стабильного API). Обновляется агентом.
UPSTREAM_MANUAL = {
    "tak": "2.3.3",
    "optimfrog": "5.100",
    "monkeys_audio": "13.25",
}

GITHUB = {
    "flac": "xiph/flac",
    "wavpack": "dbry/WavPack",
}


def load_formats():
    out = {}
    for path in sorted(glob.glob(os.path.join(FORMATS_DIR, "*.json"))):
        with open(path, encoding="utf-8") as f:
            fmt = json.load(f)
        out[fmt["id"]] = fmt
    return out


def pinned_version(fmt_id, fmt):
    downloads = fmt.get("downloads") or [{}]
    d = downloads[0]
    url = d.get("url", "")
    notes = " ".join(x for x in (fmt.get("notes", ""), d.get("notes", "")) if x)
    m = re.search(r"flac-([0-9]+(?:\.[0-9]+)*)-win", url)
    if m:
        return m.group(1)
    m = re.search(r"wavpack-([0-9]+(?:\.[0-9]+)*)-x64", url)
    if m:
        return m.group(1)
    m = re.search(r"TAK_([0-9]+(?:\.[0-9]+)*)", url)
    if m:
        return m.group(1)
    m = re.search(r"v([0-9]+\.[0-9]+)", notes)
    if m:
        return m.group(1)
    m = re.search(r"Monkey's Audio\s+([0-9]+(?:\.[0-9]+)*)", notes)
    if m:
        return m.group(1)
    return None


def latest_github(repo):
    req = urllib.request.Request(
        "https://api.github.com/repos/%s/releases/latest" % repo,
        headers={"User-Agent": "llao-ci", "Accept": "application/vnd.github+json"},
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        data = json.load(r)
    tag = data.get("tag_name", "")
    return tag.lstrip("v")


def parse_version(s):
    return tuple(int(x) for x in re.findall(r"\d+", s or ""))


def load_skips():
    if not os.path.exists(SKIP_PATH):
        return {}
    with open(SKIP_PATH, encoding="utf-8") as f:
        return json.load(f)


def main():
    report_only = "--report" in sys.argv
    fmts = load_formats()
    skips = load_skips()
    checked = 0
    outdated = []

    for fmt_id, repo in GITHUB.items():
        fmt = fmts.get(fmt_id)
        if not fmt:
            continue
        pinned = pinned_version(fmt_id, fmt)
        try:
            latest = latest_github(repo)
        except Exception as exc:
            print("%-14s GitHub API недоступен (%s) — пропуск" % (fmt_id, exc))
            continue
        checked += 1
        if pinned is None:
            print("%-14s версию из конфига не распознал — проверьте вручную" % fmt_id)
            continue
        print("%-14s закреплено %s, последний релиз %s" % (fmt_id, pinned, latest))
        if parse_version(latest) > parse_version(pinned):
            outdated.append((fmt_id, pinned, latest, "github"))

    for fmt_id, latest in UPSTREAM_MANUAL.items():
        fmt = fmts.get(fmt_id)
        if not fmt:
            continue
        pinned = pinned_version(fmt_id, fmt)
        checked += 1
        if pinned is None:
            print("%-14s версию из конфига не распознал — проверьте вручную" % fmt_id)
            continue
        print("%-14s закреплено %s, актуально %s (вручную)" % (fmt_id, pinned, latest))
        if parse_version(latest) > parse_version(pinned):
            outdated.append((fmt_id, pinned, latest, "manual"))

    if report_only:
        if outdated:
            print("\nДоступны обновления кодеков:")
            for fmt_id, pinned, latest, src in outdated:
                print("  %-14s %s -> %s" % (fmt_id, pinned, latest))
        print("\nПроверено форматов: %d" % checked)
        return 0

    blocked = [o for o in outdated if o[0] not in skips]
    if not blocked:
        print("\nВсе закреплённые версии актуальны (проверено: %d)." % checked)
        return 0

    print("\nДоступны более новые версии (релиз заблокирован):")
    for fmt_id, pinned, latest, src in blocked:
        reason = skips.get(fmt_id)
        print("  %-14s %s -> %s%s" % (
            fmt_id, pinned, latest,
            ("  [в skip-списке: %s]" % reason) if reason else ""))
    print("\nОбновите formats/*.json (URL + checksum + cli_check.expect) или добавьте")
    print("кодек в %s с причиной. Подробности — в AGENTS.md." % os.path.relpath(SKIP_PATH, ROOT))
    return 1


if __name__ == "__main__":
    sys.exit(main())
