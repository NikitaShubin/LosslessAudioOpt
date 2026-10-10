#!/usr/bin/env python3
"""Два демона из одного каталога не должны мешать друг другу.

Регрессия. Каталог сессий у демона был общим для всех процессов каталога:
`Engine::init` писал прямо в `tmp/` и стирал его целиком при старте и при
остановке, а имя сессии — это PID и счётчик. Второй демон из того же
каталога получал те же имена и тот же `ref.wav`, и один из них удалял
рабочий каталог другого прямо посреди прогона.

На живой библиотеке это выглядело как «tak не читает ref.wav», «wavpack:
can't open file ref.wav» и «data chunk extends beyond the file» на обычных
16/44.1/стерео файлах — то есть как поломка кодеков там, где кодеков не
виновато. У `serve` вообще нет ключа `--tmp`, развести демоны по каталогам
пользователь не может.

Проверяем ровно этот сценарий: два демона из одного бинарника, каждый со
своим файлом, оба должны дойти до успеха.
"""

import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _daemon_harness import Daemon, gen_wav, resolve_daemon, wait_state  # noqa: E402

FAILURES = []


def check(cond, msg):
    if cond:
        print("  ok: " + msg)
    else:
        print("FAIL: " + msg)
        FAILURES.append(msg)


def main():
    binary = resolve_daemon(sys.argv[1:])
    if not binary:
        print("SKIP: нет собранного llao-linux")
        return 0
    workdir = tempfile.mkdtemp(prefix="llao-pair-")
    try:
        # Оба демона — один и тот же бинарник, значит один и тот же tmp.
        os.makedirs(os.path.join(workdir, "a"))
        os.makedirs(os.path.join(workdir, "b"))
        a = Daemon(binary, os.path.join(workdir, "a"), jobs=2, extra=["--no-download"])
        b = Daemon(binary, os.path.join(workdir, "b"), jobs=2, extra=["--no-download"])
        wa = os.path.join(workdir, "a", "one.wav")
        wb = os.path.join(workdir, "b", "two.wav")
        gen_wav(wa, 440, 0.3)
        gen_wav(wb, 660, 0.3)

        a.start()
        print("case 1: демон A уже работает, поверх него поднимается демон B")
        a.rpc("add", {"paths": [wa]})

        def working(st):
            return any(r["state"] in ("prep", "running") for r in st["rows"])

        wait_state(a, working, timeout=120)

        # Именно этот момент и ломал прогон: init демона B стирал общий tmp
        # вместе с ref.wav файла, который в этот момент кодировался.
        b.start()
        b.rpc("add", {"paths": [wb]})

        def done(st):
            rows = st["rows"]
            return bool(rows) and all(r["state"] in ("ok", "error") for r in rows)

        wait_state(a, done, timeout=300)
        wait_state(b, done, timeout=300)
        ra = a.get("/api/state")["rows"]
        rb = b.get("/api/state")["rows"]
        check(len(ra) == 1 and len(rb) == 1, f"по одной строке у каждого: {len(ra)}, {len(rb)}")
        check(all(r["state"] == "ok" for r in ra),
              "работавший демон A не пострадал: " + (ra[0].get("last_error") or "ok")[:120])
        check(all(r["state"] == "ok" for r in rb),
              "демон B не пострадал: " + (rb[0].get("last_error") or "ok")[:120])
        rc_a, rc_b = a.stop(), b.stop()
        check(rc_a is not None, f"демон A завершился сам: {rc_a}")
        check(rc_b is not None, f"демон B завершился сам: {rc_b}")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())