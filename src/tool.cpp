#include "tool.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

#include "archive.h"
#include "download.h"
#include "i18n.h"
#include "out.h"
#include "proc.h"
#include "sha256.h"
#include "util.h"

namespace tool {

namespace {

// Защита скачивания/распаковки: bin/<id>/ общий (в т.ч. ffmpeg-кэш для alac/tta/mpeg4_als),
// при параллельном первом запуске несколько потоков не должны качать одновременно.
std::mutex g_ensure_mutex;

std::string cache_dir(const config::Format& fmt) {
    // ffmpeg-форматы (alac/tta/mpeg4_als) делят один бинарник — качаем его один раз.
    std::string id = fmt.engine_kind == "ffmpeg" ? "ffmpeg" : fmt.id;
    return util::join_path(config::bin_dir(), id);
}

std::string cached_binary(const config::Format& fmt) {
    std::string marker = util::join_path(cache_dir(fmt), ".binary");
    std::string path = util::trim(util::read_text(marker));
    if (!path.empty() && util::file_exists(path) && util::is_pe(path)) return path;
    // Маркер мог быть записан под другой ОС (wine сохраняет пути как Z:\..., на
    // Windows они не существуют, и наоборот). Если такой путь недействителен,
    // пробуем тот же файл в нашем кэше — кэш переносим между wine и Windows.
    if (!path.empty()) {
        std::string alt = util::join_path(cache_dir(fmt), util::base_name(path));
        if (util::file_exists(alt) && util::is_pe(alt)) return alt;
    }
    return {};
}

std::string in_path(const config::Format& fmt) {
    std::string name = fmt.engine_kind == "binary" ? fmt.engine_executable : "ffmpeg";
    std::string p = util::find_in_path(name);
    if (!p.empty() && util::is_pe(p)) return p;
    return {};
}

std::vector<config::DownloadEntry> entries_for_os(const config::Format& fmt) {
    std::vector<config::DownloadEntry> out;
    std::string os = util::current_os();
    for (const auto& e : fmt.downloads) {
        if (e.os == "any" || e.os == os) out.push_back(e);
    }
    if (out.empty()) {
        for (const auto& e : fmt.downloads) {
            if (e.os == "any") out.push_back(e);
        }
    }
    return out;
}

bool verify_checksum(const std::string& path, const std::string& expected, std::string* err) {
    if (expected.empty()) return true;
    auto data = util::read_file(path);
    if (data.empty()) {
        *err = i18n::str("downloaded file is empty (0 bytes) — the server throttled or dropped the connection");
        return false;
    }
    std::string actual = sha256::hex(data);
    if (util::to_lower(actual) != util::to_lower(expected)) {
        *err = i18n::fmt("checksum mismatch: expected %s, got %s (downloaded %s bytes — the download may have been interrupted)",
                  expected.c_str(), actual.c_str(), std::to_string(data.size()).c_str());
        return false;
    }
    return true;
}

std::string download_name(const std::string& url) {
    std::string u = url;
    size_t q = u.find('?');
    if (q != std::string::npos) u = u.substr(0, q);
    std::string base = util::base_name(u);
    if (base.empty() || base == "/" || base == ".") return "download.bin";
    return base;
}

// Копирует скачанный PE-файл в кэш, добавляя расширение .exe, если его нет
// (URL вроде .../x64 не содержит имени файла с расширением).
std::string ensure_exe_extension(const std::string& src, const std::string& cache,
                                 const std::string& filename) {
    std::string name = filename;
    std::string lower = util::to_lower(name);
    auto data = util::read_file(src);
    if (!util::ends_with(lower, ".exe") && data.size() >= 2 && data[0] == 'M' && data[1] == 'Z') {
        name += ".exe";
    }
    std::string dest = util::join_path(cache, name);
    util::copy_file(src, dest);
    return dest;
}

// Поиск 7-Zip: локальная копия (bin/7z/7z.exe или рядом с exe), затем PATH.
// Требуется полный 7z.exe (не 7zr): он умеет извлекать
// самораспаковывающиеся NSIS-установщики.
std::string find_7z() {
    std::string local = util::join_path(util::join_path(config::bin_dir(), "7z"), "7z.exe");
    if (util::file_exists(local)) return local;
    local = util::join_path(util::exe_dir(), "7z.exe");
    if (util::file_exists(local)) return local;
    return util::find_in_path("7z.exe");
}

// Рекурсивный поиск файла по имени (регистронезависимо) в каталоге.
std::string find_recursive(const std::string& root, const std::string& name) {
    std::error_code ec;
    for (const auto& e : std::filesystem::recursive_directory_iterator(std::filesystem::u8path(root), ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        if (util::to_lower(e.path().filename().u8string()) == util::to_lower(name)) {
            return e.path().u8string();
        }
    }
    return {};
}

// Возвращает путь к бинарнику либо пустую строку; message — статус/предупреждение.
std::string prepare_entry(const config::Format& fmt, const config::DownloadEntry& entry,
                          std::string* message) {
    const std::string& fmt_id = fmt.id;
    const std::string& kind = entry.kind;
    if (entry.url.empty()) {
        throw config::Error("[" + fmt_id + "] " + i18n::fmt("kind=%s needs a url", kind.c_str()));
    }

    std::string cache = cache_dir(fmt);
    util::mkdirs(config::bin_dir());
    util::mkdirs(cache);

    std::string tmp = util::join_path(cache, ".download");
    util::mkdirs(tmp);

    std::string filename = download_name(entry.url);

    // Скачивание с переиспользованием кэша: инсталлятор (kind=extract7z) остаётся
    // в bin/<id>/ и при повторном запуске не перекачивается и согласия не требует.
    // URL вроде ".../x64" даёт имя без расширения, а в кэш файл кладётся как x64.exe —
    // проверяем оба варианта.
    std::string cached = util::join_path(cache, filename);
    if (!util::file_exists(cached) && util::file_exists(cached + ".exe")) cached += ".exe";
    std::string archive_path = cached;
    if (!util::file_exists(archive_path)) {
        archive_path = util::join_path(tmp, filename);
        std::string dl_err;
        if (!download::get(entry.url, archive_path, &dl_err)) {
            throw config::Error("[" + fmt_id + "] " + i18n::fmt("could not download %s: %s", entry.url.c_str(), dl_err.c_str()));
        }
    }
    std::string verr;
    if (!verify_checksum(archive_path, entry.checksum, &verr)) {
        util::remove_file(archive_path);
        throw config::Error("[" + fmt_id + "] " + verr);
    }

    if (kind == "extract7z") {
        // Извлечение нужных файлов из установщика БЕЗ установки: плагины для
        // winamp/foobar не ставятся, ничего в системе не меняется.
        if (entry.files.empty()) {
            throw config::Error("[" + fmt_id + "] " + i18n::str("kind=extract7z needs a 'files' list"));
        }
        // Инсталлятор должен остаться в кэше (не в tmp/).
        if (archive_path != cached) {
            std::string dest = ensure_exe_extension(archive_path, cache, filename);
            util::remove_file(archive_path);
            archive_path = dest;
        }
        std::string sevenz = find_7z();
        if (sevenz.empty()) {
            std::string need;
            for (size_t i = 0; i < entry.files.size(); i++) {
                if (i) need += ", ";
                need += entry.files[i];
            }
            *message = i18n::fmt("7-Zip not found. Install 7-Zip or extract the files from the "
                            "installer manually (plugins are not needed, only: %s): %s",
                            need.c_str(), archive_path.c_str());
            return {};
        }
        std::string outdir = util::join_path(tmp, "extract");
        util::mkdirs(outdir);
        proc::Result xr = proc::run({sevenz, "x", "-y", archive_path, "-o" + outdir}, 300);
        if (!xr.started || xr.exit_code != 0) {
            throw config::Error("[" + fmt_id + "] " +
                                i18n::fmt("extracting %s via 7-Zip failed (code %d)",
                                          util::base_name(archive_path).c_str(), xr.exit_code));
        }
        for (const auto& name : entry.files) {
            std::string found = find_recursive(outdir, name);
            if (found.empty()) {
                throw config::Error("[" + fmt_id + "] " + i18n::fmt("file '%s' not found in the installer", name.c_str()));
            }
            util::copy_file(found, util::join_path(cache, name));
        }
        return util::join_path(cache, entry.files[0]);
    }

    // kind == "archive": zip-архив с бинарником.
    std::string extracted = util::join_path(tmp, "src");
    util::mkdirs(extracted);
    std::string xerr;
    if (!archive::extract_zip(archive_path, extracted, &xerr)) {
        util::remove_file(archive_path);
        throw config::Error("[" + fmt_id + "] " + i18n::fmt("unpacking %s: %s", filename.c_str(), xerr.c_str()));
    }
    util::remove_file(archive_path);

    // Если в каталоге один верхний подкаталог и нет файлов — спускаемся в него.
    std::string src_root = extracted;
    {
        std::vector<std::string> dirs, files;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(extracted), ec)) {
            if (e.is_directory()) dirs.push_back(e.path().u8string());
            else if (e.is_regular_file()) files.push_back(e.path().u8string());
        }
        if (dirs.size() == 1 && files.empty()) src_root = dirs[0];
    }

