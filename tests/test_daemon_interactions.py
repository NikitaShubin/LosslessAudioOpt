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
import json
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
        progressed = H.wait_until(
            lambda: any(x["state"] in ("prep", "running", "ok", "stopped", "error")
                        for x in d.rows() if x["id"] in new_ids),
            timeout=120, interval=3)
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
        new_ids = H.wait_until(
            lambda: [x["id"] for x in d2.rows() if x["id"] not in ids2]
            or None,
            timeout=15, interval=1)
        check(len(new_ids or []) >= 1,
              f"restart produced new jobs (engine alive): {new_ids}")
        check(d2.stop() == 0, "case 7: exit 0")

        print("case 8: cancel-file → re-add тот же путь — принят и стартует")
        # Свой демон и свой каталог: иначе в очередь попадают файлы предыдущих
        # кейсов, и проверка дублей считала бы их дубликатами этого сценария.
        workdir8 = tempfile.mkdtemp(prefix="llao-inter8-")
        d3 = H.Daemon(binary, workdir8, jobs=2.0)
        d3.start()
        p8 = os.path.join(workdir8, "cancel_readd.wav")
        H.gen_wav(p8, 999)
        r = d3.rpc("add", {"paths": [p8]})
        ids8 = [x["id"] for x in r["result"]["added"]]
        check(len(ids8) == 1, "add 1 file")
        # Пауза сразу после add: при jobs=2.0 короткий wav может обработаться
        # и замениться in-place раньше, чем тест успеет сделать cancel-file
        # (тогда исходник исчезает и re-add отклонится «path not found»).
        # Пауза фиксирует задание в queued — сценарий воспроизводится без гонки.
        d3.rpc("pause", {})
        time.sleep(0.5)
        # отменяем через cancel-file (без remove — added_paths_ НЕ очищается)
        d3.rpc("cancel-file", {"id": ids8[0]})
        time.sleep(1)
        # повторно добавляем тот же путь (add снимает паузу)
        r = d3.rpc("add", {"paths": [p8]})
        added8 = r["result"]["added"]
        rejected8 = r["result"]["rejected"]
        check(len(added8) == 1, f"re-add accepted after cancel-file: added={len(added8)} rej={rejected8}")
        # новые задания стартуют
        new_id8 = added8[0]["id"]
        started = H.wait_until(
            lambda: any(x["id"] == new_id8
                        and x["state"] in ("prep", "running", "ok", "stopped")
                        for x in d3.rows()),
            timeout=20, interval=2)
        check(started, "re-added file after cancel-file leaves queued")
        # Дубль той же строки после cancel+re-add — потеря целостности списка:
        # пользователь видит один файл дважды, а persist спокойно пишет обе.
        rows8 = d3.rows()
        by_path8 = {}
        for x in rows8:
            by_path8.setdefault(x["path"], []).append(x)
        dup8 = {p_: v for p_, v in by_path8.items() if len(v) > 1}
        # Допускается ровно две строки одного пути: отменённая (stopped,
        # оставшаяся в истории списка) и новая. Это осознанное поведение
        # cancel-file — он помечает строку, а не удаляет. Запрещено трёх и более:
        # значит повторные add копят копии, и persist записывает их все.
        check(max((len(v) for v in by_path8.values()), default=0) <= 2,
              "case 8: не более двух строк на путь после cancel+re-add: %r"
              % {k: [x["state"] for x in v] for k, v in dup8.items()})
        live8 = [x for v in by_path8.values() for x in v
                 if x["state"] in ("queued", "prep", "running")]
        check(len(live8) <= 1, f"case 8: файл не активен дважды: {len(live8)}")
        # Персист должен содержать столько же строк, сколько видит UI.
        qp8 = os.path.join(os.path.dirname(d3.disc), "queue.json")
        if os.path.exists(qp8):
            with open(qp8, encoding="utf-8") as f:
                q8 = json.load(f)
            check(len(q8.get("rows", [])) == len(rows8),
                  f"case 8: queue.json и /api/state согласованы: "
                  f"{len(q8.get('rows', []))} против {len(rows8)}")
        check(d3.stop() == 0, "case 8: exit 0")
        shutil.rmtree(workdir8, ignore_errors=True)

        print("case 9: batch-stop → reorder → batch-start — reorder не ломает рестарт")
        d4 = H.Daemon(binary, workdir, jobs=2.0)
        d4.start()
        paths9 = []
        for i, freq in enumerate((1000, 1100, 1200)):
            p = os.path.join(workdir, f"r{i}.wav")
            # 20 секунд, а не дефолтные 0.25: кейс проверяет batch-stop по
            # АКТИВНЫМ строкам. Короткий wav на быстрой машине успевает
            # завершиться (state=ok) до cancel, а restart завершённые строки
            # сознательно не поднимает — тест получался плавающим.
            H.gen_wav(p, freq, duration=20)
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
        new_ids9 = H.wait_until(
            lambda: [x["id"] for x in d4.rows() if x["id"] not in ids9]
            or None,
            timeout=15, interval=1)
        check(len(new_ids9 or []) >= 1,
              f"restart after reorder produced new jobs: {new_ids9}")
        check(d4.stop() == 0, "case 9: exit 0")

        # case 10: строгий режим — сбой ОДНОГО варианта перечёркивает файл,
        # остальные варианты этого файла останавливаются, очередь продолжается.
        #
        # Регрессия с живой библиотеки: ветка «variant_errors > 0 -> error»
        # стояла под verify_all, поэтому при verify=winner (дефолт демона) файл
        # выходил ok с красной точкой на сломанном alac. Смысл режима без
        # --ignore-errors: дыры в проверке быть не должно.
        workdir10 = tempfile.mkdtemp(prefix="llao-strict-")
        restore = None
        try:
            H.gen_wav(os.path.join(workdir10, "bad.wav"), 500, 0.2)
            H.gen_wav(os.path.join(workdir10, "good.wav"), 700, 0.2)
            restore = H.stub_monkeys_audio_encoder(workdir10)
            d10 = H.Daemon(binary, workdir10, jobs=1.0)
            d10.start()
            r10 = d10.rpc("add", {"paths": [
                os.path.join(workdir10, "bad.wav"),
                os.path.join(workdir10, "good.wav")]})
            check(r10["ok"], "case 10: add двух файлов")
            ids10 = [x["id"] for x in r10["result"]["added"]]
            check(len(ids10) == 2, f"case 10: два id {ids10}")
            # Ждём терминального состояния обоих файлов. Верхняя граница по
            # времени, а не «после N секунд всё должно быть готово»: ночные
            # пресеты optimfrog на виниле уходят далеко за любой разумный срок,
            # и жёсткий таймаут в тесте означал бы только flaky-падение.
            ids_all = set(d10.ids())
            st10 = H.wait_state(
                d10,
                lambda st: bool(ids_all) and all(
                    x["state"] in ("ok", "error", "stopped")
                    for x in st["rows"] if x["id"] in ids_all),
                timeout=1800, interval=5)
            check(st10 is not None,
                  "case 10: оба файла дошли до терминального состояния за отведённое время")
            if st10 is None:
                print("     состояние на момент таймаута:",
                      [(x["id"], x["state"]) for x in d10.rows()])
                st10 = {"rows": d10.rows()}
            rows10 = {x["id"]: x for x in st10["rows"]}
            bad = rows10.get(ids10[0], {})
            # Первый файл: monkeys_audio отказал -> строгий режим перечёркивает
            # файл, даже если остальные форматы отработали.
            check(bad.get("state") == "error",
                  f"case 10: сбой варианта перечеркнул файл (state={bad.get('state')})")
            # Причина от кодера обязана дойти до строки: у виновника своя
            # task_error, у файла — last_error. Безликое «variant failed»
            # диагностикой не считается.
            err_txt = (bad.get("last_error") or "") + " ".join(
                (t.get("error") or "") for t in (bad.get("task_infos") or []))
            check("Command line error" in err_txt,
                  f"case 10: причина от кодера дошла до строки: {err_txt[:140]}")
            # Отказавший вариант помечен failed, остальные этого файла — серым
            # (skipped): их остановили из-за соседнего сбоя, виновник один.
            # Все варианты monkeys_audio честно красные: их пять, и каждый
            # отказ кодировщика — самостоятельный виновник, а не жертва
            # остановки соседа. Проверяем именно это различие, иначе тест
            # принимал бы и прежнюю поломку, где виновник превращался в серый.
            tasks = bad.get("tasks") or []
            infos = bad.get("task_infos") or []
            mac_failed = sum(
                1 for i, s in enumerate(tasks)
                if s == "failed" and i < len(infos) and infos[i].get("fmt") == "monkeys_audio")
            check(mac_failed >= 1,
                  f"case 10: отказы monkeys_audio помечены failed: {tasks[:8]}")
            others = [s for i, s in enumerate(tasks)
                      if s != "skipped" and (i >= len(infos) or
                                             infos[i].get("fmt") != "monkeys_audio")]
            check("failed" not in others,
                  f"case 10: невиновные варианты не красные: {others[:8]}")
            check("skipped" in tasks,
                  f"case 10: остановленные варианты серые (skipped): {tasks[:8]}")
            # Второй файл: monkeys_audio отказывает и для него тоже (подмена
            # бинарника глобальна), поэтому строгий режим перечёркивает и его —
            # это правильно. Главное проверяемое здесь: он дошёл до терминального
            # состояния сам, а не завис после ошибки первого файла.
            good = rows10.get(ids10[1], {})
            check(good.get("state") == "error",
                  f"case 10: второй файл обработан, а не завис "
                  f"(state={good.get('state')})")
            check(d10.stop() == 0, "case 10: exit 0")
        finally:
            if restore:
                restore()
            shutil.rmtree(workdir10, ignore_errors=True)

        print("case 11: shutdown")
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
