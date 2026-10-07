# Changelog

All notable changes to LLAO, newest first. This file is the base text; [CHANGELOG.ru.md](CHANGELOG.ru.md) is the Russian translation of the same content. The release page text is taken from this file (see `release.yml`), so a version section here is what users see on the release.

Versions 1.x shipped a terminal status bar and a `llao-daemon` process. **Both were removed in 2.0** — the engine now runs headless and the interface is a browser. Sections for 1.x therefore describe an interface that no longer exists; the engine behaviour they describe is still current.

## 2.4.0 — 2026-10-07

### Statistics: a journal to debug in and a table to count with

A real library run had produced a 138 MB `stats.json` for 5134 files. Two thirds
of it was the same information written 387 009 times: 75 candidates per file,
each repeating its format, variant, status and timings. The file was also
rewritten from scratch for every finished file — `append` re-read the whole base
and dumped it back — which on that run was 5134 full rewrites.

Statistics are now two files with different jobs.

**`stats.jsonl` is the journal.** Append-only, one line per finished file, with
everything known about it: source properties, the size of the WAV it was decoded
to, every candidate with its size, sidecar, timings, verification mode and error,
the variants a codec refused on its own limits, and the winner. Writing a record
is now a single append with no read. This is the place to debug — `llao stats
--dump=<file>` writes it as one JSON array. An existing `stats.json` is still read
if there is no journal yet, so old databases keep working untouched.

**`stats.tsv` is the result table.** One row per file, one column per
`format:variant` holding that variant's size, plus the source properties, the
size of the uncompressed WAV and the winner. On that same library it comes to
about 4.5 MB — roughly 30 times smaller. A cell is empty when the variant was
never tried, `NA` when the codec's own limits ruled it out for this file, and a
number otherwise. Rows are keyed by the path left on disk: once a file is
converted its original is gone, and there would be nothing to look it up by. A
file seen in several runs is merged keeping the most complete result, so a later
run that retries only the failed variants fills the gaps instead of overwriting
them. Files that did not finish are not in the table at all.

The table is rebuilt from the journal when work ends, not while it is in flight —
in the daemon, once nothing has been in flight for 30 seconds. A paused queue
does not hold that back: the trigger is the absence of work, not an empty file
list. `llao stats --export-tsv=<file>` writes it on demand. Paths are overridable
with `LLAO_STATS_JOURNAL`, `LLAO_STATS_TSV` and `LLAO_STATS_FILE`.

`llao stats` and the format ranking now read the table rather than the journal.
A side effect worth having: the order in which formats are tried no longer shifts
in the middle of a run as records accumulate — it comes from a snapshot taken
before the run starts.

### A file that was never written no longer counts as delivered

The record for a finished file was written before the file was handed over, so a
file whose replacement failed — or whose size on disk did not match the verified
candidate — was recorded as `ok` with savings that did not exist. The record is
now written after the handover, when the final status and the resulting path are
both known. That also makes the table key exact instead of guessed.

### WAV size

Every record and every table row carries the size of the uncompressed WAV the
file was decoded to. Without it the sizes of different codecs are not comparable:
it is the denominator the compression ratio is measured against, and it is not
recorded anywhere else.

## 2.3.9 — 2026-10-06

### Strict mode now really is strict
Without `--ignore-errors`, a failure in any single variant now closes the whole file — it was supposed to already, but the rule only applied with `--verify=all`. The daemon runs `--verify=winner` by default, so a variant failure was quietly ignored: 54 finished files on a real library were reported `ok` while carrying a red dot on a variant that was never checked.

The reasoning is not about the winner. A variant we neither checked nor fixed could have been the better one, so a result with a hole in its verification cannot be handed out in strict mode. That is the whole point of the mode. Taking "any one correct variant" is what `--ignore-errors` does — it excludes the broken variant from the selection and lets the file through.

Closing a file means stopping it: every remaining process for that file is terminated immediately whatever its state, and no further variants of it are started.

Four defects surfaced while fixing this:

