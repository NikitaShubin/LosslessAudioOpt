#!/usr/bin/env python3
"""Интеграционный тест очереди демона (замысловатые сценарии).

Требует: собранный llao-linux (или llao.exe под wine — не здесь),
ffmpeg в PATH для генерации тестовых wav. Без ffmpeg тест пропускается (exit 0).

Сценарии (все против живого HTTP-API демона на случайном порту, --no-auth):
  1. add трёх файлов -> id 0,1,2, без дублей
  2. двойной reorder подряд по стабильным id (второй не ломает порядок)
  3. remove running-файла -> повторные полы state: призраков нет,
     удалённый файл не заменён на диске (нет .tak/.flac)
  4. reorder подмножеством после remove (раньше молча отказывал)
  5. pause -> resume: те же id, без дублей
  6. restart завершённого: старая строка заменена новой, дублей нет
  7. restart активного/неизвестного -> пустой restarted; restart без ids -> bad_args
  7.11 optimize с target_dir: результат в target_dir/<rel>, оригинал не тронут;
       повторный optimize уже оптимизированного — всё равно пишется в цель
  8. shutdown: процесс завершается, discovery-файл удалён

Запуск: python3 tests/test_daemon_queue.py [--daemon ./llao-linux]
"""
import os
import shutil
import sys
import tempfile
import time

import _daemon_harness as H

FAILURES = []


def check(cond, msg):
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES.append(msg)


