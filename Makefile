# Makefile: сборка llao (монолит) и llao-daemon (headless-демон с HTTP-API).
#
# Linux:   make TARGET=linux   -> llao-linux, llao-daemon-linux
# Windows: make TARGET=windows -> llao.exe,  llao-daemon.exe (по умолчанию)
#
# Объектные файлы разделяются по таргетам (build/<target>/..): чередование
# TARGET не требует make clean и исключает смешивание libcpp/libstdc++.

TARGET ?= windows

# Движок + инфраструктура демона (общие для монолита и демона).
# main.cpp (монолит) и serve.cpp (демон) линкуются отдельно.
ENGINE_SRCS := src/util.cpp \
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
        src/tags.cpp \
        src/status.cpp \
        src/screen.cpp \
        src/obs.cpp \
        src/status_sink.cpp \
        src/optimize.cpp \
        src/events.cpp \
        src/daemon_sink.cpp \
        src/rpc.cpp \
        src/http_api.cpp \
        third_party/miniz/miniz.c

ifeq ($(TARGET),linux)
  CXX := g++
  CC := gcc
  CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Ithird_party -Ithird_party/httplib -Isrc
  CFLAGS := -O2 -Ithird_party
  LDFLAGS := -lpthread
  MONO_BIN := llao-linux
  DAEMON_BIN := llao-daemon-linux
else
  CXX := x86_64-w64-mingw32-g++
  CC := x86_64-w64-mingw32-gcc
  CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -Ithird_party -Ithird_party/httplib -Isrc
  CFLAGS := -O2 -DWIN32_LEAN_AND_MEAN -Ithird_party
  LDFLAGS := -static -static-libgcc -static-libstdc++ -lwinhttp -lws2_32 -lbcrypt -lshell32
  MONO_BIN := llao.exe
  DAEMON_BIN := llao-daemon.exe
endif

OBJDIR := build/$(TARGET)
OBJS := $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(filter %.cpp,$(ENGINE_SRCS)))
OBJS := $(patsubst third_party/%.c,$(OBJDIR)/%.o,$(filter %.c,$(ENGINE_SRCS))) $(OBJS)
# Веб-ассеты вшиваются только в демона.
WEB_ASSETS_SRCS := src/web_assets.cpp src/web_assets_data.cpp
WEB_ASSETS_OBJS := $(patsubst src/%.cpp,$(OBJDIR)/%.o,$(WEB_ASSETS_SRCS))
MONO_OBJS := $(OBJS) $(OBJDIR)/main.o $(WEB_ASSETS_OBJS)
DAEMON_OBJS := $(OBJS) $(OBJDIR)/serve.o $(WEB_ASSETS_OBJS)

all: $(MONO_BIN) $(DAEMON_BIN)

$(MONO_BIN): $(MONO_OBJS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(DAEMON_BIN): $(DAEMON_OBJS)
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
	rm -f llao.exe llao-linux llao-daemon.exe llao-daemon-linux
	rm -f test-unit test-daemon-core

test-unit: tests/test_resource_manager.cpp
	g++ -std=c++17 -O2 -Wall -Wextra -o $@ $<

test-daemon-core: tests/test_daemon_core.cpp src/events.cpp src/daemon_sink.cpp src/rpc.cpp src/util.cpp
	g++ -std=c++17 -O2 -Wall -Wextra -Ithird_party -Isrc -o $@ $^

# Живой интеграционный тест очереди (нужны собранный llao-daemon-linux
# и ffmpeg; поднимает свой демон на случайном порту, 18180 не трогает).
test-daemon-queue: llao-daemon-linux
	python3 tests/test_daemon_queue.py

# Комплексный тест комбинаций взаимодействия с очередью.
test-daemon-interactions: llao-daemon-linux
	python3 tests/test_daemon_interactions.py

# Генерация встроенных веб-ассетов (zip → C++ массив).
src/web_assets_data.cpp: web/index.html web/app.js web/style.css tools/embed_assets.py
	python3 tools/embed_assets.py

.PHONY: all clean test-unit test-daemon-core test-daemon-queue test-daemon-interactions