- the failure counter only advanced on exceptions, while an ordinary encoder refusal returns normally with no exception, so the strict check never fired;
- victims of a stop (processes killed mid-flight) were reported as `failed` instead of `skipped`, making one culprit look like dozens;
- a stopped file could never close: its task accounting was skipped, so the completion condition never held and the row hung in `running` forever;
- the scheduler kept handing work to an already stopped file, and the planner and a running worker then disagreed about whether anything was left to do.

The culprit is still marked red and keeps its own reason; victims are grey. The reason now reaches the web row instead of a bare "variant failed" — in strict mode the diagnostics are the point.

### Strict mode concerns the file, never the queue
An error marks one row and the rest of the queue keeps running — in no scenario does one file stop the queue. Only CLI `optimize` stops at the first failing file, which is its long-standing behaviour for scripts, and it still reports each failure by name with its reason rather than a bare `Aborted: N`.

## 2.3.8 — 2026-10-06

### The web interface stopped showing "Failed to fetch" while the queue was busy
On a large library the browser stopped working whenever the queue was busy: the top-right indicator read **Failed to fetch** and the file list froze. The cause was `/api/state`, which the page requests once a second. It returns the entire queue, and building it takes a deep copy of the whole state mirror — 5307 rows measured **37 MB and 161 seconds** under load, against 4–6 seconds on an idle queue. The browser gave up long before that.

Building the document now happens in a background thread. A request is answered immediately from the last completed document, so response time no longer depends on how loaded the queue is. If the queue is quiet enough that a rebuild is cheap (under a second), it is done synchronously, so a caller reading the state right after a change still sees that change. On a loaded queue the previous document is served instead — data a second behind beats waiting minutes.

The response also got about half the size: **37 MB → 18.6 MB**. `task_infos` on finished files were 84% of the payload, and the page only reads the format and variant from them (to draw variant dots and mark the winner). Parameters and notes are now sent for files that are running or failed, and omitted for finished ones.

### Restarting many files no longer times out
Restarting a batch of files could take longer than the client waited, and the client then repeated the request — which restarted the same files a second time. The web now shows *already running: N* instead of reporting the repeat as lost work.

The queue file is written once per batch rather than once per file: on a 5307-row queue a single write serialises 28 MB, so a batch of 21 restarts wrote it 21 times. One restart batch measured 336 seconds, down from timing out at 900. Rows whose id is no longer known are reported separately, so a repeat is never mistaken for lost work.

A stale id could also add a second row for a file that was already queued: cancelled files leave a record behind, and looking the id up in the engine found it again. Cancelling, restarting and restarting again produced two rows for one file. Restart now requires the row to be visible in the list the user is looking at, otherwise it reports the id as gone.

### The listening backlog is 1024 instead of 5
`httplib` defaults to a listen backlog of 5, so five unaccepted connections were enough — the kernel then drops the handshake and the client sees a timeout. Under codec load this looked like the API and the web had died. The backlog is now 1024 (`-DCPPHTTPLIB_LISTEN_BACKLOG`; the vendored header is not patched).

### `llao-linux serve` now stops on Ctrl-C and SIGTERM
On Linux the signal handler set a flag that nothing ever read, so the daemon kept listening and had to be killed with `SIGKILL` — `systemctl stop` did not work. The regression came with splitting `serve.cpp` into modules. A watcher thread now turns the flag into a shutdown.

## 2.3.7 — 2026-10-05

### ALAC files no longer fail with `moov is corrupted`
ALAC files whose audio data happens to contain the four bytes `moov` were rejected outright with `moov is corrupted`, on both reading and writing. MP4 is a box format: the file is a sequence of boxes, each with a declared size and a four-letter name, and the real `moov` box — the one holding the tags — was being found by searching for that word in the raw bytes. Audio data is compressed and therefore looks random, so on a large file the odds of those four bytes appearing inside the sound data are good; one real file in the library had them 10.9 MB into the audio, with the actual `moov` another 28 MB further on.

