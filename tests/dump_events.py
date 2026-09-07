#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Снятие «золотого» дампа event-потока демона для эталонной сверки при
рефакторинге событийной модели (Фаза B).

Запуск:
    python3 tests/dump_events.py optimize     # двухфайловый optimize-сценарий
    python3 tests/dump_events.py restore      # restore -> flac с target_dir
    python3 tests/dump_events.py --out <dir>  # каталог дампа (по умолчанию tests/golden)

Скрипт поднимает llao-linux, с момента готовности полит /api/events,
проигрывает сценарий (add/reorder/remove/cancel-file/restart/pause/resume/
shutdown) и пишет:
    tests/golden/events-<name>.jsonl   — сырые события (seq,type,id,payload)
    tests/golden/events-<name>.types   — только типы в порядке следования
    tests/golden/events-<name>-state.json — финальный снимок /api/state

Сравнение на Фазе B: типы и порядок по .types (детерминировано), содержимое
payload — по здравому смыслу (размеры кандидатов зависят от версий кодеков).
Инварианты порядка (begin_file/job_meta/added до prep/task; set_tasks до
task) проверяются вручную по .jsonl.
"""
import json
import os
import shutil
import sys
import threading
import time

import _daemon_harness as H

OUT_DIR = os.path.join(H.ROOT, "tests", "golden")


class Collector:
    """Фоновый поток: полит /api/events, пишет сырые события в JSONL."""
    def __init__(self, d):
        self.d = d
        self.since = d.get("/api/state")["last_seq"]
        self.lock = threading.Lock()
        self.stopped = False
        self.raw = []

    def run(self):
        while not self.stopped:
            try:
                poll = self.d.events(self.since)
            except Exception:
                time.sleep(0.2)
                continue
            with self.lock:
                for ev in poll["events"]:
                    self.raw.append(ev)
                if poll["resync"]:
                    self.raw.append({"type": "__resync__", "seq": 0,
                                     "args": {"lost": "yes"}})
                self.since = poll["last_seq"]
            time.sleep(0.05)

    def stop(self):
        self.stopped = True


def wait_rows(d, pred, timeout=400, interval=2):
    deadline = time.time() + timeout
    while time.time() < deadline:
        st = d.get("/api/state")
        rows = st["rows"]
        if pred(rows):
            return rows
        time.sleep(interval)
    return None


def scenario_optimize(d, workdir):
    files = [os.path.join(workdir, f"t{i}.wav") for i in range(2)]
    H.gen_wav(files[0], 440)
    H.gen_wav(files[1], 880)
    r = d.rpc("add", {"paths": files})
    ids = [x["id"] for x in r["result"]["added"]]
    d.rpc("reorder", {"order": [ids[1], ids[0]]})
    rows = wait_rows(d, lambda rs: any(x["state"] == "ok" for x in rs))
    if rows is None:
        raise RuntimeError("первый файл не завершился (optimize)")
    done_ids = [x["id"] for x in rows if x["state"] == "ok"]
    rest = [x for x in rows if x["state"] != "ok"]
    if rest:
        d.rpc("pause", {})
        d.rpc("resume", {})
        d.rpc("remove", {"id": rest[0]["id"]})


def scenario_restore(d, workdir):
    src = os.path.join(workdir, "src.wav")
    target = os.path.join(workdir, "out")
    os.makedirs(target, exist_ok=True)
    H.gen_wav(src, 330)
    r = d.rpc("add", {"paths": [src], "mode": "restore", "target_dir": target})
    fid = r["result"]["added"][0]["id"]
    rows = wait_rows(d, lambda rs: any(x["id"] == fid and x["state"] == "ok"
                                       for x in rs))
    if rows is None:
        raise RuntimeError("restore-файл не завершился")


def main():
    args = sys.argv[1:]
    scenario = None
    out_dir = OUT_DIR
    only_types = False
    for a in args:
        if a in ("optimize", "restore"):
            scenario = a
        elif a.startswith("--out="):
            out_dir = a.split("=", 1)[1]
        elif a == "--types-only":
            only_types = True
    if scenario is None:
        print("укажи сценарий: optimize|restore", file=sys.stderr)
        return 1
    os.makedirs(out_dir, exist_ok=True)
    workdir = os.path.join(out_dir, "_dump-work")
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    d = H.Daemon(H.DEFAULT_DAEMON, workdir, jobs=2.0)
    d.start()
    col = Collector(d)
    th = threading.Thread(target=col.run, daemon=True)
    th.start()
    try:
        if scenario == "optimize":
            scenario_optimize(d, workdir)
        else:
            scenario_restore(d, workdir)
        time.sleep(1)
        final_state = d.get("/api/state")
    finally:
        col.stop()
        th.join(timeout=5)
        d.stop(force=True)
    base = os.path.join(out_dir, f"events-{scenario}")
    with open(base + ".jsonl", "w") as f:
        for ev in col.raw:
            f.write(json.dumps(ev, ensure_ascii=False) + "\n")
    with open(base + ".types", "w") as f:
        f.write("\n".join(ev["type"] for ev in col.raw) + "\n")
    with open(base + "-state.json", "w") as f:
        json.dump(final_state, f, ensure_ascii=False, indent=1)
    n = len(col.raw)
    types = [ev["type"] for ev in col.raw]
    uniq = sorted(set(types))
    print(f"ok: {base}.jsonl ({n} событий, {len(uniq)} типов)")
    print("типы:", ", ".join(uniq))
    return 0


if __name__ == "__main__":
    sys.exit(main())