# Makefile: сборка единого исполняемого файла llao (движок + сервер + CLI).
#
# Linux:   make TARGET=linux   -> llao-linux
# Windows: make TARGET=windows -> llao.exe (по умолчанию)
#
# Объектные файлы разделяются по таргетам (build/<target>/..): чередование
# TARGET не требует make clean и исключает смешивание libcpp/libstdc++.

TARGET ?= windows

# --- Общие объекты (движок) ---
COMMON_SRCS := src/util.cpp \
        src/sha256.cpp \
        src/config.cpp \
        src/i18n.cpp \
        src/out.cpp \
        src/proc.cpp \
        src/download.cpp \
        src/archive.cpp \
        src/tool.cpp \
        src/stats.cpp \
        src/report.cpp \
        src/media.cpp \
        src/tags_core.cpp \
        src/tags_vorbis.cpp \
        src/tags_apev2.cpp \
        src/tags_id3.cpp \
        src/tags_mp4.cpp \
        src/tags_wav.cpp \
        src/tags_sidecar.cpp \
        src/tags_write.cpp \
        src/linear_sink.cpp \
        src/obs.cpp \
        src/optimize.cpp \
        src/optimize_util.cpp \
        src/optimize_codec.cpp \
        src/optimize_runner.cpp \
        third_party/miniz/miniz.c

# --- Событийный слой сервера (HTTP-API, RPC, очередь, персистентность) ---
SERVER_SRCS := src/events.cpp \
        src/daemon_sink.cpp \
        src/rpc.cpp \
        src/persist.cpp \
        src/http_api.cpp \
        src/serve_session.cpp \
        src/serve_queue.cpp \
        src/serve_persist.cpp \
        src/serve_entry.cpp

# Веб-ассеты вшиваются в единый бинарник.
WEB_ASSETS_SRCS := src/web_assets.cpp src/web_assets_data.cpp

ifeq ($(TARGET),linux)
  CXX := g++
  CC := gcc
  CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Ithird_party -Ithird_party/httplib -Isrc
  CFLAGS := -O2 -Ithird_party
  LDFLAGS := -lpthread
  BIN := llao-linux
else
  CXX := x86_64-w64-mingw32-g++
  CC := x86_64-w64-mingw32-gcc
  CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -Ithird_party -Ithird_party/httplib -Isrc
  CFLAGS := -O2 -DWIN32_LEAN_AND_MEAN -Ithird_party
  LDFLAGS := -static -static-libgcc -static-libstdc++ -lwinhttp -lws2_32 -lbcrypt -lshell32
  BIN := llao.exe
endif

OBJDIR := build/$(TARGET)
OBJ_FROM_SRC = $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(filter %.cpp,$1)) \
               $(patsubst third_party/%.c,$(OBJDIR)/%.o,$(filter %.c,$1))
COMMON_OBJS := $(call OBJ_FROM_SRC,$(COMMON_SRCS))
SERVER_OBJS := $(call OBJ_FROM_SRC,$(SERVER_SRCS))
WEB_ASSETS_OBJS := $(call OBJ_FROM_SRC,$(WEB_ASSETS_SRCS))
# Единый бинарник: движок + событийный слой (serve_session/queue/persist/entry —
# декомпозиция serve.cpp) + main.cpp (диспетчер CLI/серверных подкоманд) +
# веб-ассеты.
MAIN_OBJS := $(COMMON_OBJS) $(SERVER_OBJS) $(OBJDIR)/main.o $(WEB_ASSETS_OBJS)

all: $(BIN)

$(BIN): $(MAIN_OBJS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJDIR)/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf build
	rm -f llao.exe llao-linux
	rm -f test-unit test-daemon-core

test-unit: tests/test_resource_manager.cpp
	g++ -std=c++17 -O2 -Wall -Wextra -o $@ $<

test-daemon-core: tests/test_daemon_core.cpp src/events.cpp src/daemon_sink.cpp src/rpc.cpp src/util.cpp
	g++ -std=c++17 -O2 -Wall -Wextra -Ithird_party -Isrc -o $@ $^

# Живые интеграционные тесты сервера (нужен собранный llao-linux и ffmpeg;
# каждый поднимает свой сервер на случайном порту, 18180 не трогает).
# Тесты рассчитаны на нативный linux-бинарник: собирайте через `make TARGET=linux
# llao-linux` (при TARGET=windows таргет просто пропускается, если бинарник
# не собран — скрипт печатает SKIP).
ifeq ($(TARGET),linux)
DAEMON_TEST_BIN := llao-linux
else
DAEMON_TEST_BIN :=
endif

test-daemon: test-daemon-restore test-daemon-interactions test-daemon-queue test-daemon-persist

test-daemon-queue: $(DAEMON_TEST_BIN)
	python3 tests/test_daemon_queue.py

# Комплексный тест комбинаций взаимодействия с очередью.
test-daemon-interactions: $(DAEMON_TEST_BIN)
	python3 tests/test_daemon_interactions.py

# Интеграционный тест режима восстановления (restore) и целевой папки.
test-daemon-restore: $(DAEMON_TEST_BIN)
	python3 tests/test_daemon_restore.py

# Персистентность очереди (queue.json), crash-перезапуск и транзакционный sidecar.
test-daemon-persist: $(DAEMON_TEST_BIN)
	python3 tests/test_daemon_persist.py

# Генерация встроенных веб-ассетов (zip → C++ массив).
src/web_assets_data.cpp: web/index.html web/app.js web/style.css tools/embed_assets.py
	python3 tools/embed_assets.py

.PHONY: all clean test-unit test-daemon-core test-daemon test-daemon-queue test-daemon-interactions test-daemon-restore test-daemon-persist