Boxes are now walked properly: each box is read at its own offset with its own declared size, and the tag box is the one the file structure actually points to. The search also now handles 64-bit box sizes (the `largesize` form used by files over 4 GB), boxes that run to the end of the file, and boxes whose declared size leaves no room for a header — all previously either misread or rejected.

### The browser says which variant failed, and why
A failed variant was a red dot with a tooltip naming the format and variant but no reason, and the only error text shown was the file's own, which on a file with dozens of variants does not say which variant failed or why. Hovering a red dot now shows the reason for that variant — for example which encoder rejected the input. The reason is stored with the row, so it survives a daemon restart; only failed variants carry one, because the full state is already tens of megabytes on a large library.

### The file row tooltip no longer spans the screen
The row tooltip held the full path, which turned a one-line hint into a multi-line panel that covered the rows underneath it — while the shortened path was already visible in the row itself. The tooltip now shows the same shortened path. The full path is still available in the command line and in the API response.

### A lost lyrics field now says what was lost
The verification message for a dropped lyrics field was `lyrics did not survive` with nothing else, so there was no way to tell an empty field from a large one. It now reports the size and the opening characters. Other fields already did this.

## 2.3.6 — 2026-10-04

### An APE cover with a non-standard key no longer crashes the run
Some rippers write the cover-art key as `Cover Art (front)` with a lowercase letter. The parser matched the key exactly against `Cover Art (Front)` / `Cover Art (Back)`, so such an image was not recognised as a picture and went into a text field with all of its JPEG bytes. For a format without native picture support the group moves to a sidecar, and writing that sidecar failed with `[json.exception.type_error.316] invalid UTF-8 byte at index 4041: 0xAB`, aborting the variant.

The cover key is now matched case-insensitively, so the image is stored as a picture (inside the archive) rather than a text field.

### The UTF-8 sanitizer no longer lets values above U+10FFFF through
`sanitize_utf8` checked for overlong encodings (`F0` + byte under `0x90`) and illegal leading bytes (above `F4`), but not the upper bound: `F4` followed by a byte above `0x8F` encodes a codepoint greater than U+10FFFF, which the strict JSON writer rejects. One of the affected covers contained exactly such a sequence (`F4 AB B5 9E`), so even sanitised tags failed to dump. Such sequences are now scrubbed like any other invalid byte.

## 2.3.5 — 2026-10-03

### The browser no longer downloads the whole list every second
The page refreshed itself by fetching the complete daemon state once per second. That state carries the per-variant plan (`task_infos`) for every row, so on a real library it is megabytes — 4.7 MB for 5,300 files, and growing with the library. Over a narrow port forward (or any slow link) the response does not arrive intact: the browser fails to parse it, and the page then shows an empty list, as if the queue had been lost. The daemon was answering correctly the whole time.

The page now asks for changes only — `/api/events?since=`, which is a few hundred bytes while work is running and empty when nothing moved — and downloads the full state only when the daemon reports that the page has fallen behind its event buffer (`resync`) or on the first load. A 5,300-file library drops from roughly 4.7 MB per second to a few hundred bytes per second.

### A dropped connection no longer empties the list
A truncated or empty response produced a bare `Unexpected end of JSON input` in the status line and nothing else, which tells the user neither what went wrong nor whether their work is still running. Responses are now checked before parsing, so the status line names the cause (empty response, or how many bytes arrived unparseable), and the previously received list stays on screen instead of being replaced by an empty one — a broken connection is visible as a broken connection, not as a lost queue.

### A kill -9 no longer silently restarts an interrupted file
`queue.json` is rewritten in full on every change, so on a real library the write lags behind the daemon's own view: the API can already show a file as `prep` while the file on disk still says `queued`. Killing the daemon inside that window left the interrupted file marked as never started, and the next launch put it back into the queue and processed it again without asking — the exact opposite of what an interrupted file is supposed to do.

