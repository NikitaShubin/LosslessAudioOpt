# formats/ — lossless formats configuration

Every `*.json` file describes one audio format for the LLAO optimizer. The encoders
themselves are NOT bundled: the config contains everything needed for the executable
to find, download by URL and prepare the required utility itself.

Besides the codecs, the directory holds two service JSON files (not formats, they do
not take part in `check-formats`):
- `inputs.json` — input formats (e.g. `wav`/`aiff`/`ogg`) with a `lossless` flag,
  which are not target codecs;
- `tag_tables.json` — tag key/4CC tables (`id3`, `mp4`, `wav`) used by the parsers
  and by `canonical_key` (hardcoding keys in C++ is forbidden); the
  `canonical_aliases` section holds synonyms inside the canonical schema
  (normalized name → canonical key).

## Adding a new format

1. Create `formats/<id>.json` following the schema below (use `flac.json` as a
   reference).
2. Put working links to the console utilities into `downloads` (Windows builds only;
   for ffmpeg codecs you can reference the shared BtbN build).
3. Describe `caps` (channels/bit depth/sample rate) and `tag.capabilities` exactly,
   otherwise the format will either run into encoder errors or silently lose tags —
   that is unacceptable.
4. Specify `variants` — all the compression parameters that have to be brute-forced.
5. Run `llao.exe check-formats` to validate the schema.

## Config schema

| Field | Type | Required | Description |
|---|---|---|---|
| `id` | string | yes | The format key (the file name without the extension). |
| `name` | string | yes | Human-readable name. |
| `extension` | string | yes | Result extension without the dot. |
| `homepage` | string | no | Link to the website. |
| `enabled` | bool | no | `true` by default. |
| `engine.kind` | string | yes | `binary` (external utility) or `ffmpeg` (built-in codec). |
| `engine.executable` | string | for `binary` | The encoder binary name. |
| `engine.decoder_executable` | string | no | Binary for decoding to WAV (defaults to `executable`). |
| `engine.codec` | string | for `ffmpeg` | The ffmpeg codec (e.g. `alac`, `tta`, `mp4als`). |
| `engine.container` | string | for `ffmpeg` | The container (e.g. `m4a`, `tta`). |
| `downloads[]` | array | yes | Ways to obtain the utility (per OS). |
| `downloads[].os` | string | yes | `windows` \| `any` (the codecs are only available as Windows builds). |
| `downloads[].url` | string | yes | Direct link to the archive or the installer. |
| `downloads[].kind` | string | yes | `archive` \| `extract7z`. |
| `downloads[].file_glob` | string | for `archive` | Where the binary is in the archive (glob). |
| `downloads[].files` | array | for `extract7z` | File names to extract from the installer (e.g. `["la.exe", "la-core.dll"]`). |
| `downloads[].checksum` | object | no | `{"type": "sha256", "value": "..."}`. |
| `downloads[].notes` | string | no | Explanations. |
| `cli_check` | object | no | `{"cmd": [...], "expect": [...]}` — comparison of the config with the utility's real CLI (see below). |
| `encode.cmd` | array | yes | The encode command template. Placeholders: `{input}`, `{output}`, `{params}`, `{codec}`. |
| `encode.variants` | array | yes | All parameter combinations: `{"id", "args", "note"}`. Empty = a single run. |
| `decode.cmd` | array | yes | The decode-to-WAV command template: `{input}`, `{output}`. |
| `verify.kind` | string | no | `builtin` (a fast integrity check) or `none`. |
| `verify.cmd` | array | no | The check command, placeholder `{input}`. |
| `tag.system` | string | yes | `vorbis` \| `apev2` \| `id3` \| `id3v1` \| `mp4`. |
| `tag.writer` | string | yes | Tag writer key: `flac` \| `wavpack` \| `monkeysaudio` \| `optimfrog` \| `id3` \| `mp4` \| `tak`. |
| `tag.capabilities` | object | yes | `text`, `pictures`, `lyrics`, `cue_sheet`, `replay_gain`, `chapters` (bool). |
| `caps.channels` | object | yes | `{"min": N, "max": M}`. |
| `caps.bit_depth` | array | yes | The list of supported bit depths (int). |
| `caps.sample_rate` | object | no | `{"min": N, "max": M}`. |
| `notes` | string | no | Explanations. |

Rules:
- Commands are arrays of strings, **without a shell** (`subprocess` without
  `shell=True`).
- `{params}` expands to the **list of arguments** of the variant (`variants[].args`);
  if the list is empty, not a single argument appears in place of the placeholder.
  `{codec}` comes from `engine.codec`.
- The first element of `cmd` is the utility name; at launch it is replaced with the
  real path (from `bin/<id>/`, PATH or the download result).
- If a format cannot embed some class of tags — state `false` honestly in
  `tag.capabilities`: the tags that do not fit go into a ZIP sidecar, no data is
  lost.
- `caps` should be conservative: it is better to exclude the format for a file than
  to get a "successful" encoding with a changed bit depth/channel count.

## CLI for verification

- `llao.exe check-formats` — schema validation of all `formats/*.json`.
- `llao.exe tools [fmt_id ...] [--no-download]` — status/download of the utilities
  (cache in `bin/<id>/`, the path to the binary is stored in the `.binary` marker).
- `llao.exe help <fmt_id> -- --help` — run the utility with `--help` and show the
  output (the arguments after `--` are passed to the utility unchanged).

Downloading: from the URL in `downloads[]`, SHA-256 verification (if specified), zip
unpacking, the result is copied into `bin/<id>/` (for `engine.kind: "ffmpeg"` — into
the shared `bin/ffmpeg/`, so that ffmpeg.exe is not downloaded separately for every
format).

`downloads[].kind` specifics:

- `archive` — zip unpacking, binary search by `file_glob` (case-insensitive,
  `win64`/`x64`/`amd64` preferred), neighbouring files (DLLs) are copied along.
- `extract7z` — the downloaded installer (e.g. an NSIS self-extracting one) is
  unpacked with a full 7-Zip (`bin/7z/7z.exe` → next to the exe → PATH; the
  standalone `7za.exe` does not understand NSIS), the files from `files[]` are taken
  from the result by a recursive search and copied into `bin/<id>/`. **The utility is
  never installed into the system** (no Program Files, no registry, no
  winamp/foobar plugins). The downloaded installer is cached in `bin/<id>/` and is
  not re-downloaded on the next run.

## cli_check — config staleness control

While preparing the utility, `cli_check.cmd` is called (`["--help"]` by default) and
the output (lower-cased) is checked for the presence of all the substrings from
`cli_check.expect`. On a mismatch a "the config may be stale" warning is produced
(the work is not blocked). Examples: FLAC —
`["flac - command-line flac encoder/decoder", "version 1.5.0"]`, ffmpeg —
`["ffmpeg version"]`.

Besides the pinpoint substring check, the release CI compares the full `cli_check`
output with the baseline `.github/cli-baselines/<id>.txt`
(`python3 .github/scripts/check_cli_baselines.py --compare`) — any change in a
codec's help blocks the release until the config and the baseline are updated (see
AGENTS.md).

## Documentation

- [README.ru.md](README.ru.md) — this document in Russian.