    std::string binary = archive::find_binary(src_root, entry.file_glob);
    if (binary.empty()) {
        throw config::Error("[" + fmt_id + "] " + i18n::fmt("no binary matching '%s' found in the archive",
                                                          entry.file_glob.c_str()));
    }
    std::string dest = util::join_path(cache, util::base_name(binary));
    util::copy_file(binary, dest);
    // Копируем соседние файлы бинарника (DLL и пр.) — без них exe может не запуститься.
    std::error_code ec;
    std::string parent = util::dir_name(binary);
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(parent), ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        std::string fname = e.path().filename().u8string();
        if (util::to_lower(fname) == util::to_lower(util::base_name(binary))) continue;
        util::copy_file(e.path().u8string(), util::join_path(cache, fname));
    }
    return dest;
}

void cli_check(const config::Format& fmt, const std::string& binary, std::string* message) {
    if (!fmt.cli_check.present) return;
    std::vector<std::string> args = {binary};
    args.insert(args.end(), fmt.cli_check.cmd.begin(), fmt.cli_check.cmd.end());
    proc::Result r = proc::run(args, 60);
    std::string out = util::to_lower(r.output);
    std::vector<std::string> missing;
    for (const auto& exp : fmt.cli_check.expect) {
        if (out.find(util::to_lower(exp)) == std::string::npos) missing.push_back(exp);
    }
    if (!missing.empty()) {
        std::string m = i18n::str("expected options not found in the utility output: ");
        for (size_t i = 0; i < missing.size(); i++) {
            if (i) m += ", ";
            m += missing[i];
        }
        m += i18n::str(" — the config may be outdated");
        if (!message->empty()) *message += "; ";
        *message += m;
    }
}

}  // namespace