A small `queue.active.json` next to `queue.json` now records the paths the engine has taken up, written synchronously at the moment a file enters `prep` and before the mirror is updated. On restart, a `queued` row whose path is in that file is restored as `stopped` with the reason "остановлено при перезапуске", exactly like an explicitly interrupted row. The marker is cleared when the reload finishes and on a graceful shutdown, so a completed file is never mistaken for an interrupted one.

## 2.3.4 — 2026-10-02

### Cancelling a file no longer locks its path in the queue
`cancel-file` (and the bulk/cancel-all variants) mark the row as `stopped` and leave it in the list, but the path stayed in the daemon's de-duplication set. Re-adding that file then hit the stale entry: on a running queue the add was refused as "already in queue", and on a finished row it created a second row for the same path. Both outcomes mean the list, the queue file and the actual work drift apart — the same class of inconsistency as the truncated queue in 2.3.3.

The paths of cancelled rows are now released, exactly as `remove` already did.

### Reordering the list can no longer half-apply
`reorder` received the visible list's order and passed it straight to the engine, which holds cancelled "zombie" rows the visible list never shows. The engine therefore rejected most real reorder requests, and the visible list could be left reordered while the engine kept its own order — the two diverging silently, and the file order surviving a restart differently from what the UI showed.

Reorder now translates the visible order into the engine's own positions and rejects only genuinely invalid input: duplicate ids, or ids the list does not contain. The visible list and the queue file are always updated together or not at all.

## 2.3.3 — 2026-10-02

### A restart no longer truncates the queue
Restoring the queue from `queue.json` added the files one at a time, while `persist()` was already being called by the engine's workers and by RPC threads. Every one of those writes saved only the part restored so far — so a daemon that died mid-restore left a truncated file behind, and the next start dutifully restored that truncated queue. On a 5,256-file library this silently lost 4,745 rows; the files themselves were untouched, but the work list was gone.

`persist()` no longer writes the queue file while a restore is in progress, so an interrupted restore leaves the previous file intact and the next start simply does it again. The previous file is also copied to `queue.json.bak` before the first rewrite.

The restore itself is now batched: files added in one `add` call (same mode and target folder) go back into the engine in one batch instead of one by one, which is what made the reload take minutes.

## 2.3.2 — 2026-10-02

### The daemon is intolerant to errors again
The daemon defaulted to `--verify=winner`, so a candidate that failed was simply excluded from the contest and the file was reported as done. On a real library run this is what it looked like: files delivered successfully while half of their candidates had been dropped for reasons nobody could see — and a candidate that disappears silently can cost a file its real best result, because the next format by size may be larger than the one that would have won.

`llao serve` now defaults to `--verify=all`, like the CLI: every variant is checked, and a failure in any of them aborts the file instead of being swallowed. `--verify=winner` is still there for speed.

### Track and disc in the `number/total` form survive the move to M4A
`track=3/13` and `disc=1/2` — the form almost every album uses — were lost in both directions when writing or reading M4A, so the ALAC candidate was rejected on every such file with "field 'track' did not survive":

- the writer used `std::stoul("3/13")`, which parses the leading digits and **stops at the slash without throwing**, so `trkn` got `3/0` — the exception that was  to catch a bad value never fired;
- the reader took only the first 16-bit number out of `trkn`/`disk` and discarded the second, reading back `3` instead of `3/13`.

Both are fixed: the value is split on `/` and both numbers are written, and the reader assembles the pair back into `num/total` (or just `num` when there is no total, which is how APEv2, Vorbis and ID3 write a lone value). Two new scenarios in `tests/test_tags.py` check the result through **ffprobe**, not through our own parser — a reader and a writer that are wrong in the same way would otherwise agree with each other and pass.

## 2.3.1 — 2026-10-02

### The overall status bar kept stale numbers after the queue was emptied
The bar showing overall queue progress is computed in the browser from the rows, and `renderQueue()` updated it at the *end* of the function. The empty-queue branch returned earlier, so removing the last rows left the bar showing the previous figures (`100% (42/42)`) next to an empty table. A page reload fixed it — not because the data differed, but because the initial load happened to reach `updateStatusbar()` from `loadFormats()`, a third path that only runs once.