def main():
    binary = H.resolve_daemon(sys.argv)
    if not os.path.exists(binary):
        print(f"SKIP: нет бинарника {binary}")
        return 0
    if not H.ffmpeg_available():
        print("SKIP: нет ffmpeg")
        return 0

    workdir = tempfile.mkdtemp(prefix="llao-qtest-")
    # Параллелизм: движок параллелит варианты кодеков одного файла, поэтому
    # на серийном jobs=1 тест тянется минуты на файл и не укладывается в лимиты.
    # jobs=2.0 — множитель ядер (автоопределение числа воркеров).
    try:
        files = []
        for i, freq in enumerate((440, 880, 330)):
            p = os.path.join(workdir, f"t{i}.wav")
            H.gen_wav(p, freq)
            files.append(p)

        d = H.Daemon(binary, workdir, jobs=2.0)
        d.start()

        # 1. add трёх файлов
        r = d.rpc("add", {"paths": files})
        check(r["ok"] and [x["id"] for x in r["result"]["added"]] == [0, 1, 2],
              f"add ids 0,1,2: {r}")

        # 2. двойной reorder по стабильным id
        check(d.rpc("reorder", {"order": [2, 0, 1]})["ok"], "reorder 1")
        st = d.get("/api/state")
        check([x["id"] for x in st["rows"]] == [2, 0, 1], "order [2,0,1]")
        check(d.rpc("reorder", {"order": [1, 2, 0]})["ok"], "reorder 2")
        st = d.get("/api/state")
        check([x["id"] for x in st["rows"]] == [1, 2, 0], "order [1,2,0]")

        # 3. remove running-файла: призраков нет, замены нет
        check(d.rpc("remove", {"id": 2})["result"]["removed"] is True, "remove 2")
        deadline = time.time() + 24
        while time.time() < deadline:
            time.sleep(1)
            st = d.get("/api/state")
            ids = [x["id"] for x in st["rows"]]
            check(2 not in ids, f"no ghost row 2, got {ids}")
            check(len(ids) == len(set(ids)), f"no dupes: {ids}")
            if 2 not in ids:
                break
        check(not os.path.exists(files[2] + ".tak")
              and not os.path.exists(os.path.splitext(files[2])[0] + ".tak"),
              "removed file not replaced")
        check(os.path.exists(files[2]), "removed source untouched")

        # 4. reorder подмножеством после remove
        check(d.rpc("reorder", {"order": [1, 0]})["ok"], "subset reorder")
        st = d.get("/api/state")
        check([x["id"] for x in st["rows"]] == [1, 0], "subset order applied")

        # 5. pause -> resume: те же id, без дублей
        check(d.rpc("pause", {})["result"]["paused"] is True, "pause")
        check(d.get("/api/state")["paused"] is True, "paused flag")
        check(d.rpc("resume", {})["result"]["paused"] is False, "resume")
        st = d.get("/api/state")
        ids = [x["id"] for x in st["rows"]]
        check(sorted(ids) == [0, 1], f"same ids after resume: {ids}")

        # ждём завершение хотя бы одного файла для restart-теста
        # (прогон вариантов медленный: на quiet-машинах до ~150с на файл)
        st = H.wait_state(d, lambda s: any(
            x["state"] in ("ok", "stopped", "error") for x in s["rows"]), timeout=360)
        done = [x for x in st["rows"] if x["state"] in ("ok", "stopped", "error")]
        check(bool(done), f"at least one done file: {st['counters']}")

        # 6. restart остановленного через cancel-file файла: замена, не append.
        # Детерминированно: добавляем свежий файл, сразу cancel-file (пока он
        # queued/prep — исходник на месте, не заменён), затем restart. Это
        # ровно пользовательский сценарий «остановил -> запустил выделенные».
        rst_wav = os.path.join(workdir, "restart_target.wav")
        H.gen_wav(rst_wav, 640)
        r = d.rpc("add", {"paths": [rst_wav]})
        new_id = r["result"]["added"][0]["id"]
        d.rpc("cancel-file", {"id": new_id})
        # ждём, пока строка перестанет быть активной (cancel завершил транзакцию)
        deadline = time.time() + 20
        while time.time() < deadline:
            rows = d.get("/api/state")["rows"]
            row = next((x for x in rows if x["id"] == new_id), None)
            if row is None or row["state"] not in ("queued", "prep", "running"):
                break
            time.sleep(0.5)
        r = d.rpc("restart", {"ids": [new_id]})
        check(r["ok"] and r["result"]["restarted"] == [new_id],
              f"restart after cancel {new_id}: {r}")
        st = d.get("/api/state")
        ids = [x["id"] for x in st["rows"]]
        check(new_id not in ids, f"old id {new_id} gone: {ids}")
        check(len(ids) == len(set(ids)), f"no dupes after restart: {ids}")

        # 6.5. remove/cancel неизвестного id -> false, без исключений
        r = d.rpc("remove", {"id": 9999})
        check(r["ok"] and r["result"]["removed"] is False, f"remove unknown: {r}")
        r = d.rpc("cancel-file", {"id": 9999})
        check(r["ok"] and r["result"]["deleted"] is False, f"cancel unknown: {r}")
        # кривые типы не рвут соединение: bad_args, демон жив
        for bad in ({"id": -1}, {"id": "x"}, {"id": 1.5}, {"id": None}):
            r = d.rpc("cancel-file", bad)
            check(not r["ok"] and r["code"] == "bad_args", f"bad id {bad}: {r}")
        r = d.rpc("reorder", {"order": [0, -1]})
        check(not r["ok"] and r["code"] == "bad_args", f"bad order: {r}")
        st = d.get("/api/state")
        check(isinstance(st.get("rows"), list), "daemon alive after bad input")

        # 7. restart активного/неизвестного/без args
        # restart универсален: активный останавливается и запускается
        # заново заменой (без дублей). Пауза фиксирует состояния, чтобы
        # исключить гонку самого теста.
        d.rpc("pause", {})
        st = d.get("/api/state")
        active = [x["id"] for x in st["rows"] if x["state"] == "queued"]
        if active:
            before = sorted(x["id"] for x in st["rows"])
            r = d.rpc("restart", {"ids": active})
            check(r["ok"] and sorted(r["result"]["restarted"]) == sorted(active),
                  f"restart active replaces: {r}")
            st = d.get("/api/state")
            ids = sorted(x["id"] for x in st["rows"])
            check(len(ids) == len(set(ids)), f"no dupes after active restart: {ids}")
            for old in active:
                check(old not in ids, f"old id {old} replaced: {ids}")
        d.rpc("resume", {})
        r = d.rpc("restart", {"ids": [9999]})
        check(r["ok"] and r["result"]["restarted"] == [], "restart unknown")
        r = d.rpc("restart", {})
        check(not r["ok"] and r["code"] == "bad_args", "restart bad_args")

        # 7.5. параллельные add одного пути: ровно одно добавление, без дублей
        import concurrent.futures as _fut
        race_wav = os.path.join(workdir, "race.wav")
        H.gen_wav(race_wav, 660)

        def _add_once():
            return d.rpc("add", {"paths": [race_wav]})

        with _fut.ThreadPoolExecutor(max_workers=8) as ex:
            results = list(ex.map(lambda _: _add_once(), range(8)))
        total_added = sum(len(r["result"]["added"]) for r in results if r.get("ok"))
        check(total_added == 1, f"concurrent add -> exactly 1 added: {results}")
        st = d.get("/api/state")
        matches = [x for x in st["rows"] if x["label"] == "race.wav"]
        check(len(matches) == 1, f"single race.wav row: {matches}")

        # 7.6. restart остановленного при включённой паузе: должен снять паузу
        # и запустить файл (регрессия: раньше снималась — файл добавлялся в
        # «queued», не стартовал, на UI выглядело как «удаление из списка»).
        pw = os.path.join(workdir, "pause_restart.wav")
        H.gen_wav(pw, 700)
        r = d.rpc("add", {"paths": [pw]})
        pid = r["result"]["added"][0]["id"]
        d.rpc("bulk-cancel", {"ids": [pid]})   # «остановить всё»
        check(d.rpc("pause", {})["result"]["paused"] is True, "pause before restart")
        r = d.rpc("restart", {"ids": [pid]})
        check(r["ok"] and r["result"]["restarted"] == [pid],
              f"restart while paused: {r}")
        st = H.wait_state(d, lambda s: (not s["paused"]) and any(
            x["state"] in ("prep", "running") for x in s["rows"]),
            timeout=30, interval=1)
        check(st is not None and not st["paused"],
              "restart при паузе снял паузу и запустил файл")

        # 7.7. bulk-cancel / bulk-remove: один RPC, корректные счётчики
        bw = os.path.join(workdir, "bulk_target.wav")
        H.gen_wav(bw, 500)
        r = d.rpc("add", {"paths": [bw]})
        bid = r["result"]["added"][0]["id"]
        r = d.rpc("bulk-cancel", {"ids": [bid]})
        check(r["ok"] and r["result"]["cancelled"] == 1, f"bulk-cancel: {r}")
        st = d.get("/api/state")
        row = [x for x in st["rows"] if x["id"] == bid]
        check(row and row[0]["state"] == "stopped",
              f"bulk-cancel -> stopped: {st['rows']}")
        r = d.rpc("bulk-remove", {"ids": [bid]})
        check(r["ok"] and r["result"]["removed"] == 1, f"bulk-remove: {r}")
        st = d.get("/api/state")
        check(bid not in [x["id"] for x in st["rows"]], "bulk-remove убрал строку")

        # 7.8. sort: стабильная сортировка по полному пути; id сохраняются
        a_path = os.path.join(workdir, "zd.wav")
        b_path = os.path.join(workdir, "aa.wav")
        H.gen_wav(b_path, 300)
        H.gen_wav(a_path, 900)
        r = d.rpc("add", {"paths": [b_path, a_path]})
        ids = [x["id"] for x in r["result"]["added"]]
        r = d.rpc("sort", {})
        check(r["ok"] and r["result"]["sorted"] >= 2, f"sort: {r}")
        st = d.get("/api/state")
        idx = {x["id"]: i for i, x in enumerate(st["rows"])}
        check(idx[ids[0]] < idx[ids[1]], f"sort ставит aa перед zd: {st['rows']}")

        # 7.9. дедуп движка: повторное добавление папки с активным файлом
        dup_dir = os.path.join(workdir, "dupdir")
        os.makedirs(dup_dir, exist_ok=True)
        H.gen_wav(os.path.join(dup_dir, "inner.wav"), 250)
        r1 = d.rpc("add", {"paths": [dup_dir]})
        check(len(r1["result"]["added"]) == 1, f"first add dir: {r1}")
        d.rpc("pause", {})
        r2 = d.rpc("add", {"paths": [dup_dir]})
        check(len(r2["result"]["added"]) == 0, f"second add dir filtered: {r2}")
        st = d.get("/api/state")
        matches = [x for x in st["rows"] if x["label"] == "inner.wav"]
        check(len(matches) == 1, f"no dup inner.wav: {matches}")
        d.rpc("resume", {})

        # 7.10. restart сохраняет позицию строки: ручная перестановка очереди
        # не должна теряться при возобновлении остановленного файла. Прогоняем
        # несколько циклов подряд — каждые стоп+старт оставляют в движке
        # «зомби»-строку, которая раньше сбивала восстановление позиции.
        zpath = os.path.join(workdir, "pos_z.wav")
        apath2 = os.path.join(workdir, "pos_a.wav")
        H.gen_wav(zpath, 311)
        H.gen_wav(apath2, 322)
        r = d.rpc("add", {"paths": [zpath, apath2]})
        zid = r["result"]["added"][0]["id"]
        aid2 = r["result"]["added"][1]["id"]
        # ручная перестановка: [pos_z, pos_a] -> [pos_a, pos_z]
        check(d.rpc("reorder", {"order": [aid2, zid]})["result"]["reordered"] is True,
              "reorder перед restart")
        st = d.get("/api/state")
        pos_z = [i for i, x in enumerate(st["rows"]) if x["id"] == zid][0]
        check(pos_z == 1, f"pos_z на позиции 1: {pos_z}")
        for cycle in range(3):
            d.rpc("cancel-file", {"id": zid})
            deadline = time.time() + 20
            while time.time() < deadline:
                rows = d.get("/api/state")["rows"]
                row = next((x for x in rows if x["id"] == zid), None)
                if row is None or row["state"] not in ("queued", "prep", "running"):
                    break
                time.sleep(0.5)
            r = d.rpc("restart", {"ids": [zid]})
            check(r["ok"] and r["result"]["restarted"] == [zid],
                  f"restart (цикл {cycle}): {r}")
            st = H.wait_state(d, lambda s: any(
                x["label"] == os.path.basename(zpath) and x["id"] != zid
                for x in s["rows"]), timeout=30, interval=1)
            check(st is not None, f"после restart (цикл {cycle}) новая строка на месте")
            newrows = [x for x in st["rows"]
                       if x["label"] == os.path.basename(zpath)]
            check(newrows and [i for i, x in enumerate(st["rows"])
                               if x["label"] == os.path.basename(zpath)][0] == pos_z,
                  f"restart сохраняет позицию (цикл {cycle}): было {pos_z}")
            zid = newrows[0]["id"]

        # 7.11. optimize с целевой папкой: результат пишется в target_dir/<rel>,
        # оригинал не трогается (регрессия: раньше target_dir игнорировался для
        # optimize, и файл заменялся на месте).
        src_dir = os.path.join(workdir, "tg_src")
        sub_dir = os.path.join(src_dir, "sub")
        tgt_dir = os.path.join(workdir, "tg_out")
        os.makedirs(sub_dir)
        os.makedirs(tgt_dir)
        song = os.path.join(sub_dir, "song.wav")
        H.gen_wav(song, 515)
        orig_size = os.path.getsize(song)
        r = d.rpc("add", {"paths": [src_dir], "mode": "optimize",
                          "target_dir": tgt_dir})
        check(len(r["result"]["added"]) == 1, f"add tg optimize: {r}")
        st = H.wait_state(d, lambda s: any(
            x["label"].endswith("song.wav") and x["state"] in ("ok", "stopped", "error")
            for x in s["rows"]), timeout=360)
        check(st is not None, "tg optimize файл завершён")
        row = [x for x in st["rows"] if x["label"].endswith("song.wav")]
        row = row[-1]
        check(row["state"] == "ok", f"tg optimize state ok: {row.get('state')}")
        check(row.get("out_path", "").startswith(tgt_dir),
              f"tg out_path внутри target_dir: {row.get('out_path')}")
        produced = []
        for root, _dirs, fnames in os.walk(tgt_dir):
            for fn in fnames:
                produced.append(os.path.join(root, fn))
        exts = tuple("." + e for e in H.format_extensions())
        check(any(fn.lower().endswith(exts)
                  for _, fn in [os.path.split(p) for p in produced]),
              f"target получил оптимизированный файл: {produced}")
        check(os.path.exists(song) and os.path.getsize(song) == orig_size,
              "оригинал на месте и не заменён")
        check([f for f in os.listdir(sub_dir) if f != "song.wav"] == [],
              f"рядом с оригиналом нет новых файлов: {os.listdir(sub_dir)}")

        # 7.11b. повторный optimize уже оптимизированного файла в ту же цель:
        # кандидат вряд ли меньше исходника, но в цель всё равно пишется,
        # исходник не трогается (регрессия «всегда писать в цель»).
        if produced:
            src2 = produced[0]
            base2 = os.path.basename(src2)
            before_src2 = os.path.getsize(src2)
            r = d.rpc("add", {"paths": [src2], "mode": "optimize",
                              "target_dir": tgt_dir})
            check(len(r["result"]["added"]) == 1, f"add tg re-opt: {r}")
            st = H.wait_state(d, lambda s: any(
                x["label"].endswith(base2)
                and x["state"] in ("ok", "stopped", "error") for x in s["rows"]),
                timeout=360)
            check(st is not None, "tg re-opt файл завершён")
            row2 = [x for x in st["rows"] if x["label"].endswith(base2)]
            row2 = row2[-1]
            check(row2["state"] == "ok", f"tg re-opt state ok: {row2.get('state')}")
            check(row2.get("out_path", "").startswith(tgt_dir),
                  f"tg re-opt доставлен в target: {row2.get('out_path')}")
            check(os.path.exists(src2) and os.path.getsize(src2) == before_src2,
                  "re-opt исходник не тронут")

        # 8. shutdown: процесс вышел, discovery удалён
        rc = d.stop()
        check(rc == 0, f"exit code 0, got {rc}")
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
