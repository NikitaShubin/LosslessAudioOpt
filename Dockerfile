# Dev-окружение LLAO: сборка llao.exe (llvm-mingw), llao-linux и прогон тестов
# (включая проверку поведения llao.exe под wine на Linux).
#
# Основа: mstorsjo/llvm-mingw (Ubuntu 24.04 + llvm-mingw, уже содержит make, g++,
# python3 и кросс-тулчейн x86_64-w64-mingw32-*). Поверх ставим то же, что и CI:
# wine, ffmpeg, p7zip, curl.
#
# Использование:
#   docker build -t llao-dev .
#   docker run --rm -it -v "$PWD:/work" -w /work llao-dev sh
#   # внутри:
#   make TARGET=windows     # -> llao.exe
#   make TARGET=linux all   # -> llao-linux
#   make test-unit test-daemon-core
#   WINEARCH=win64 wineboot -u  # инициализация префикса (однократно)

FROM mstorsjo/llvm-mingw:20260826

ENV WINEDEBUG=-all

RUN dpkg --add-architecture i386 \
    && apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        wine \
        wine64 \
        wine32:i386 \
        ffmpeg \
        p7zip-full \
        curl \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work