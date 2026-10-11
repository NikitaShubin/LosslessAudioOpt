// Перечисление файлов: в очередь попадает только известное аудиорасширение.
//
// Проверка живёт отдельной целью, а не внутри test-unit, потому что
// is_supported_file живёт в optimize_util.cpp, который тянет за собой теги и
// miniz — ради одного предиката это неподъёмно тяжёлая сборка.
//
// Здесь же закреплено решение, принятое заказчиком: имена, «похожие на
// мусор», ничего не отсекают. Файлы `._имя` и содержимое каталога `__MACOSX`
// приходят с macOS, но решать, что файл ненастоящий, не задача llao. Отсев
// по имени молча выбрасывал бы из библиотеки всё, что так названо.

#include <cstdio>
#include <set>
#include <string>

#include "optimize_internal.h"

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_passed;                                                       \
        } else {                                                              \
            ++g_failed;                                                       \
            fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);\
        }                                                                     \
    } while (0)

int main() {
    const std::set<std::string> exts = {"wav", "mp3", "ape", "flac"};

    // Известные расширения — в очередь.
    const char* good[] = {
        "/lib/album/track.wav",
        "/lib/album/song.ape",
        "/lib/album/_ok.mp3",
        "/lib/album/.hidden.wav",
        "/lib/album/Track.WAV",          // регистр расширения не важен
        "Z:\\lib\\album\\track.flac",
        "/lib/album/song.flac.wav",      // берётся последнее расширение
    };
    for (const char* p : good) CHECK(optimize::is_supported_file(p, exts));

    // Неизвестные расширения и файлы без расширения — не в очередь.
    const char* bad[] = {
        "/lib/album/cover.jpg",
        "/lib/album/notes.txt",
        "/lib/album/README",
    };
    for (const char* p : bad) CHECK(!optimize::is_supported_file(p, exts));

    // Регрессия: `._` внутри имени не должен выкидывать файл. Артефакты macOS
    // приходят с расширением аудио, и отсев по имени молча убирал их из
    // библиотеки, чтобы починить счётчик ошибок.
    const char* macos[] = {
        "/lib/album/._track.wav",
        "/lib/__MACOSX/album/._track.wav",
        "/lib/__MACOSX/album/._01 - Track.flac",
    };
    for (const char* p : macos) CHECK(optimize::is_supported_file(p, exts));

    if (g_failed == 0) {
        fprintf(stderr, "ALL PASSED (%d assertions)\n", g_passed);
        return 0;
    }
    fprintf(stderr, "%d/%d FAILED\n", g_failed, g_passed + g_failed);
    return 1;
}