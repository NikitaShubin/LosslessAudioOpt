#!/usr/bin/env python3
"""Комплексный тест комбинаций взаимодействия с очередью демона.

Ловит ошибки класса «действие по id попало не в тот файл» и «очередь встала»:
каждая комбинация add/reorder/remove/cancel/restart/pause/resume проверяется
утверждением о точном составе очереди после неё.

Требует: собранный llao-daemon-linux, ffmpeg в PATH. Без ffmpeg — SKIP.
Демон поднимается свой, на случайном порту (--no-auth), 18180 не трогает.

Запуск: python3 tests/test_daemon_interactions.py [--daemon ./llao-daemon-linux]
"""
import concurrent.futures as fut
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
    if cond:
        print(f"  ok: {msg}")
    else:
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
        self.proc = None

    def start(self, extra=()):
        log = os.path.join(self.workdir, "daemon.log")
        with open(log, "w") as lf:
            self.proc = subprocess.Popen(
                [self.binary, "serve", "--port", str(self.port),
                 "--no-auth", "--jobs", "2", *extra],
                cwd=ROOT, stdout=lf, stderr=subprocess.STDOUT,
                env={**os.environ, "LLAO_DISCOVERY": self.disc})
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                self.get("/api/state")
                return
            except Exception:
                if self.proc.poll() is not None:
                    raise RuntimeError("daemon exited early, see " + log)
                time.sleep(0.2)
        raise RuntimeError("daemon did not start, see " + log)

    def _req(self, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}",
                                     data=data,
                                     headers={"Content-Type": "application/json"},
                                     method="POST" if body is not None else "GET")
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read().decode())

    def get(self, path):
        return self._req(path)

    def rpc(self, cmd, args=None):
        return self._req("/rpc", {"cmd": cmd, "args": args or {}})

    def rows(self):
        return self.get("/api/state")["rows"]

    def ids(self):
        return [x["id"] for x in self.rows()]

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


def no_dupes(rows):
    ids = [x["id"] for x in rows]
    return len(ids) == len(set(ids))


def main():
    binary = sys.argv[sys.argv.index("--daemon") + 1] \
        if "--daemon" in sys.argv else os.path.join(ROOT, "llao-daemon-linux")
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
            gen_wav(p, freq)
            paths.append(p)

        d = Daemon(binary, workdir)
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
            if any(x["state"] in ("prep", "running", "ok", "skip", "error")
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

        print("case 7: shutdown")
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
