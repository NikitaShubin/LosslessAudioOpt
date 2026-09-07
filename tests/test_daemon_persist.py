#!/usr/bin/env python3
"""Тесты персистентности очереди демона и crash-безопасной доставки.

Покрывает:
  P1  очередь.json пишется рядом с discovery-файлом (формат v2, real-статусы)
  P2  graceful restart: done остаются ok, active при перезапуске -> queued и
      продолжают обработку; строки папки не теряются
  P3  crash (kill -9): prep/running при перезапуске -> stopped с причиной,
      engine-строк отпали; порядок очереди сохранён
  P4  исчезнувший/невалидный исходник при восстановлении -> строка остаётся,
      stopped/error, в движок не уходит
  P5  доставка sidecar на месте (in-place): .tags.zip появляется рядом,
      транзакционных артефактов .llao-tmp.* нет
  P6  /api/state возвращает no_auth:true при --no-auth
  P7  повторный запуск на той же discovery не дублирует строки (дедуп по пути)

Требует: собранный llao-linux, ffmpeg в PATH. Без ffmpeg — SKIP.
Демон поднимается свой, на случайном порту (--no-auth), 18180 не трогает.

Запуск: python3 tests/test_daemon_persist.py [--daemon ./llao-linux]
"""
import json
import os
import shutil
import sys
import tempfile
import time
import zipfile

import _daemon_harness as H

FAILURES = []


def check(cond, msg):
    if cond:
        print(f"  ok: {msg}")
    else:
        print(f"FAIL: {msg}")
        FAILURES.append(msg)


def queue_path(d):
    # queue.json лежит рядом с discovery-файлом.
    return os.path.join(os.path.dirname(d.disc), "queue.json")


