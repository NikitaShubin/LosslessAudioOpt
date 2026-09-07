# Архитектурный аудит и декомпозиция

## Цель

Провести ревизию архитектуры движка по правилам `AGENTS.md`, убрать остатки
хардкода форматов/ключей/расширений в C++ и разбить три монолитных файла
(`serve.cpp`, `optimize.cpp`, `tags.cpp`) на модули.

Итог: всё описание форматов и тегов — в `formats/*.json`, C++ не содержит
форматно-зависимых таблиц и стабильно компилируется с включёнными
`-Wall -Wextra`.

## 1. Хардкод — устранено

### 1a. Список lossless-кодеков

- Было: `media.cpp` проверял `fmt_id == "flac"`, `=="wavpack"` и т.д. для
  определения «является ли формат lossless» (нужно для лимита декод-папки).
- Стало: в каждый `formats/*.json` добавлено поле `"lossless": true|false`, а
  конфиг раскладывает его в `config::Format::lossless`.
  `media::lossless_codec_set()` строит множество из загруженных форматов —
  хардкод имён исчез.
- Для binary-кодеков добавлено поле `"ffprobe_codec"`: ffprobe отдаёт
  `codec_name`, который может отличаться от `id` формата
  (monkeys_audio→`ape`, mpeg4_als→`als`, optimfrog→`optimfrog`, alac→`alac`).

### 1b. Расширения входных файлов

- Было: в `media.cpp` хардкод-список `wav/aiff/ogg` считался lossless-входами.
- Стало: `formats/inputs.json` описывает входные форматы (kind lossless и т.д.),
  `config::load_inputs()` читает его, `supported_extensions()` строится из
  данных — без имён форматов в C++.

### 1c. Ключи тегов

- Было: таблицы `VORBIS_KEYS`, `APE_KEYS` и 4CC-словари хардкодились в C++.
- Стало: `formats/tag_tables.json` — секции `id3`, `mp4`, `wav`; `config`
  загружает их в `TagTables` с кешем (`load_tag_tables()`), а `canonical_key`
  и парсеры (ID3 frame id, MP4 4CC, WAV LIST-INFO) берут ключи из конфига.

## 2. Декомпозиция serve.cpp

Монолит `serve.cpp` (~1000+ строк) разбит на 4 модуля + общий header:

| Файл | Содержимое |
|---|---|
| `serve_internal.h` | общие типы и объявления (переход к внешней связности) |
| `serve_entry.cpp` | `run_daemon` и точка входа демона |
| `serve_session.cpp` | сессия/опции демона |
| `serve_queue.cpp` | очередь и её операции |
| `serve_persist.cpp` | персистентность очереди и crash-безопасная доставка |

`src/serve.cpp` удалён. Объекты подключаются через `SERVER_SRCS` в Makefile.

## 3. Декомпозиция optimize.cpp

Монолит `optimize.cpp` (3128 строк) разбит на 4 модуля + header:

| Файл | Строк | Содержимое |
|---|---|---|
| `optimize_internal.h` | 325 | общие типы и объявления |
| `optimize_util.cpp` | 174 | пути, tmp, resolve_jobs, оценки размеров |
| `optimize_codec.cpp` | 381 | FileSession, DiskBudget, ResourceManager, build_cmd, кодеки |
| `optimize_runner.cpp` | 1275 | Runner + FileJob и воркер-цикл |
| `optimize.cpp` | 802 | Engine + run()/restore + list_variants |

Механика перехода:
- все типы/функции анонимного namespace монолита переведены во внешнюю
  связность через `optimize_internal.h` (named `namespace optimize`);
- `base_tmp_dir` вынесен в header (используется `Engine::Impl`);
- `Runner` объявлен в header, методы определены out-of-line в
  `optimize_runner.cpp`;
- DiskBudget/ResourceManager размещены в `optimize_codec.cpp`.

## 4. Декомпозиция tags.cpp

Монолит `tags.cpp` (1845 строк) разбит на 8 модулей + header:

| Файл | Содержимое |
|---|---|
| `tags_internal.h` | бинарные хелперы и объявления кросс-модульных функций |
| `tags_core.cpp` | канонизация, TagSet (extract/merge/rebuild), plan_tags, validate |
| `tags_vorbis.cpp` | FLAC метаданные, Vorbis comment, FLAC PICTURE, OGG |
| `tags_apev2.cpp` | APEv2 (build/parse) |
| `tags_id3.cpp` | ID3v2 и ID3v1 (build/parse), UTF-16 хелперы |
| `tags_mp4.cpp` | m4a_children, mp4 ilst parse |
| `tags_wav.cpp` | WAV LIST INFO + встроенные ID3v2-блоки |
| `tags_sidecar.cpp` | write/read_sidecar (ZIP) |
| `tags_write.cpp` | write_group (запись встроенных тегов по write_method) |

`src/tags.cpp` удалён. Объекты подключены через `COMMON_SRCS` в Makefile.

## 5. Проверки

- `make TARGET=linux llao-linux` — чистая сборка с `-Wall -Wextra`, 0 ошибок и
  0 предупреждений (проверено после декомпозиции tags и optimize).
- Финальная валидация: полная сборка + полный набор тестов
  (`test-unit`, `test-daemon-core`, `test-daemon`).