Status ensure(const config::Format& fmt, bool download, const std::string& log_prefix) {
    Status st;

    auto try_existing = [&]() -> bool {
        std::string cached = cached_binary(fmt);
        if (!cached.empty()) {
            st.path = cached;
            st.status = "cache";
            cli_check(fmt, cached, &st.message);
            return true;
        }
        std::string inpath = in_path(fmt);
        if (!inpath.empty()) {
            st.path = inpath;
            st.status = "path";
            cli_check(fmt, inpath, &st.message);
            return true;
        }
        return false;
    };

    if (try_existing()) return st;
    if (!download) {
        st.status = "missing";
        return st;
    }

    // Двойная проверка под блокировкой: другой поток мог уже скачать формат.
    std::lock_guard<std::mutex> lk(g_ensure_mutex);
    if (try_existing()) return st;

    auto entries = entries_for_os(fmt);
    for (const auto& entry : entries) {
        try {
            std::string message;
            std::string path = prepare_entry(fmt, entry, &message);
            if (!path.empty()) {
                util::write_text(util::join_path(cache_dir(fmt), ".binary"), path);
                st.path = path;
                st.status = "downloaded";
                st.message = message;
                cli_check(fmt, path, &st.message);
                return st;
            }
            if (!message.empty()) {
                st.message = message;
            }
        } catch (const config::Error& exc) {
            st.message = exc.what();
            out::error("%s%s\n", log_prefix.c_str(), exc.what());
        } catch (const std::exception& exc) {
            st.message = exc.what();
            out::error("%s%s\n", log_prefix.c_str(), exc.what());
        }
    }
    st.status = "missing";
    return st;
}

}  // namespace tool