`renderQueue()` now updates the status bar before any early return, so an emptied queue hides the bar immediately.

### Removal refreshes the list at once
Removing a row (per-row 🗑, per-row 🧹, «remove completed», or a bulk delete of selected rows) now re-reads the state right after the RPC succeeds instead of waiting for the next one-second poll.

## 2.3.0 — 2026-10-02

### One record per file in `stats.json`
`stats.json` used to hold one record **per candidate**, with the winner marked after the fact. That made the base impossible to reason about: a file took two or three rows, repeated runs duplicated entries, and the export could not show who won a file.

One record is now one processed file: its source properties, every candidate that was tried (size, sidecar, timings, verification mode, error) and the winner. Since the whole run is recorded, any figure can be derived from the same base later instead of being frozen at the moment it was first computed.

The ranking is now computed **on the files each format won** (the smallest candidate per file), not by averaging over all candidates. The old way was misleading in both directions: a format with many variants accumulated more "averages" than a format with one, and the sources behind them differed. FLAC used to show `-3.14%` savings on a real library; on the same data it is `43.78%`.

### Statistics output
`llao stats` and `llao stats --report=<file>` gained two histograms — the savings distribution and the source size distribution — and the export also carries a savings histogram **per format**, so a format with one lucky file no longer looks like a consistently better one. Files whose result grew instead of shrinking are counted in a separate `grew` bucket rather than disappearing.

Both views count `cost` (file + sidecar), and both skip lossy sources: converting mp3 to lossless never shrinks anything, so counting it made the summary claim savings that did not happen.

### Updating codecs
`llao tools --update-codecs` (and `llao serve --update-codecs`, which refuses to start if an update fails) brings the codecs up to their newest available versions and rewrites the pinned download recipes in `formats/*.json`. A codec is updatable when its config has a `latest` recipe; those without one are reported as `skipped`.

A new utility is checked against `cli_check.expect` before anything is replaced, and any failure restores the previous working binary and leaves the config untouched. Codecs without a hash source (a zip that is deleted after unpacking, or an evergreen `…/releases/latest/…` URL whose file changes on every download) keep an empty checksum instead of getting a meaningless one — recording it would reject the next installation.

### Monkey's Audio 13.27
Monkey's Audio is updated to **13.27** (the official release, built from the binaries its author sent ahead of time). It fixes the encoder refusing any source with an odd total number of PCM samples: 24-bit mono with an odd sample count used to fail with `Error: 1002`, while the decoder read such files without trouble. Odd lengths now compress, and the round-trip is byte-for-byte identical to the source.

Verified on the shipped binary: 24-bit mono at 1 / 3 / 4799 / 4801 / 96001 samples, `data` chunk sizes that are not a multiple of 3, a declared `data` size larger than the file, 16-bit mono and 24-bit stereo with an odd frame count. The new codec also handles what `caps` previously ruled out — **8 channels and 32-bit** are now in the comparison set instead of being excluded up front.

`llao tools --update-codecs` is what put the version in place: the file was fetched, its `--help` checked against `cli_check.expect`, and only then the pinned recipe in `formats/monkeys_audio.json` was rewritten.

### Test isolation
The daemon test harness did not pass `LLAO_STATS_FILE`, so every integration test wrote into the user's own `stats.json`. On a real base that had produced 39,960 out of 42,200 junk records from temporary `/tmp` files, which distorted the format ranking and the export. Tests now use their own file.


## 2.2.0 — 2026-09-30

### Task dots in the web UI
The dots in a file row are no longer "just progress":
- variants excluded by `caps` are marked one dot per variant (yellow) with a reason, instead of one entry per format — a format can have a suitable variant at an unsuitable bit depth, and a plain list of "excluded formats" could not convey that;
- the winner marker and its task index survive a daemon restart (`queue.json` stores the fmt/variant pair);
- a new `skipped` state — a grey dot for a task cancelled not by its own failure but by an abort caused by a neighbouring task, so the culprit (red `failed`) is distinguishable from the victims.

