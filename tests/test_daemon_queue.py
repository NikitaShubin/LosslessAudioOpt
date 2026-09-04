#!/usr/bin/env python3
"""Интеграционный тест очереди демона (замысловатые сценарии).

Требует: собранный llao-daemon-linux (или llao-daemon.exe под wine — не здесь),
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
  8. shutdown: процесс завершается, discovery-файл удалён

Запуск: python3 tests/test_daemon_queue.py [--daemon ./llao-daemon-linux]
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAILURES = []


def check(cond, msg):
    if not cond:
        print(f"FAIL: {msg}")
        FAILURES.append(msg)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Daemon:
    def __init__(self, binary, workdir):
        self.binary = binary
        self.workdir = workdir
        self.port = free_port()
        self.disc = os.path.join(workdir, "daemon.json")
        self.log = os.path.join(workdir, "daemon.log")
        self.proc = None

    def start(self, extra=()):
        with open(self.log, "w") as log:
            self.proc = subprocess.Popen(
                [self.binary, "serve", "--port", str(self.port),
                 "--no-auth", "--jobs", "1", *extra],
                cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                env={**os.environ, "LLAO_DISCOVERY": self.disc})
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                self.get("/api/state")
                return
            except Exception:
                if self.proc.poll() is not None:
                    raise RuntimeError("daemon exited early, see " + self.log)
                time.sleep(0.2)
        raise RuntimeError("daemon did not start, see " + self.log)

    def _req(self, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}",
                                     data=data,
                                     headers={"Content-Type": "application/json"},
                                     method="POST" if body is not None else "GET")
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read().decode())

    def get(self, path):
        return self._req(path)[1]

    def rpc(self, cmd, args=None):
        return self._req("/rpc", {"cmd": cmd, "args": args or {}})[1]

    def stop(self):
        try:
            self.rpc("shutdown", {})
        except Exception:
            pass
        try:
            self.proc.wait(timeout=15)
        except Exception:
            self.proc.kill()
        return self.proc.returncode


def gen_wav(path, freq):
    r = subprocess.run(["ffmpeg", "-y", "-f", "lavfi", "-i",
                        f"sine=frequency={freq}:duration=1",
                        "-c:a", "pcm_s16le", "-ar", "44100", "-ac", "2", path],
                       capture_output=True)
    if r.returncode != 0 or not os.path.exists(path):
        raise RuntimeError("ffmpeg failed")


def wait_state(d, pred, timeout=120, interval=3):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = d.get("/api/state")
        if pred(last):
            return last
        time.sleep(interval)
    return last


def main():
    binary = sys.argv[sys.argv.index("--daemon") + 1] \
        if "--daemon" in sys.argv else os.path.join(ROOT, "llao-daemon-linux")
    if not os.path.exists(binary):
        print(f"SKIP: нет бинарника {binary}")
        return 0
    if shutil.which("ffmpeg") is None:
        print("SKIP: нет ffmpeg")
        return 0

    workdir = tempfile.mkdtemp(prefix="llao-qtest-")
    try:
        files = []
        for i, freq in enumerate((440, 880, 330)):
            p = os.path.join(workdir, f"t{i}.wav")
            gen_wav(p, freq)
            files.append(p)

        d = Daemon(binary, workdir)
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
        for _ in range(4):
            time.sleep(3)
            st = d.get("/api/state")
            ids = [x["id"] for x in st["rows"]]
            check(2 not in ids, f"no ghost row 2, got {ids}")
            check(len(ids) == len(set(ids)), f"no dupes: {ids}")
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
        st = wait_state(d, lambda s: any(
            x["state"] in ("ok", "skip", "error") for x in s["rows"]), timeout=180)
        done = [x for x in st["rows"] if x["state"] in ("ok", "skip", "error")]
        check(bool(done), f"at least one done file: {st['counters']}")

        # 6. restart остановленного через cancel-file файла: замена, не append.
        # Детерминированно: добавляем свежий файл, сразу cancel-file (пока он
        # queued/prep — исходник на месте, не заменён), затем restart. Это
        # ровно пользовательский сценарий «остановил -> запустил выделенные».
        rst_wav = os.path.join(workdir, "restart_target.wav")
        gen_wav(rst_wav, 640)
        r = d.rpc("add", {"paths": [rst_wav]})
        new_id = r["result"]["added"][0]["id"]
        d.rpc("cancel-file", {"id": new_id})
        time.sleep(1)
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
        gen_wav(race_wav, 660)

        def _add_once():
            return d.rpc("add", {"paths": [race_wav]})

        with _fut.ThreadPoolExecutor(max_workers=8) as ex:
            results = list(ex.map(lambda _: _add_once(), range(8)))
        total_added = sum(len(r["result"]["added"]) for r in results if r.get("ok"))
        check(total_added == 1, f"concurrent add -> exactly 1 added: {results}")
        st = d.get("/api/state")
        matches = [x for x in st["rows"] if x["label"] == "race.wav"]
        check(len(matches) == 1, f"single race.wav row: {matches}")

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