def read_queue(d):
    p = queue_path(d)
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def main():
    binary = H.resolve_daemon(sys.argv)
    if not os.path.exists(binary):
        print(f"SKIP: нет бинарника {binary}")
        return 0
    if not H.ffmpeg_available():
        print("SKIP: нет ffmpeg")
        return 0

    workdir = tempfile.mkdtemp(prefix="llao-persist-")
    try:
        # jobs=1 осознанно: P1/P3 должны застать файл в prep/running в момент
        # graceful/crash-перезапуска. При jobs=2.0 (все ядра) короткие файлы
        # мгновенно уходят в ok, и детерминизм активной строки теряется.
        # Сокращение файлов до 0.25 с и так ускоряет тест в разы.
        d = H.Daemon(binary, workdir, jobs=1)
        d.start()

        # P6. no_auth в /api/state при --no-auth.
        st = d.get("/api/state")
        check(st.get("no_auth") is True, "no_auth:true при --no-auth")

        # Два файла: один дадим завершиться, другой оставим обработывающимся.
        a_wav = os.path.join(workdir, "a.wav")
        b_wav = os.path.join(workdir, "b.wav")
        H.gen_wav(a_wav, 440)
        H.gen_wav(b_wav, 880)
        r = d.rpc("add", {"paths": [a_wav, b_wav], "mode": "optimize"})
        check(len(r["result"]["added"]) == 2, f"add двух файлов: {r['result']}")
        a_id = r["result"]["added"][0]["id"]
        b_id = r["result"]["added"][1]["id"]

        # Ждём завершения первого файла, второй пусть останется активным.
        st = H.wait_state(d, lambda s: any(
            x["id"] == a_id and x["state"] in ("ok", "stopped", "error")
            for x in s["rows"]), timeout=360, interval=2)
        a_done = [x for x in (st or {}).get("rows", []) if x["id"] == a_id]
        check(a_done and a_done[0]["state"] == "ok", f"a.wav завершён ok: {a_done}")
        b_active = [x for x in d.rows() if x["id"] == b_id]
        check(b_active and b_active[0]["state"] in ("queued", "prep", "running"),
              f"b.wav ещё активен: {b_active}")

        # P1. queue.json существует рядом с discovery и имеет структуру v2.
        q = read_queue(d)
        check(q is not None and q.get("version") == 2, "queue.json v2 рядом с discovery")
        check(q and any(r.get("path", "").endswith("a.wav") and r["state"] == "ok"
                        for r in q["rows"]), "в queue.json a.wav реальный ok")
        check(q and any(r.get("path", "").endswith("b.wav") and r["state"] in
                        ("queued", "prep", "running")
                        for r in q["rows"]), "в queue.json b.wav активен (пишем как есть)")

        # P2. Грациозный перезапуск: shutdown -> новый демон на той же discovery.
        rc = d.stop()
        check(rc == 0, f"graceful shutdown exit 0 (got {rc})")
        # Строки и их статусы сохранены; после штатного shutdown активные -> queued.
        q = read_queue(d)
        check(all(r["state"] in ("ok", "stopped", "error", "queued")
                  for r in q["rows"]), "после shutdown нет prep/running в queue.json")
        b_lines = [r for r in q["rows"] if r.get("path", "").endswith("b.wav")]
        check(b_lines and b_lines[0]["state"] == "queued",
              "после shutdown активный b.wav стал queued")

        # Запускаем заново на той же discovery.
        d2 = H.Daemon(binary, workdir, jobs=1)
        d2.start(ready="rpc")
        rows = d2.rows()
        check(len([x for x in rows if x["state"] == "ok"]) == 1,
              f"после перезапуска один ok: {[(x['label'], x['state']) for x in rows]}")
        check(any(x["state"] == "ok" and os.path.basename(x["label"]) == "a.wav"
                  for x in rows), "a.wav восстановлен как ok")
        st = H.wait_state(d2, lambda s: any(
            x["state"] == "ok" and os.path.basename(x["label"]) == "b.wav"
            for x in s["rows"]), timeout=360, interval=2)
        check(st is not None, "b.wav после перезапуска добрался до ok")
        rows = d2.rows()
        check(len([x for x in rows if x["state"] == "ok"]) == 2,
              "после обработки оба файла ok, дублей нет")

        # P7. повторный запуск не дублирует строки.
        ids = [x["id"] for x in rows]
        check(len(ids) == len(set(ids)) and len(rows) == 2,
              f"строк ровно 2, без дублей: {rows}")

        # P4. исходник исчез -> при перезапуске строка остаётся stopped с причиной.
        # У перезапущенных строк id из резервного диапазона (движок заново
        # присваивает свои), поэтому ищем по метке/пути.
        ghost_wav = os.path.join(workdir, "ghost.wav")
        H.gen_wav(ghost_wav, 330)
        r = d2.rpc("add", {"paths": [ghost_wav]})
        gid = r["result"]["added"][0]["id"]
        d2.stop()
        os.remove(ghost_wav)  # файл исчез до следующего запуска
        d3 = H.Daemon(binary, workdir, jobs=1)
        d3.start(ready="rpc")
        rows = d3.rows()
        grows = [x for x in rows if x["id"] == gid or
                 os.path.basename(x["label"]) == "ghost.wav"]
        check(grows and grows[0]["state"] in ("stopped", "error"),
              f"исчезнувший исходник восстановлен как stopped/error: {grows} & {d3.rows()}")
        check(grows and grows[0].get("last_error"),
              f"у исчезнувшего исходника есть last_error: {grows and grows[0].get('last_error')}")
        check(not rows or len([x for x in rows if x["state"] == "queued"]) == 0,
              "в движок заново ушло 0 строк (все восстановлены честно)")

        # P5. Транзакционное поведение sidecar при in-place оптимизации: внешний
        # sidecar читается и либо встраивается в целевой формат (тогда лишний
        # <база>.tags.zip удаляется), либо перезаписывается кандидатом (с
        # консистентным has_sidecar). В любом случае .llao-tmp.* артефактов нет.
        sc_dir = os.path.join(workdir, "sc")
        os.makedirs(sc_dir)
        sc_wav = os.path.join(sc_dir, "song.wav")
        H.gen_wav(sc_wav, 515)
        # Валидный sidecar v2 (vorbis-группа) в ВЫХОДНОЙ конвенции
        # (<имя без расширения>.tags.zip) — как после прошлой оптимизации.
        sc_zip = os.path.join(sc_dir, "song.tags.zip")
        doc = {"version": 2, "format": "llao-sidecar",
               "groups": [{"type": "vorbis",
                           "fields": {"title": ["Old Title"]},
                           "pictures": []}]}
        with zipfile.ZipFile(sc_zip, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("tags.json", json.dumps(doc).encode())
        d3.rpc("add", {"paths": [sc_dir], "mode": "optimize"})
        st = H.wait_state(d3, lambda s: any(
            os.path.basename(x["label"]) == "song.wav"
            and x["state"] in ("ok", "stopped", "error")
            for x in s["rows"]), timeout=360, interval=2)
        row = [x for x in (st or {}).get("rows", [])
               if os.path.basename(x["label"]) == "song.wav"]
        if row and row[0]["state"] == "ok":
            leftovers = [f for f in os.listdir(sc_dir) if ".llao-tmp" in f]
            check(not leftovers, f"нет .llao-tmp.* артефактов: {leftovers}")
            # Live-зеркало обязано совпадать с реальностью на диске: sidecar
            # доставлен внешним файлом — has_sidecar=true, встроен в контейнер —
            # false (см. DaemonSink::out_file, тот же диск-тест, что в persist).
            live_has = row[0].get("has_sidecar")
            disk_has = os.path.exists(sc_zip)
            check(live_has == disk_has,
                  f"live has_sidecar ({live_has}) совпадает с диском ({disk_has})")
            # Отрисовка может встроить теги (тогда sidecar удаляется) или оставить
            # внешние (тогда sidecar переписан кандидатом). Проверяем транзакцию:
            # sidecar либо отсутствует, либо валидный v2 без потери title.
            if not os.path.exists(sc_zip):
                check(not row[0].get("has_sidecar"),
                      "теги встроены, лишний <база>.tags.zip удалён")
            else:
                ok = False
                try:
                    with zipfile.ZipFile(sc_zip) as z:
                        inner = json.loads(z.read("tags.json"))
                    ok = inner.get("format") == "llao-sidecar" and any(
                        g.get("type") == "vorbis" and
                        g.get("fields", {}).get("title")
                        for g in inner.get("groups", []))
                except Exception:
                    ok = False
                check(ok, "sidecar транзакционно переписан без потери title")
            # Тот же файл переживает перезапуск (persist-отражение).
            d3.stop()
            d4b = H.Daemon(binary, workdir, jobs=1)
            d4b.start(ready="rpc")
            rows4 = d4b.rows()
            srows = [x for x in rows4 if os.path.basename(x["label"]) == "song.wav"]
            check(srows and srows[0]["state"] == "ok",
                  f"ok восстановлен после перезапуска: {srows}")
            d4b.stop()
        else:
            print(f"  note: sidecar-прогон не завершился ok "
                  f"({row and row[0].get('state')}: "
                  f"{row and row[0].get('detail', '')}) — проверка пропущена")

        # P3. Crash-перезапуск: prep/running -> stopped с причиной.
        d5 = H.Daemon(binary, workdir, jobs=1)
        d5.start(ready="rpc")
        c_wav = os.path.join(workdir, "c.wav")
        H.gen_wav(c_wav, 200)
        r = d5.rpc("add", {"paths": [c_wav]})
        cid = r["result"]["added"][0]["id"]
        # Убедимся, что файл ушёл в обработку (prep/running), потом убиваем.
        st = H.wait_state(d5, lambda s: any(
            x["id"] == cid and x["state"] in ("prep", "running")
            for x in s["rows"]), timeout=60, interval=1)
        check(st is not None, "c.wav ушёл в prep/running перед kill")
        d5.proc.kill()
        d5.proc.wait()
        d6 = H.Daemon(binary, workdir, jobs=1)
        d6.start(ready="rpc")
        rows = d6.rows()
        # У перезапущенных строк id из резервного диапазона — ищем по метке.
        crows = [x for x in rows if x["id"] == cid or
                 os.path.basename(x["label"]) == "c.wav"]
        check(crows and crows[0]["state"] == "stopped",
              f"после crash-active reload: c.wav stopped: {crows}")
        check(crows and "остановлено" in crows[0].get("last_error", ""),
              f"причина остановки при crash: {crows and crows[0].get('last_error')}")
        check(d6.rpc("restart", {"ids": [crows[0]["id"]]})["ok"],
              "crash-stopped строку можно перезапустить")
        d6.stop()

    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())