### Error contract
- a variant failure in strict mode no longer ends in a nameless `Aborted: N file(s) failed`: the file is closed *before* the abort, and its name and reason go to the output, `--report` and `stats.json`. Multi-line reasons from codec stderr are folded into one line;
- files that did not run to the end because of an abort are counted by a separate `Not processed` line instead of being drawn as empty rows;
- partially copied delivery files are removed on both the Windows and the POSIX path.

### `--formats`
`optimize --formats=<id>` with an unknown id used to filter out every format silently (`no suitable candidates`, code 0). It is now `ERROR: unknown format '<id>'` with code 1, the same way `variants` rejects an unknown `fmt_id`.

### `llao stats --report=<file>`
A new export writes the format ranking as a plain text table **without localization** — only format ids, sizes and percentages — so it can be handed to a codec author as is. On a real 3.6 GB library the ranking looks like this:

```
tak               86.78%      3432 candidates
monkeys_audio     84.63%      1895 candidates
optimfrog         83.88%      6080
la                83.27%      1484
wavpack           78.20%      9408
alac              77.20%       388
tta               73.19%       443
flac              45.09%      7203
```

The database path is overridable through `LLAO_STATS_FILE`.

### Localization
- `llao serve --help` moved to an English base with the translation in `lang/ru.json` (it used to be printed with a raw `printf` containing Cyrillic, so both the Russian and the English user saw Russian);
- the `usage()` keys in `lang/ru.json` were brought in line with the current text: 26 strings got a translation, 11 stale keys were dropped. A Russian user no longer sees English lines in the help.

### Documentation
The README is now bilingual: `README.md` is English (the primary one, it ships in the release) and `README.ru.md` is Russian; the config schema is split the same way (`formats/README.en.md` / `README.ru.md`). `AGENTS.md` gained a rule about keeping the pairs in sync. This file and its Russian counterpart were added at the same time, so the release text has a source in the repository.

### Encoder crashes
The work that was versioned 2.1.3 during development shipped in this release:
- the ffmpeg fallback path now canonicalizes the reference WAV, which removes an access violation (0xC0000005) in OptimFROG on the fallback path;
- an encoder crash stops the work on the whole file. Previously the remaining variants kept spinning on the same broken reference WAV, although the result obviously could not change — `stats.json` held 7287 such records for a single broken input.

### Miscellaneous
- Monkey's Audio updated to **13.26**;
- the build now rebuilds objects when a header changes (`-MMD/-MP`) — before this, stale objects with the old class layout were left behind, which showed up as a segfault or lost vtable entries;
- `WavReader` got an explicit `close()` that guards against a double close of the handle;
- the daemon reported a hardcoded `ignore_errors: true` in `/api/state`, so the UI claimed tolerant mode while the daemon ran strictly.

## 2.1.2 — 2026-09-24

- WAV canonicalization after a native decode. Decoding `.ape` through `MAC.exe` produced a reference WAV with a non-canonical header (a `fact` chunk, or `bext`/`minf`/`elm1` before `fmt`): OptimFROG crashed with 0xC0000005 and produced no file, LA reported `invalid .wav header`. `media::canonicalize_wav()` reduces the header to a canonical 44-byte `fmt`(16 PCM)+`data`, with the PCM copied byte-for-byte. The ffmpeg fallback is not normalized (its format suits both codecs).

## 2.1.1 — 2026-09-11

- a favicon with a gradient in the web UI, the version in the UI header, and the stable `llao-latest-win64.zip` asset.

## 2.1.0 — 2026-09-10

- The queue no longer depends on the daemon's working directory: a row stores an absolute `root` plus a path relative to it, so a restart from any directory recognizes its sources. Before this, a restart from a different directory could report every row as `source file not found`.
- `replace_file()` instead of `std::rename` when writing results and the discovery file, which on Windows did not overwrite the target and could leave dangling `.tmp` files or second records.

