#!/usr/bin/env python3
# Интеграционный тест режима «восстановление» (restore) демона: целевая папка,
# структура /source/** -> /target/**, исходник не трогается, «уже целевой
# формат» происходит сразу без пережима, restart сохраняет семантику строки.
#
# Требует собранный llao-linux (make TARGET=linux) и ffmpeg в PATH.
# Поднимает свой демон на случайном порту (18180 не трогает).
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _daemon_harness as H

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print(f"FAIL: {msg}")
    else:
        print(f"  ok: {msg}")


def main():
    binary = H.resolve_daemon(sys.argv)
    if not os.path.exists(binary):
        print(f"SKIP: нет бинарника {binary}")
        return 0
    if not H.ffmpeg_available():
        print("SKIP: нет ffmpeg")
        return 0

    workdir = tempfile.mkdtemp(prefix="llao-restest-")
    try:
        # 0. --restore-to с неизвестным форматом: старт отклоняется (ранее
        #    молча давал ok с 0 задачами).
        with open(os.path.join(workdir, "bad-restore-to.log"), "w") as fl:
            bad = subprocess.Popen(
                [binary, "serve", "--no-auth", "--jobs", "2.0",
                 "--restore-to", "no_such_fmt"],
                cwd=H.ROOT, stdout=fl, stderr=subprocess.STDOUT)
            rc = bad.wait(timeout=15)
        check(rc != 0, f"--restore-to=no_such_fmt отклоняется (rc={rc}, ок если !=0)")
        target = os.path.join(workdir, "target")
        os.mkdir(target)

        src = os.path.join(workdir, "src")
        os.mkdir(src)
        src_sub = os.path.join(src, "sub")
        os.mkdir(src_sub)
        a = os.path.join(src, "a.wav")
        b = os.path.join(src_sub, "b.wav")
        H.gen_wav(a, 440)
        H.gen_wav(b, 880)

        d = H.Daemon(binary, workdir)
        d.start()

        # 1. restore папки рекурсивно: Rel-структура воспроизводится в target,
        #    исходники не трогаются, результат — ok со status restore.
        r = d.rpc("add", {"paths": [src], "mode": "restore", "target_dir": target})
        check(r["ok"] and len(r["result"]["added"]) == 2,
              f"restore add dir: {r}")
        rows = H.wait_state(d, lambda s: all(x["state"] == "ok"
                                           for x in s["rows"] if x["state"] != "error"))
        check(rows is not None, "restore: все строки завершены")
        ids = {x["label"]: x for x in rows["rows"]}
        check(ids.get("a.wav") and ids["a.wav"]["state"] == "ok",
              "(a.wav) state ok")
        check(ids.get("sub/b.wav") and ids["sub/b.wav"]["state"] == "ok",
              "(sub/b.wav) state ok")
        check(ids["a.wav"]["mode"] == "restore", "(a.wav) mode=restore")
        check(ids["a.wav"]["out_path"] == os.path.join(target, "a.flac"),
              f"(a.wav) out_path -> {ids['a.wav']['out_path']}")
        check(ids["sub/b.wav"]["out_path"] == os.path.join(target, "sub", "b.flac"),
              f"(sub/b.wav) out_path -> {ids['sub/b.wav']['out_path']}")
        check(ids["a.wav"]["pct"] > 0, f"(a.wav) pct>0 ({ids['a.wav']['pct']})")
        check(os.path.exists(os.path.join(target, "a.flac")),
              "target/a.flac существует")
        check(os.path.exists(os.path.join(target, "sub", "b.flac")),
              "target/sub/b.flac существует")
        check(os.path.exists(a) and os.path.exists(b),
              "исходники не тронуты")

        # 2. вход уже целевого формата (flac): ok сразу, fs не пережимает, метка
        #    считается результатом без out_path.
        flac_in = os.path.join(src, "c.flac")
        subprocess.run(["ffmpeg", "-y", "-i", a, "-c:a", "flac", "-f", "flac",
                        flac_in], capture_output=True, check=True)
        r = d.rpc("add", {"paths": [flac_in],
                           "mode": "restore", "target_dir": target})
        check(r["ok"] and len(r["result"]["added"]) == 1, "add flac: 1 МБ")
        row = H.wait_state(d, lambda s: any("c.flac" in x["label"]
                                          and x["state"] == "ok" for x in s["rows"]))
        check(row is not None, "flac уже целевой: ok")
        flac_row = next(x for x in row["rows"] if "c.flac" in x["label"])
        check(not flac_row["out_path"], "уже flac: out_path пуст (файл не пережат)")
        check(os.path.exists(flac_in), "уже flac: исходник цел")

        # 3. двухрежимный дедуп: активная restore-строка того же пути отклоняет
        #    повторное добавление и в optimize, и в restore.
        wav2 = os.path.join(src, "d.wav")
        H.gen_wav(wav2, 523)
        r1 = d.rpc("add", {"paths": [wav2], "mode": "restore", "target_dir": target})
        check(r1["ok"] and len(r1["result"]["added"]) == 1, "add d.wav restore")
        r2 = d.rpc("add", {"paths": [wav2], "mode": "optimize"})
        check(not r2["result"]["added"], f"dup optimize rejected: {r2}")
        r3 = d.rpc("add", {"paths": [wav2], "mode": "restore", "target_dir": target})
        check(not r3["result"]["added"], f"dup restore rejected: {r3}")

        # 4. restart: новая строка сохраняет режим restore и целевую папку,
        #    пока исходник существует.
        row = H.wait_state(d, lambda s: any("d.wav" in x["label"]
                                          and x["state"] == "ok" for x in s["rows"]))
        check(row is not None, "d.wav завершён")
        old = next(x for x in row["rows"] if "d.wav" in x["label"])
        rr = d.rpc("restart", {"id": old["id"]})
        check(rr["ok"], f"restart d.wav: {rr}")
        rows2 = H.wait_state(d, lambda s: any(
            "d.wav" in x["label"] and x["id"] != old["id"] for x in s["rows"]))
        check(rows2 is not None, "после restart новая строка на месте")
        nw = next(x for x in rows2["rows"] if "d.wav" in x["label"]
                  and x["id"] != old["id"])
        check(nw["mode"] == "restore", f"restart сохранил mode=restore: {nw['mode']}")
        check(nw["target_dir"] == target,
              f"restart сохранил target_dir: {nw['target_dir']}")

        # 5. целевая папка обязана существовать; отсутствие — rejected.
        missing = os.path.join(workdir, "no-such-target")
        r4 = d.rpc("add", {"paths": [wav2], "mode": "restore", "target_dir": missing})
        check(r4["result"]["rejected"],
              f"несуществующая target rejected: {r4}")

        rc = d.stop()
        check(rc == 0, f"shutdown exit 0 (got {rc})")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())