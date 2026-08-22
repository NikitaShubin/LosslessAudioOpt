# Makefile: сборка llao-linux (native) или llao.exe (Windows cross-compile).
#
# Linux:   make TARGET=linux
# Windows: make TARGET=windows  (по умолчанию)

TARGET ?= windows

SRCS := src/main.cpp \
        src/util.cpp \
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
        src/optimize.cpp \
        third_party/miniz/miniz.c

ifeq ($(TARGET),linux)
  CXX := g++
  CC := gcc
  CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Ithird_party -Isrc
  CFLAGS := -O2 -Ithird_party
  LDFLAGS := -lpthread
  TARGET_BIN := llao-linux
else
  CXX := x86_64-w64-mingw32-g++
  CC := x86_64-w64-mingw32-gcc
  CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Ithird_party -Isrc
  CFLAGS := -O2 -Ithird_party
  LDFLAGS := -static -static-libgcc -static-libstdc++ -lwinhttp
  TARGET_BIN := llao.exe
endif

OBJS := $(SRCS:.cpp=.o)
OBJS := $(OBJS:.c=.o)

all: $(TARGET_BIN)

$(TARGET_BIN): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET_BIN) llao.exe llao-linux

.PHONY: all clean
