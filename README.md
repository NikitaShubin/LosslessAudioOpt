# LLAO — Lossless Audio Optimizer

Recompresses lossless audio into the smallest **lossless** format: it brute-forces
every supported format and compression level, verifies the result and carries the
tags over.

Of all candidates the file with the smallest total size (audio + tags) wins. The
winner is validated byte-for-byte and replaces the original file. Audio and tags
survive without loss.

> Savings depend heavily on the content: on ordinary music OptimFROG or TAK wins
> on average, while on noise-like material (studio noise, sound effects) Monkey's
> Audio often wins. Use `llao.exe stats` to see how it goes for your own collection.

## Quick start

1. Download the latest `llao-v<version>-win64.zip` from
   [Releases](https://github.com/NikitaShubin/LosslessAudioOpt/releases) and unpack it
   anywhere (nothing to install). All codecs are already in the archive, so no
   internet connection is needed while it works.
2. Run it from the command line:

   ```bat
   llao.exe tools
   llao.exe optimize "D:\Music" --jobs=8
   ```

   `tools` checks/downloads the codec utilities once (checksums are verified while
   downloading). `optimize` recompresses every audio file in the folder, recursively.

   On Linux (under wine) `llao.exe` comes with an `llao` wrapper that covers
   everything wine does not provide out of the box, without touching `llao.exe`:

   - **Daemon singleton.** The named mutex `Local\llao-singleton` in wine lives
     within a single wineserver (= a single `WINEPREFIX`), so all runs go to one
     fixed prefix (`~/.llao-wine`, overridable via `LLAO_WINEPREFIX` or `WINEPREFIX`)
     — a second `serve` copy is rejected, just like on Windows.
   - **Prefix initialization.** A fresh prefix is warmed up with `wineboot -u`
     (same as in the project's CI).
   - **Debugger noise** is suppressed (`WINEDEBUG=-all`, as in the project's tests),
     and wine's stderr goes to `${LLAO_LOG:-/tmp/llao-wine.log}`.
   - **Status bar width** is determined via `stty`/`tput` and passed through
     `LLAO_STATUS_SIZE` (`GetConsoleScreenBufferInfo` crashes in wine).

   ```sh
   ./llao tools
   ./llao optimize "/media/Music" --jobs=8
   ./llao serve
   ```

   If the executable bit got lost (e.g. after unpacking a zip) — use `sh llao ...`.

## Key features

- **No losses.** Audio is copied byte-for-byte (decode → reference PCM → encode →
  verify the decode against the reference). Tags are carried over completely: every
  tag format is kept as a separate group and is either embedded into the target
  format or moved into a ZIP sidecar next to the file.
- **Full brute force.** Every format and every compression variant is covered; the
  search for each file runs in its own process in parallel (`--jobs`).
- **Self-contained.** Console utilities of the codecs are downloaded automatically
  (with SHA-256 verification) and stored in `bin/<id>/`; the release archive already
  ships them.
- **Windows-native.** A single executable `llao.exe` (C++17, no install, no external
  libraries). On Linux it works under `wine` (via the `llao` wrapper) or natively
  (`make TARGET=linux`). Everything — CLI and headless server — lives in one binary.
- **Smart search order.** Statistics accumulate across runs (format, variant,
  compression level, content type); the most likely winners are tried first. Show
  the table with `llao.exe stats`.
- **Lossy inputs are skipped by default** (recompressing mp3/aac to lossless makes
  no sense); processing them requires `--allow-lossy`.
- **Staleness control for configs.** When a utility is downloaded, its `--help`
  output is compared against the config's expectation; a mismatch produces a warning.
- **Run log.** With `--debug` every event (launch, files, candidates, errors) is
  written to `runs/*.jsonl` — handy for analysing winners and investigating failures.
- **Daemon and web UI.** The `llao serve` subcommand is a headless process with an
  HTTP API and a web interface: the queue can be filled, reordered, paused and
  files restarted on the fly without tying up a terminal (see "Daemon and web UI").

## Formats

| id | Format | Extension | Utility | Notes |
|---|---|---|---|---|
| `flac` | FLAC | `.flac` | `flac.exe` | levels `-0..-8`, built-in MD5 |
| `wavpack` | WavPack | `.wv` | `wavpack.exe` | `-h/-hh` + `-x0..-x6`, MD5 |
| `monkeys_audio` | Monkey's Audio | `.ape` | `mac.exe` | levels `-c1000..-c5000` |
| `alac` | Apple Lossless | `.m4a` | `ffmpeg.exe` | no levels |
| `tta` | True Audio | `.tta` | `ffmpeg.exe` | no levels |
| `tak` | TAK | `.tak` | `takc.exe` | `-p0..-p4` + E/M levels (`-p4m`), CRC+MD5 |
| `optimfrog` | OptimFROG | `.ofr` | `ofr.exe` | presets `0..10` + `max`, MD5 |
| `la` | LA | `.la` | `la.exe` | 16-bit only, ID3v1 only |
| `mpeg4_als` | MPEG-4 ALS | `.m4a` | `ffmpeg.exe` | disabled (the encoder was dropped from newer ffmpeg) |

The ffmpeg formats (alac/tta/mpeg4_als) share one `bin/ffmpeg/` build. The full list
of parameters — `llao.exe variants`.

## CLI

```
llao.exe check-formats                        # validate the formats configuration
llao.exe tools [fmt_id ...] [--no-download]   # status/download utilities into bin/<id>/
llao.exe optimize <file|folder> [--jobs=N|M.F] # brute-force formats/parameters (recursive)
                 [--formats flac,wavpack]     # limit the set of formats (unknown id -> error)
                 [--report=<file|folder>]     # final report (or a directory for it)
                 [--no-download] [--dry-run]  # no downloading / no file replacement
                 [--allow-lossy]              # process lossy inputs too (mp3, aac, …)
                 [--verify=all|winner|none]   # candidate verification mode (all by default)
                 [--ignore-errors]            # file errors — mark skip, do not abort the run
                 [--tmp=<path>]               # temporary folder (next to the binary by default)
                 [--debug] [--no-stats]       # runs/*.jsonl log / do not accumulate stats.json
llao.exe restore <file|folder> [--jobs=N|M.F] # inverse optimization: return to the target
                 [--to=flac]                  # format (FLAC by default), tags come back
                 [--variant=<id>] [--no-download] [--allow-lossy]
llao.exe help <fmt_id> -- <arguments>         # run a codec utility (--help and so on)
llao.exe serve [options]                      # headless engine with HTTP API and web UI
llao.exe stats [--report=<file>]               # show accumulated statistics
llao.exe variants                             # list formats and compression variants
```

`llao.exe stats` prints the accumulated statistics: how many files were processed
and replaced, and the format ranking by average savings **on the files each format
won**. `llao.exe stats --report=<file>` writes the same ranking as a plain text
table with no localization — a compact, shareable summary of how each codec
performs on your material. The `stats.json` path can be overridden with the
`LLAO_STATS_FILE` environment variable.

One record in `stats.json` is one processed file: its source properties, every
candidate that was tried (size, sidecar, timings, verification mode, error), and
the winner. Since the whole run is recorded, any figure can be derived later from
the same base instead of being frozen at the moment it was first computed.

`--jobs=N` — an exact number of parallel processes; `--jobs=M.F` — a multiplier of
the available cores (e.g. `--jobs=1.5` on 16 cores gives 24 processes). The default
is `--jobs=2.0`, i.e. twice the core count. The number of simultaneously running
processes is additionally limited by free space on the tmp disk and by available
memory (the window adapts to the file sizes).

`--verify` controls candidate verification:
- `all` (default) — every candidate is checked with the format's built-in check
  (`flac -t` and so on), decoded and compared bit-for-bit against the reference PCM;
- `winner` — candidates are not verified while selecting, only the winner (the
  smallest one) is fully verified, before the file is replaced. Faster, but errors
  surface later, and only for one variant;
- `none` — no verification at all, immediate replacement. Maximum speed, risk of
  damaged files is on the user.

An error in any file (broken input, variant failures, unavailable utility, a winner
that failed verification) **aborts the run** by default: the running processes
(encoders/decoders in other threads) are terminated immediately, without waiting for
them to finish. With `--ignore-errors` such files are marked `error` (the original is
not replaced) and the run continues; `--ignore-errors` does not affect `restore`.

A failed file is **always** shown, regardless of the mode: an
`ERROR <file> — <reason>` line in the output, a row with `error` status in
`--report`, records for its variants in `stats.json`. Reasons that arrive from the
codecs' stderr as multiple lines are folded into one line so the report table is not
broken.

Without `--ignore-errors` a file error aborts the run and yields `errors: N` with
exit code `1` (`130` — only for Ctrl+C). With `--ignore-errors` a skip **is** the
expected behaviour: for every file the optimization proceeds among the successful
candidates, failed files are not counted as errors, the run is not aborted and the
exit code stays `0` — a non-zero code in this mode is only possible when the service
itself fails (configuration, no input files, a broken ffmpeg), not for an individual
file. Files that did not run to the end because of an abort are not drawn as empty
rows in the report, they are accounted for by a separate `Not processed` line.

An error in **one** variant closes the whole file in strict mode: work on it stops
and its remaining variants are cancelled. Task dots in the web interface tell the
culprit from the victims: a failed task is red (`failed`), one cancelled by an abort
caused by someone else's error is grey (`skipped`). In daemon mode and with
`--ignore-errors` a variant error does not close the file: the remaining variants
run to the end, and if the winner passes verification the file finishes as `ok` and
is not counted as an error.

### Interface language

The output is localized through `lang/<code>.json` catalogs. The language is chosen
in this order of priority:

1. The `--lang=ru|en` flag (anywhere on the command line);
2. the `LLAO_LANG=ru|en` environment variable;
3. the `"language"` field in `llao.json` next to `llao.exe` (`"auto"` by default);
4. autodetection (`LANG`, then the Windows user language).

For example, in the `llao.json` file next to `llao.exe`:

```json
{ "language": "ru" }
```

## Daemon and web UI

The `llao serve` subcommand runs the headless engine without console output;
control happens through the HTTP API and the web interface. Files and folders can
be added to an already running queue, reordered, restarted, finished ones cleaned up
— no terminal needed. There is one binary: `llao.exe` / `llao-linux`.

### Launch

```
llao serve [options]
```

Session options (optimization defaults):

- `--jobs N|M.F` — thread count or core multiplier (2.0 by default);
- `--verify all|winner|none` — candidate verification mode (winner by default);
- `--dry-run` — do not write the result;
- `--no-download` — do not download codecs (the startup gate only checks);
- `--no-stats` — do not accumulate `stats.json`;
- `--debug` — `runs/*.jsonl` log and debugging;
- `--report PATH` — final report on daemon shutdown (file or directory);
- `--restore-to ID` — target format of the restore mode (`flac` by default; the id
  comes from `formats/*.json` and is verified at startup).

Server options:

- `--bind ADDR` — listening address (`0.0.0.0` by default — all interfaces, the
  service is reachable from other machines on the local network);
- `--port N` — port (`18180` by default); `0` = pick a free one;
- `--token HEX` — explicit access token (otherwise generated and printed at startup);
- `--no-auth` — disable authorization (local debugging only);
- `--discovery PATH` — override the path to the discovery file.

For example:

```bat
llao serve
```

At startup the daemon prints the address, the token and the discovery file path; the
token is needed to enter the web interface. From another machine open
`http://<machine-IP>:18180/` and enter the same token. If the port is not exposed,
allow it in the firewall (Windows: `netsh advfirewall firewall add rule
name="llao" dir=in action=allow protocol=TCP localport=18180`). On Linux:
`wine llao.exe serve` or the native `./llao-linux serve`.

One daemon copy per session: a repeated launch is rejected (on Windows — a named
mutex `Local\llao-singleton`) so that queues and tmp folders are not shared. On Linux
under wine the same mutex lives in one wineserver — the wrapper (`./llao serve`)
guarantees a single `WINEPREFIX`, so the singleton works the same way; the native
`./llao-linux serve` has no singleton.

### Web interface

Open `http://127.0.0.1:<port>/` and enter the token. Features:

- adding folders/files (recursively) to a running queue — the "+" button in the
  header row opens a modal dialog; the "Output" field sets the output directory for
  both modes: the result is written to
  `<target folder>/<batch subfolder>/<file>` (the structure is preserved), the
  sources are not touched; an empty field means in-place replacement; the "Restore"
  flag turns on `restore` mode. In optimize mode, with a target folder set, the best
  candidate is written even if it is not smaller than the original (a full
  conversion of the batch); if there is no winner at all (no candidate passed
  validation) — the file is skipped;
- file status — the row's background colour (queued / preparing / working / done /
  stopped / error); progress: for working files — the share of completed tasks
  (blue), for finished ones — savings in percent (green). In restore mode the
  percentages are honest: when the file grows, the loss is shown in grey. The result
  path in the "File" column is relative to the target folder;
- bulk operations on selected rows: stop, start, move to top/up/down/end, remove;
  row dragging; sorting by path;
- pausing the whole queue, restarting stopped ones, clearing only successfully
  finished ones;
- a task status bar in the queue header row (between "+" and the action buttons):
  the share of completed tasks, including unpacking files to WAV. Rows with a built
  plan show the exact task count; rows before preparation show the number of session
  variants from `formats/*.json` (via `/api/formats`) as a preliminary estimate,
  counting nothing as done (once the plan is built, the number is replaced with the
  exact one). Clicking the status bar toggles autoscroll that follows the files being
  processed (and follows upwards when the list is cleaned or active rows move); a
  dark scrollbar.

The token and the autoscroll setting are remembered by the browser. In dev mode
(launched from a directory with the `web/` sources next to it) the daemon serves the
files straight from disk — HTML/JS/CSS edits are applied by refreshing the page
without a rebuild and without stopping active tasks; in a release the assets are
embedded into the executable.

### API

All requests except `GET /` and `/static/*` require the
`Authorization: Bearer <token>` header.

| Endpoint | Purpose |
|---|---|
| `GET /api/state` | snapshot: version, session options, counters, pause, queue rows |
| `GET /api/events?since=N` | event log (log/task/state/…) with `last_seq` for the delta |
| `GET /api/formats` | list of enabled formats |
| `POST /rpc` | JSON-RPC: `{ "cmd": "...", "args": {...} }` |

`/rpc` commands: `ping`, `stat`, `add`, `cancel-file`, `remove`,
`bulk-cancel`, `bulk-remove`, `clear-done`, `sort`, `restart`,
`pause`/`cancel-all`, `resume`, `reorder`, `shutdown`, `formats`, `debug`.

The discovery file is `%LOCALAPPDATA%/llao/daemon.json` on Windows,
`$XDG_DATA_HOME/llao/daemon.json` on Linux (overridden by `--discovery` or the
`LLAO_DISCOVERY` env var). It contains `port`, `token`, `pid`, `version` — allowing
clients to connect to a running daemon.

### Queue persistence

Next to the discovery file the daemon keeps `queue.json` (`version: 2` format, row
order = queue order). The state is updated on every queue change and at the end of
`shutdown`, therefore:

> **Do not launch a second daemon instance without specifying `--discovery`.**
> It will pick up the same production queue
> (`$XDG_DATA_HOME/llao/queue.json`) and start processing it in parallel: several
> processes write `queue.json` at the same time and burn all cores with codecs, and
> a "stuck" test instance can only be restarted through its own discovery file. For
> any debugging/verification isolate the instance: `llao serve --port N --discovery
> /tmp/llao-test/<id>.json` — `queue.json` will be created next to it and the
> production queue stays untouched.

- after a correct daemon restart `ok` rows are preserved, active ones continue
  processing, stopped/error ones stay in their places with a reason;
- rows store an absolute root (`root`) plus a path relative to it (`path`): the
  queue does not depend on the daemon's working directory, and a restart from any
  directory recognizes the sources (relative additions are absolutized at the moment
  of `add`); old rows without `root` keep the previous behaviour;
- after a crash (`kill -9`, machine shutdown) the active processing is not
  "disguised": on the next start unfinished rows are marked
  `stopped on restart (processing not finished)`, and `queued` rows go back into the
  queue (the sources and their sidecars are re-checked fairly; a vanished file is
  marked stopped with a reason);
- an `ok` row is restored with verification of the result and its sidecar by the
  actual path (`out_path`).

The result write is crash-safe: a candidate is first copied to a temporary file next
to it (`.<name>.llao-tmp.<ext>`), the old file is deleted, then the temporary file is
renamed into place — no backup copy and with the correct extension. The sidecar is
delivered by the same transaction (`.<name>.llao-tmp.tags.zip`): if the tags are
embedded into the target format, the outdated `<name>.tags.zip` is removed; if the
candidate needs an external sidecar — it is atomically replaced. If the sequence is
broken in between, `queue.json` keeps the original row, and the next launch will
honestly report unfinished work instead of showing a false "done".

## How it works (briefly)

1. The `formats/*.json` configs are read and validated.
2. `tools`: for every enabled format the utility is located — `bin/<id>/` cache →
   PATH → download by URL (SHA-256), archive unpacking or extraction from the
   installer via 7-Zip (no install; the downloaded installer is cached and is not
   re-downloaded).
3. For every file (in a separate process):
   - probe (ffprobe) → decode into a reference WAV; lossy inputs and files with a
     video stream are skipped (unless `--allow-lossy`);
   - extraction of the canonical tag set (text, pictures, lyrics, cuesheet,
     ReplayGain, chapters);
   - brute force over the applicable formats and compression variants;
   - for every candidate: encode → built-in check → decode → bit-for-bit comparison
     of PCM against the reference → tag writing (embedded or ZIP sidecar) → tag
     validation;
   - the winner = the candidate of minimal size.
4. The winner replaces the source (with the correct extension), a `.tags.zip`
   sidecar is placed next to it if needed.
5. The file is written to the local `stats.json` statistics with all of its
   candidates — they determine the search order in subsequent runs.
6. Everything is logged and summarized into a report: a format table, savings,
   exclusion reasons.

### Restore

`restore` returns files to their original format (FLAC by default) preserving the
tags. The interface is similar to the optimizer. For every file there are two
phases: decode the source → encode into the target format.

## Tags

- Every encountered tag format (ID3v2, RIFF LIST INFO, Vorbis comment, APEv2, ID3v1,
  MP4 ilst) is extracted as a separate group.
- When compressing, a single group is converted into the target format's native
  type; with several groups each one is embedded into its own type if the target
  supports it, and into a ZIP sidecar otherwise (`tags.zip` with `tags.json` and
  `pictures/`). Conflicting groups are not merged.
- When restoring (`restore`) the reconciled set is collapsed into one native group;
  old v1 sidecars are read too.
- A candidate's cost is the file size (+ sidecar); data is never lost.

## Building from sources

- Target OS: Windows 10+. MinGW-w64 cross-compilation; the working toolchain is
  [llvm-mingw](https://github.com/mstorsjo/llvm-mingw):
  `export PATH="$HOME/opt/llvm-mingw-<ver>-…/bin:$PATH" && make`.
- A dev container with the full environment (llvm-mingw + wine32/64 + ffmpeg + p7zip):
  `make docker-image docker-build` (builds `llao.exe` and `llao-linux`),
  `make docker-shell` — an interactive shell with the project in `/work`.
  This is handy for running wine-dependent logic (`llao.exe`, codecs) without
  installing anything on the host.
- Native Linux build: `make TARGET=linux`. The `llao` executable works natively;
  the codecs (Windows builds) are run under wine.
- Running under wine: `wine llao.exe …` (the codec utilities are Windows builds).
  On Linux the `llao` wrapper suppresses wine noise (`WINEDEBUG=-all`).
- Build dependencies: `nlohmann/json` and `miniz` live in `third_party/`.
- A single `llao` binary (`llao.exe` / `llao-linux`) is built: the engine
  (`optimize*.cpp`, `tags_*.cpp` — decomposed modules), the event layer
  (`serve_*.cpp`, `serve_internal.h`) and `main.cpp` (the subcommand dispatcher)
  are linked together (`Makefile`). Web assets are embedded via
  `tools/embed_assets.py` (regenerated when `web/*` changes).
- Architecture details and the history of decomposing the monoliths — see
  `docs/architecture-audit.md`.
- The format configs schema — see `formats/README.en.md`.

### Tests

- Native (no wine, built with g++):
  `make test-unit test-daemon-core && ./test-unit && ./test-daemon-core`.
- Server integration tests (need a built `llao-linux`, `ffmpeg` in PATH and codecs
  in `bin/`): `make TARGET=linux` then `make test-daemon`
  (restore → interactions → queue → persist). Each test brings up its own daemon
  on a random port (`--no-auth`), `18180` stays untouched. `test-daemon-persist`
  covers `queue.json`, graceful and crash restarts of the queue and the transactional
  sidecar delivery.
- CI: `.github/workflows/ci-linux.yml` — builds the linux binary, installs codecs
  with the native `llao-linux tools` (the Windows codec builds run under wine) and
  runs all the tests; `bin/` is cached by `formats/*.json`.
- Release run (`release.yml`, tag `v*`): `llao.exe` build under wine +
  llvm-mingw, codec CLI baselines, version freshness, tag transfer and errors.

## Documentation

- [README.ru.md](README.ru.md) — this document in Russian.
- [CHANGELOG.md](CHANGELOG.md) — what changed in each version (the text of the
  release page is taken from it).

## License

MIT — see [LICENSE](LICENSE). The codecs in `bin/` are distributed under their own
authors' licenses (see `README`/`LICENSE.txt` inside the format directories).
