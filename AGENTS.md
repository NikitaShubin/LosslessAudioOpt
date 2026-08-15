# AGENTS.md

## Общение
- Всегда отвечай пользователю **по-русски**.
- Весь текст, комментарии и документация для пользователя — на русском языке.

## Релизный процесс и CI

Релиз создаётся тегом вида `v1.0.0` (или вручную через workflow_dispatch):
`.github/workflows/release.yml` собирает `llao.exe` (llvm-mingw), устанавливает
кодеки (`wine llao.exe tools`) и публикует архив `llao-v<версия>-win64.zip`
с предустановленными кодеками.

Сборка **блокируется** (exit != 0) при любом из условий:

1. **CLI кодеков изменился** — `.github/scripts/check_cli_baselines.py --compare`
   сверяет вывод `cli_check` (обычно `--help`) с эталонами
   `.github/cli-baselines/<id>.txt`. Скрипт показывает unified-diff и новые
   option-строки.
2. **Вышла новая версия кодека** — `.github/scripts/check_upstream.py --check`
   сверяет версии из `formats/*.json` с актуальными (flac/wavpack — GitHub API,
   tak/optimfrog/monkeys_audio — ручная таблица `UPSTREAM_MANUAL` в скрипте).
3. **Упали тесты тегов** — `python3 tests/test_tags.py`.

### Что делать при красной сборке (порядок разбора)

- **CLI-эталоны:** разобрать изменение — новая версия кодека, изменилась справка,
  сломалась команда. Обновить `formats/*.json` (варианты, `cli_check.expect`),
  затем перегенерировать эталоны:
  `python3 .github/scripts/check_cli_baselines.py --update`, проверить `git diff`
  эталонов и закоммитить вместе с конфигами. Никогда не перезаписывать эталоны
  «вслепую» без объяснения, что изменилось.
- **Upstream-проверка:** обновить `formats/*.json` (url, checksum, notes,
  `cli_check.expect`), для ручных кодеков — `UPSTREAM_MANUAL` в скрипте. Если
  новая версия сознательно не берётся — записать причину в
  `.github/cli-baselines/skip-upstream.json`:
  `{"<id>": "<причина, дата>"}`.
- **Тесты:** чинить код/конфиги; тесты требуют wine, `ffmpeg` в PATH,
  собранный `llao.exe` и наполненный `bin/`.

### Как обновить кодеки вручную

- Скачать дистрибутив, посчитать `sha256sum`, заменить в `formats/<id>.json`
  url + checksum + notes + при необходимости варианты и `cli_check.expect`.
- `monkeysaudio.com/x64` — нестабильный редирект (иногда 406); надёжный прямой
  URL: `https://www.monkeysaudio.com/files/MAC_<версия>_x64.exe`.
- Актуальные версии для релизных заметок: `python3 .github/scripts/check_upstream.py --report`.
