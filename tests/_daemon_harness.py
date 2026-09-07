#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Общий харнесс для интеграционных тестов сервера.

Единая точка подъёма llao-linux против живого HTTP-API на случайном
порту (--no-auth, --jobs=2.0 — множитель ядер, свой discovery-файл
через LLAO_DISCOVERY).

Используется tests/test_daemon_queue.py, test_daemon_interactions.py,
test_daemon_restore.py и tests/dump_events.py; чтобы тесты не дублировали
запуск процесса, HTTP-клиент и генерацию wav.

Сервер всегда запускается из корня репозитория (cwd=ROOT): кодек-утилиты
ищутся в bin/<id>/ относительно рабочей директории.

Все поднятые серверы регистрируются и завершаются через atexit — даже упавший
или прерванный тест не оставляет процесс (прежние брошенные процессы грузили
CPU, делили bin/ и создавали флейки в shutdown-таймаутах).
"""
import atexit
import glob
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DAEMON = os.path.join(ROOT, "llao-linux")

_ALIVE = []


def format_extensions():
    """Расширения всех включённых кодеков из formats/*.json (без точки, нижний
    регистр). Источник истины — конфиги, а не хардкод в тестах."""
    exts = []
    for f in glob.glob(os.path.join(ROOT, "formats", "*.json")):
        try:
            d = json.load(open(f))
        except (OSError, json.JSONDecodeError):
            continue
        ext = d.get("extension")
        if ext:
            exts.append(str(ext).lstrip(".").lower())
    return exts


def _cleanup_alive():
    """Гарантированно завершает демонов, поднятых тестами.

    Вызывается atexit: даже если тест упал по исключению (assert, NameError и
    т.п.) или был прерван, ни один поднятый демон не останется висеть и
    мешать последующим прогонам.
    """
    for d in list(_ALIVE):
        try:
            if d.proc is not None and d.proc.poll() is None:
                d.proc.terminate()
                try:
                    d.proc.wait(timeout=5)
                except Exception:
                    d.proc.kill()
                    d.proc.wait()
        except Exception:
            pass


atexit.register(_cleanup_alive)


def free_port():
    """Свободный TCP-порт на 127.0.0.1."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def gen_wav(path, freq, duration=0.25):
    """Короткий тестовый wav (0.25 с по умолчанию), pcm_s16le, 44100/2
    через ffmpeg (должен быть в PATH). Чем короче файл — тем быстрее
    обработка (кодеки/декодеры/verify), а суть тестов от длины не зависит."""
    r = subprocess.run(["ffmpeg", "-y", "-f", "lavfi", "-i",
                        f"sine=frequency={freq}:duration={duration}",
                        "-c:a", "pcm_s16le", "-ar", "44100", "-ac", "2", path],
                       capture_output=True)
    if r.returncode != 0 or not os.path.exists(path):
        raise RuntimeError("ffmpeg failed")


def ffmpeg_available():
    return shutil.which("ffmpeg") is not None


def resolve_daemon(argv, default=None):
    """Бинарник демона из --daemon <path> либо по умолчанию."""
    if "--daemon" in argv:
        return argv[argv.index("--daemon") + 1]
    return default or DEFAULT_DAEMON


def wait_state(d, pred, timeout=120, interval=3):
    """Поллит /api/state пока pred(state) или до таймаута; None при таймауте."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = d.get("/api/state")
        if pred(last):
            return last
        time.sleep(interval)
    return last


class Daemon:
    """Живой демон на случайном порту с компактным HTTP/RPC-клиентом."""

    def __init__(self, binary, workdir, jobs=2.0, extra=()):
        """jobs — число воркеров: передаётся в --jobs как есть; дефолт 2.0 —
        множитель числа ядер (позволяет движку использовать все ядра машины)."""
        self.binary = binary
        self.workdir = workdir
        self.jobs = jobs
        self.port = free_port()
        self.disc = os.path.join(workdir, "daemon.json")
        self.log = os.path.join(workdir, "daemon.log")
        self.proc = None
        self.cmd = [binary, "serve", "--port", str(self.port), "--no-auth",
                    "--jobs", str(jobs), *extra]

    def start(self, extra=(), ready="state"):
        """Поднимает процесс и ждёт готовности (state или rpc ping)."""
        cmd = list(self.cmd) + list(extra)
        with open(self.log, "w") as log:
            self.proc = subprocess.Popen(
                cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                env={**os.environ, "LLAO_DISCOVERY": self.disc})
        _ALIVE.append(self)
        # Стартовый гейт кодеков (cli_check через wine на Linux) может занимать
        # заметно больше 15 с — особенно когда префикс только что разогревался.
        deadline = time.time() + 120
        while time.time() < deadline:
            try:
                if ready == "rpc":
                    self.rpc("ping", {})
                else:
                    self.get("/api/state")
                return self
            except Exception:
                if self.proc.poll() is not None:
                    raise RuntimeError("daemon exited early, see " + self.log)
                time.sleep(0.2)
        raise RuntimeError("daemon did not start, see " + self.log)

    def _req(self, path, body=None, timeout=15):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}", data=data,
            headers={"Content-Type": "application/json"},
            method="POST" if body is not None else "GET")
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode())

    def get(self, path):
        return self._req(path)[1]

    def rpc(self, cmd, args=None):
        return self._req("/rpc", {"cmd": cmd, "args": args or {}})[1]

    def events(self, since):
        return self.get(f"/api/events?since={since}")

    def state(self):
        return self.get("/api/state")

    def rows(self):
        return self.get("/api/state")["rows"]

    def ids(self):
        return [x["id"] for x in self.rows()]

    def stop(self, force=False):
        """Вежливо shutdown, при ошибке — kill. Возвращает exit-код."""
        try:
            self.rpc("shutdown", {"force": force})
            self.proc.wait(timeout=15)
        except Exception:
            self.proc.kill()
            self.proc.wait()
        return self.proc.returncode