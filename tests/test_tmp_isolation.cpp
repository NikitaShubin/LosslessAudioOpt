// Изоляция временных каталогов между процессами.
//
// Регрессия: демон писал сессии прямо в `tmp/` и стирал его целиком при старте
// и остановке. Второй llao из того же каталога получал те же имена сессий и
// те же ref.wav — на живой библиотеке это выглядело как «tak не читает
// ref.wav», «wavpack: can't open file ref.wav» и «data chunk extends beyond
// the file» на обычных файлах. Разовый прогон был изолирован по PID, демон —
// нет.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

#include "optimize_internal.h"
#include "util.h"

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_passed;                                                        \
        } else {                                                                \
            ++g_failed;                                                        \
            fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                      \
    } while (0)

int main() {
    const std::string base = "/tmp/llao-test-tmp";
    const std::string mine = optimize::session_tmp_dir(base);
    const std::string pid = util::process_id();

    // Каталог процесса лежит внутри базы и назван по PID.
    CHECK(mine == base + "/" + pid);
    CHECK(optimize::base_tmp_dir(base) == base);

    // Сессия получает своё имя внутри каталога процесса, и имена не повторяются.
    const std::string s1 = optimize::session_cookie();
    const std::string s2 = optimize::session_cookie();
    CHECK(s1 != s2);
    CHECK(s1.rfind(pid + "-", 0) == 0);

    // Соседний процесс: его каталог не должен попасть под нашу очистку.
    const std::string neighbour = base + "/999999";  // чужой процесс
    util::mkdirs(mine);
    util::mkdirs(neighbour);
    CHECK(util::dir_exists(mine));
    CHECK(util::dir_exists(neighbour));

    optimize::clear_session_tmp_dir_impl(base);

    // Свой каталог убран, чужой — нет. Раньше очистка сносила весь tmp.
    CHECK(!util::dir_exists(mine));
    CHECK(util::dir_exists(neighbour));
    std::error_code ec;
    fs::remove_all(fs::u8path(neighbour), ec);
    fs::remove_all(fs::u8path(base), ec);

    if (g_failed == 0) {
        fprintf(stderr, "ALL PASSED (%d assertions)\n", g_passed);
        return 0;
    }
    fprintf(stderr, "%d/%d FAILED\n", g_failed, g_passed + g_failed);
    return 1;
}