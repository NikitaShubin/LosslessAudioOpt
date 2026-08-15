#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Проверка CLI кодеков против эталонных снапшотов (golden files).

Сравнивает вывод `cli_check.cmd` (обычно --help) каждого кодека из formats/*.json
с эталоном в .github/cli-baselines/<id>.txt. Любое расхождение считается ошибкой
(exit 1): релизная сборка блокируется до разбора изменений разработчиком/агентом.

Исключены ffmpeg-форматы (engine.kind == "ffmpeg"): master-сборки BtbN меняются
ежедневно (git-хэш в -version), эталон был бы вечно «красным»; кодеки alac/tta
стабильны и без вариантов.

Режимы:
  --compare   (по умолчанию) сравнить и вернуть ненулевой код при расхождениях
  --update    перезаписать эталоны текущим выводом кодеков (после сознательного
              разбора изменений, см. AGENTS.md -> «Релизный процесс и CI»)

Примеры:
  python3 .github/scripts/check_cli_baselines.py --compare
  python3 .github/scripts/check_cli_baselines.py --update
"""
import difflib
import glob
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FORMATS_DIR = os.path.join(ROOT, "formats")
BASELINE_DIR = os.path.join(ROOT, ".github", "cli-baselines")
BIN_DIR = os.path.join(ROOT, "bin")


def wine_cmd():
    # Ubuntu (classic WoW64) кладёт 32-битный лоадер в `wine`, а `wine64` умеет оба;
    # WineHQ (new WoW64) предоставляет только `wine`.
    for cand in ("wine64", "wine"):
        if shutil.which(cand):
            return cand
    return "wine"


def load_formats():
    fmts = []
    for path in sorted(glob.glob(os.path.join(FORMATS_DIR, "*.json"))):
        with open(path, encoding="utf-8") as f:
            fmts.append(json.load(f))
    return fmts


def find_binary(cache_dir, name):
    base = name.lower()
    if not base.endswith(".exe"):
        base += ".exe"
    if not os.path.isdir(cache_dir):
        return None
    for fn in os.listdir(cache_dir):
        if fn.lower() == base:
            return os.path.join(cache_dir, fn)
    return None


def capture(args):
    if sys.platform == "win32":
        cmd = args
    else:
        cmd = [wine_cmd()] + args
    env = dict(os.environ)
    env["WINEDEBUG"] = "-all"
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180, env=env)
    return r.stdout + r.stderr


def normalize(text):
    lines = [ln.rstrip() for ln in text.replace("\r\n", "\n").split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    return lines


def option_like(line):
    return bool(re.match(r"^[-/]{1,2}[A-Za-z]", line))


def targets():
    out = []
    for fmt in load_formats():
        if fmt.get("engine", {}).get("kind") == "ffmpeg":
            continue
        cli = fmt.get("cli_check")
        if not cli or not cli.get("cmd"):
            continue
        cache_dir = os.path.join(BIN_DIR, fmt["id"])
        binary = find_binary(cache_dir, fmt["engine"]["executable"])
        out.append((fmt["id"], binary, cli["cmd"]))
    return out


def main():
    update = "--update" in sys.argv

    failed = False
    summary = []
    for fmt_id, binary, cli_args in targets():
        base = os.path.join(BASELINE_DIR, fmt_id + ".txt")
        if binary is None:
            summary.append("%-14s НЕТ бинарника в bin/%s/" % (fmt_id, fmt_id))
            failed = True
            continue

        current = normalize(capture([binary] + cli_args))
        if update:
            os.makedirs(BASELINE_DIR, exist_ok=True)
            with open(base, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(current) + "\n")
            summary.append("%-14s эталон обновлён" % fmt_id)
            continue

        if not os.path.exists(base):
            summary.append("%-14s НЕТ эталона (нужен --update)" % fmt_id)
            failed = True
            continue
        with open(base, encoding="utf-8") as f:
            baseline = normalize(f.read())

        if baseline == current:
            summary.append("%-14s OK" % fmt_id)
            continue

        failed = True
        summary.append("%-14s ИЗМЕНИЛСЯ" % fmt_id)
        print("\n=== %s: CLI изменился ===" % fmt_id)
        diff = difflib.unified_diff(baseline, current, fromfile="baseline", tofile="current", lineterm="")
        for line in diff:
            print("    " + line.rstrip())
        added = [ln[1:].strip() for ln in current if False]
        added = [ln.lstrip("+- ").strip() for ln in list(difflib.unified_diff(baseline, current, lineterm="")) if ln.startswith("+")]
        added = [ln for ln in added if option_like(ln) and not ln.startswith("+++")]
        if added:
            print("\n    Новые option-строки:")
            for ln in added:
                print("      + " + ln)

    print("\nИтог:")
    for s in summary:
        print("  " + s)
    if update:
        print("Эталоны перезаписаны. Проверьте дифф в git и обновите formats/*.json при необходимости.")
        return 0
    if failed:
        print("Ошибка: CLI кодеков изменился. Разбор и обновление — см. AGENTS.md.")
        return 1
    print("Все справки кодеков соответствуют эталонам.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