## 2.0.0 — 2026-09-08

### The terminal interface is gone
The status bar and the separate `llao-daemon` process were removed. The engine runs headless and the interface is a browser: the `serve` subcommand starts an HTTP daemon with a web UI. The CLI (`optimize`, `restore`, `tools`, `variants`, `stats`) and the server live in **one binary** — `llao.exe` / `llao-linux`.

### Daemon and web UI
- HTTP API and a built-in web UI (no build step, vanilla ES6, embedded into the executable): login by token, the queue table with per-variant task dots, counters, add by path, cancel-file/cancel-all, reorder, remove, pause/resume, restart, shutdown with confirmation;
- a persistent queue in `queue.json` (v2) next to the discovery file, updated on every change and on shutdown, so `ok` rows survive a restart, active rows continue and stopped/error rows keep their reason;
- instant process kill on remove/shutdown, a prep watchdog with a time budget, tombstones instead of ghost rows, atomic restart, and id-based (not position-based) removal;
- crash safety: unfinished rows are reported as such on the next start, and the result write is transactional through a temporary file, so a crash between the steps never shows a false "done".

### Architecture
- tags and format descriptions moved into `formats/*.json` — no hardcoded format names or tag key tables in C++; a new format is added by JSON alone;
- the monoliths were decomposed: `serve.cpp` into `serve_entry`/`serve_session`/`serve_queue`/`serve_persist`, `optimize.cpp` (3128 lines) into `optimize_util`/`optimize_codec`/`optimize_runner`, `tags.cpp` (1845 lines) into per-format modules; see `docs/architecture-audit.md`;
- the engine was decoupled from the UI through `obs::Sink` and turned into a long-running queue with a public `Engine` API.

### Infrastructure
- CI builds the single binary natively on Linux and runs the whole test suite, with `bin/` cached by `formats/*.json`;
- a dev container (llvm-mingw + wine32/64 + ffmpeg + p7zip) and the `llao` wine wrapper with a real single-instance mutex;
- `stats.json` records candidate performance metrics: `wall_ms`, `prep_wall_ms`, `decode_wall_ms`.

## 1.10.1 — 2026-08-23

- `files` lists in the format configs: the codec cache and the release archive now copy only the listed files, which removed ~8 MB from the release and ~610 MB from a local `bin/`;
- local `bin/` cleaned of unused GUI tools, installers and documentation.

## 1.10.0 — 2026-08-23

- a pseudographic interface for `restore`, identical to the optimizer (two phases, navigation, auto-follow, scrollbar), plus the `--no-status` flag;
- an alias chain for native decoders: symlink → hardlink → original → temporary copy, so a decoder can always be pointed at a path it accepts;
- a unified footer and scrollbar color palette;
- ffprobe JSON artifacts stripped from probe output.

## 1.9.0 — 2026-08-22

- native Linux build: `make TARGET=linux` produces `llao-linux` with g++, no cross-compiler needed; the Windows codec utilities run under wine;
- `DiskBudget`: a centralized disk budget with file-level and variant-level budgeting, deferred files and real free-space checks, so a run stops cleanly instead of filling the disk;
- `--tmp=<path>` for a custom temporary directory (RAMFS-compatible);
- the status bar is suppressed on a pipe, and `decoder_path` finds `.exe` decoders on Linux.

## 1.8.1 — 2026-08-22

- terminal size detection under wine via `stty`/`tput`, and `init()` no longer disables the interactive UI when `GetConsoleMode` fails — previously `wine llao.exe` without `LLAO_STATUS_FORCE=1` turned the whole UI off;
- external process output normalized, and the error text is no longer truncated.

## 1.8.0 — 2026-08-20

- stall detection: a codec that stops making progress is killed, not just a slow one (polling of CPU time and output size);
- a proportional hard timeout (based on the WAV size) instead of a fixed 1800 s;
- the scheduler footprint model corrected, with a separate per-file base cost;
- distinct footer and scrollbar colors in the UI.

