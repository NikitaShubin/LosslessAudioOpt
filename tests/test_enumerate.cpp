// Перечисление файлов: мусор macOS не должен попадать в очередь.
//
// Проверка живёт отдельной целью, а не внутри test-unit, потому что
// is_supported_file живёт в optimize_util.cpp, который тянет за собой теги и
// miniz — ради одного предиката это неподъёмно тяжёлая сборка.

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

    // `._имя` — ресурс-форк, независимо от расширения и от того, лежит ли он
    // в `__MACOSX`.
    const char* junk[] = {
        "/lib/__MACOSX/album/._track.wav",
        "/lib/__MACOSX/._track.ape",
        "/lib/album/._hidden.flac",
        "/lib/album/._leading.flac.wav",  // ресурс-форк самого .flac
        "Z:\\lib\\__MACOSX\\album\\._track.mp3",
        "/lib/album/._x",   // без расширения — но мусор по-прежнему мусор
        "/lib/album/._a.WAV",  // регистр расширения роли не играет
        "/lib/album/._not_junk.txt",  // и не аудио по расширению
    };
    for (const char* p : junk) CHECK(!optimize::is_supported_file(p, exts));

    // Похожие имена мусором быть не должны. Отдельно важно: сам каталог
    // `__MACOSX` не является признаком мусора — настоящее аудио, случайно
    // оказавшееся там, лучше пережать, чем потерять.
    const char* good[] = {
        "/lib/album/track.wav",
        "/lib/album/song.ape",
        "/lib/__MACOSXbackup/track.wav",
        "/lib/album/_ok.mp3",
        "/lib/album/.hidden.wav",
        "/lib/__MACOSX/album/real.wav",
    };
    for (const char* p : good) CHECK(optimize::is_supported_file(p, exts));

    if (g_failed == 0) {
        fprintf(stderr, "ALL PASSED (%d assertions)\n", g_passed);
        return 0;
    }
    fprintf(stderr, "%d/%d FAILED\n", g_failed, g_passed + g_failed);
    return 1;
}