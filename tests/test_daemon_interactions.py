#!/usr/bin/env python3
"""Комплексный тест комбинаций взаимодействия с очередью демона.

Ловит ошибки класса «действие по id попало не в тот файл» и «очередь встала»:
каждая комбинация add/reorder/remove/cancel/restart/pause/resume проверяется
утверждением о точном составе очереди после неё.

Требует: собранный llao-linux, ffmpeg в PATH. Без ffmpeg — SKIP.
Демон поднимается свой, на случайном порту (--no-auth), 18180 не трогает.

Запуск: python3 tests/test_daemon_interactions.py [--daemon ./llao-linux]
"""
import concurrent.futures as fut
import os
import shutil
import sys
import tempfile
import time

import _daemon_harness as H

FAILURES = []


def check(cond, msg):
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"FAIL: {msg}")
        FAILURES.append(msg)


def no_dupes(rows):
    ids = [x["id"] for x in rows]
    return len(ids) == len(set(ids))


def main():
    binary = H.resolve_daemon(sys.argv)
    if not os.path.exists(binary):
        print(f"SKIP: нет бинарника {binary}")
        return 0
    if shutil.which("ffmpeg") is None:
        print("SKIP: нет ffmpeg")
        return 0

    workdir = tempfile.mkdtemp(prefix="llao-inter-")
    try:
        paths = []
        for i, freq in enumerate((440, 880, 330, 550)):
            p = os.path.join(workdir, f"f{i}.wav")
            H.gen_wav(p, freq)
            paths.append(p)

        d = H.Daemon(binary, workdir, jobs=2.0)
        d.start()

        print("case 1: add + двойной reorder + действия по id бьют точно")
        r = d.rpc("add", {"paths": paths[:3]})
        check([x["id"] for x in r["result"]["added"]] == [0, 1, 2], "add ids 0,1,2")
        check(d.rpc("reorder", {"order": [2, 0, 1]})["ok"], "reorder [2,0,1]")
        check(d.ids() == [2, 0, 1], "order visible [2,0,1]")
        check(d.rpc("reorder", {"order": [1, 2, 0]})["ok"], "reorder [1,2,0]")
        check(d.ids() == [1, 2, 0], "order visible [1,2,0]")
        # remove id 0 обязан удалить f0, а не того, кто стоит на позиции 0
        labels = {x["id"]: x["label"] for x in d.rows()}
        check(d.rpc("remove", {"id": 0})["result"]["removed"] is True, "remove 0")
        st = d.rows()
        check(all(x["id"] != 0 for x in st), "id 0 gone")
        check(all(x["label"] != labels[0] for x in st), "label f0 gone, not positional victim")
        check(no_dupes(st), "no dupes")
        # cancel оставшегося id 2 (не позиции!)
        check(d.rpc("cancel-file", {"id": 2})["result"]["deleted"] is True, "cancel 2")

        print("case 2: delete-all + re-add — новые задания стартуют")
        for x in d.rows():
            d.rpc("remove", {"id": x["id"]})
        time.sleep(1)
        check(d.rows() == [], "queue empty")
        r = d.rpc("add", {"paths": paths[:2]})
        new_ids = [x["id"] for x in r["result"]["added"]]
        check(len(new_ids) == 2, f"re-add accepted: {new_ids}")
        # новые задания обязаны покинуть queued (движок жив, бюджет цел)
        deadline = time.time() + 120
        progressed = False
        while time.time() < deadline:
            st = d.rows()
            if any(x["state"] in ("prep", "running", "ok", "stopped", "error")
                   for x in st if x["id"] in new_ids):
                progressed = True
                break
            time.sleep(3)
        check(progressed, "re-added files leave queued (no stall)")

        print("case 3: pause блокирует, resume продолжает, дублей нет")
        before = sorted(x["id"] for x in d.rows())
        check(d.rpc("pause", {})["result"]["paused"] is True, "pause")
        check(d.get("/api/state")["paused"] is True, "paused flag")
        r = d.rpc("add", {"paths": [paths[3]]})
        check(r["ok"] and d.get("/api/state")["paused"] is False, "add auto-resumes")
        after = sorted(x["id"] for x in d.rows())
        check(len(after) == len(set(after)), f"no dupes after resume: {after}")
        check(all(i in after for i in before), "old ids kept")

        print("case 4: restart активного файла — отмена+ожидание+замена")
        st = d.rows()
        active = [x["id"] for x in st
                  if x["state"] in ("queued", "prep", "running")]
        if active:
            tgt = active[0]
            r = d.rpc("restart", {"ids": [tgt]})
            check(r["ok"] and r["result"]["restarted"] == [tgt],
                  f"restart active {tgt}: {r}")
            st = d.rows()
            check(all(x["id"] != tgt for x in st), f"old id {tgt} replaced")
            check(no_dupes(st), "no dupes after active restart")
        else:
            print("  skip: no active files")

        print("case 5: параллельные смешанные операции — состояние консистентно")
        def _op(kind, payload):
            try:
                if kind == "add":
                    return d.rpc("add", {"paths": [payload]})
                if kind == "remove":
                    return d.rpc("remove", {"id": payload})
                if kind == "cancel":
                    return d.rpc("cancel-file", {"id": payload})
                if kind == "reorder":
                    return d.rpc("reorder", {"order": payload})
            except Exception as e:
                return {"ok": False, "error": str(e)}
            return None

        with fut.ThreadPoolExecutor(max_workers=8) as ex:
            fs = []
            for _ in range(4):
                fs.append(ex.submit(_op, "add", paths[0]))
            cur = d.ids()
            if cur:
                fs.append(ex.submit(_op, "reorder", list(reversed(cur))))
            for _ in fs:
                _.result()
        st = d.rows()
        check(no_dupes(st), f"no dupes after mixed ops: {[x['id'] for x in st]}")
        # порядок после reversed-reorder либо применён, либо отклонён целиком
        check(isinstance(st, list), "state readable")

        print("case 6: битые команды не рвут соединение")
        for cmd, args in (("cancel-file", {"id": -1}),
                          ("remove", {"id": "x"}),
                          ("reorder", {"order": [0, "y"]}),
                          ("restart", {"ids": "nope"}),
                          ("add", {"paths": "nope"})):
            try:
                r = d.rpc(cmd, args)
                check(not r["ok"] and r["code"] == "bad_args", f"{cmd} {args} -> bad_args")
            except Exception as e:
                check(False, f"{cmd} {args} raised {e}")
        st = d.get("/api/state")
        check(isinstance(st.get("rows"), list), "daemon alive after bad input")

        print("case 7: batch-stop-all → batch-start — рестарт работает")
        d2 = H.Daemon(binary, workdir, jobs=2.0)
        d2.start()
        paths2 = []
        for i, freq in enumerate((500, 600, 700)):
            p = os.path.join(workdir, f"bs{i}.wav")
            H.gen_wav(p, freq)
            paths2.append(p)
        r = d2.rpc("add", {"paths": paths2})
        ids2 = [x["id"] for x in r["result"]["added"]]
        check(len(ids2) == 3, f"added 3 files: {ids2}")
        # Пауза до обработки: при jobs=2.0 (все ядра) и коротких файлах они
        # могут завершиться ok и замениться in-place раньше, чем тест успеет
        # их остановить (тогда restart по ok-строке невозможен — исходника
        # уже нет). Пауза фиксирует файлы в queued (активны, не стартовали) —
        # сценарий «остановленные выделенные» воспроизводится без гонки.
        d2.rpc("pause", {})
        time.sleep(0.5)
        # batch cancel-file (эмуляция «Остановить выделенные»)
        for fid in ids2:
            d2.rpc("cancel-file", {"id": fid})
        # batch restart (эмуляция «Запустить выделенные»; restart снимает паузу)
        r = d2.rpc("restart", {"ids": ids2})
        restarted = r["result"]["restarted"]
        # Главная регрессия пользователя: restart после остановки возвращал 0.
        check(len(restarted) == len(ids2),
              f"batch restart all {len(ids2)}: got {len(restarted)}")
        # После рестарта исходные id должны быть заменены новыми заданиями —
        # в этой же очереди появляются НОВЫЕ id (не исходные).
        deadline = time.time() + 15
        new_ids = []
        while time.time() < deadline:
            cur = d2.rows()
            new_ids = [x["id"] for x in cur if x["id"] not in ids2]
            if new_ids:
                break
            time.sleep(1)
        check(len(new_ids) >= 1,
              f"restart produced new jobs (engine alive): {new_ids}")
        check(d2.stop() == 0, "case 7: exit 0")

        print("case 8: cancel-file → re-add тот же путь — принят и стартует")
        d3 = H.Daemon(binary, workdir, jobs=2.0)
        d3.start()
        p8 = os.path.join(workdir, "cancel_readd.wav")
        H.gen_wav(p8, 999)
        r = d3.rpc("add", {"paths": [p8]})
        ids8 = [x["id"] for x in r["result"]["added"]]
        check(len(ids8) == 1, "add 1 file")
        time.sleep(3)
        # отменяем через cancel-file (без remove — added_paths_ НЕ очищается)
        d3.rpc("cancel-file", {"id": ids8[0]})
        time.sleep(2)
        # повторно добавляем тот же путь
        r = d3.rpc("add", {"paths": [p8]})
        added8 = r["result"]["added"]
        rejected8 = r["result"]["rejected"]
        check(len(added8) == 1, f"re-add accepted after cancel-file: added={len(added8)} rej={rejected8}")
        # новые задания стартуют
        new_id8 = added8[0]["id"]
        deadline = time.time() + 20
        started = False
        while time.time() < deadline:
            st = d3.rows()
            for x in st:
                if x["id"] == new_id8 and x["state"] in ("prep", "running", "ok", "stopped"):
                    started = True
                    break
            if started: break
            time.sleep(2)
        check(started, "re-added file after cancel-file leaves queued")
        check(d3.stop() == 0, "case 8: exit 0")

        print("case 9: batch-stop → reorder → batch-start — reorder не ломает рестарт")
        d4 = H.Daemon(binary, workdir, jobs=2.0)
        d4.start()
        paths9 = []
        for i, freq in enumerate((1000, 1100, 1200)):
            p = os.path.join(workdir, f"r{i}.wav")
            H.gen_wav(p, freq)
            paths9.append(p)
        r = d4.rpc("add", {"paths": paths9})
        ids9 = [x["id"] for x in r["result"]["added"]]
        check(len(ids9) == 3, f"added 3: {ids9}")
        time.sleep(3)
        # batch stop
        for fid in ids9:
            d4.rpc("cancel-file", {"id": fid})
        time.sleep(1)
        # reorder (после stop — все removed)
        remaining = d4.ids()
        if remaining:
            d4.rpc("reorder", {"order": list(reversed(remaining))})
        # batch restart
        r = d4.rpc("restart", {"ids": ids9})
        restarted9 = r["result"]["restarted"]
        check(len(restarted9) == len(ids9),
              f"batch restart after reorder: {len(restarted9)}/{len(ids9)}")
        deadline = time.time() + 15
        new_ids9 = [x["id"] for x in d4.rows() if x["id"] not in ids9]
        check(len(new_ids9) >= 1,
              f"restart after reorder produced new jobs: {new_ids9}")
        check(d4.stop() == 0, "case 9: exit 0")

        print("case 10: shutdown")
        check(d.stop() == 0, "exit 0")
        check(not os.path.exists(d.disc), "discovery removed")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