## 1.7.0 — 2026-08-19

- tags fully data-driven: `key_map`, `write_constraints` and `native_reader` live in `formats/*.json`, the hardcoded key tables are gone;
- scheduler: a window per file (a large file with slow variants no longer blocks the rest), a tmp budget of free space minus a reserve, and honest errors — a candidate that is not smaller than the original is an honest SKIP;
- immediate abort on an error without `--ignore-errors` (active encoders and decoders are interrupted);
- status bar: a vertical scrollbar, correct handling of wide CJK glyphs, an hscroll mask, and a diagnostics buffer shown in the footer;
- per-process tmp isolation (`tmp/<pid>`) with ASCII names for ANSI encoders, and lower thread priority for the codecs.

## 1.6.0 — 2026-08-17

- status bar v2: a progress bar and a mosaic mode (Tab), a state palette, scrolling paths, right-aligned percentages and processor time instead of wall time. *(The status bar itself was removed in 2.0.)*

## 1.5.0 — 2026-08-17

- the interface moved into the console alternate screen buffer, so the original screen with the command line is restored on exit;
- readable replacement errors: ANSI messages are recoded to UTF-8 instead of garbled bytes, and renames retry against a transient antivirus lock;
- strict scheduler priority: an idle worker takes the variant of the earliest ready file.

## 1.4.0 — 2026-08-17

- codecs are spawned so that they cannot inherit our open file handles. Before this they inherited a transient descriptor and parallel file replacement failed with `ERROR_SHARING_VIOLATION`; constraining the inheritance through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` was rejected as unstable on real Windows, so files are now opened non-inheritable with `FILE_SHARE_DELETE`;
- Ctrl+C handling: the first press is a graceful stop, the second a forced exit, and the encoders die with the process (a Job Object with `KILL_ON_JOB_CLOSE`);
- the prep window no longer follows the number of files not yet started, cancellation is checked in the workers, and a failed replacement counts as a file error.

## 1.3.0 — 2026-08-16

- the status bar works in the alternate screen buffer, overflow resets the scroll region, and log lines are clipped to the window width with the tail kept;
- CI moved to `checkout@v5` and `action-gh-release@v3`.

## 1.2.0 — 2026-08-16

- safe in-place replacement: the original is moved to `.llao-bak.<ext>`, the candidate is renamed into place, and a failure rolls back;
- `--verify=all|winner|none` and `--ignore-errors`, with an immediate abort on a file error;
- parallel prep of up to `min(jobs, remaining files)` — before this the CPU idled on large libraries;
- streaming WAV comparison instead of loading the file into memory;
- the `llao` wine wrapper ships in the release archive, so `llao.exe` can be run on Linux without wine noise.

## 1.1.2 — 2026-08-16

- status bar: spaces were dropped while drawing, so the text ran together and the percentage stuck to the file name;
- `restore`: renaming the temporary file retries, so a transient lock no longer leaves a `.llao-restore-tmp` behind.

## 1.1.1 — 2026-08-15

- honest errors and a native probe fallback: valid `.ofr` files (ffmpeg cannot parse OptimFROG at all) were marked SKIP with `errors: 0`, while real failures (a broken file, a missing utility) were disguised as success with exit 0;
- the error test counts errors independently of the output language.

## 1.1.0 — 2026-08-15

- `--jobs=N|M.F` — a core multiplier (default 2.0) — and a scheduler window that adapts to free tmp space and RAM;
- the status bar block at the bottom of the screen.

## 1.0.0 — 2026-08-15

- first tagged release: parallel variant processing, a global scheduler window and a tmp limit by size;
- localization (i18n catalogs, the interface language follows `--lang`, `LLAO_LANG`, `llao.json` and the system language);
- a release CI that builds `llao.exe`, installs the codecs and publishes the archive, and blocks the release when a codec's CLI output changes or a pinned version is stale;
- Monkey's Audio 13.25.
