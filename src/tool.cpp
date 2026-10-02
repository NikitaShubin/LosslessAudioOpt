#include "tool.h"

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <vector>

#include "archive.h"
#include "download.h"
#include "i18n.h"
#include "obs.h"
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
    if (p.empty()) return {};
#ifdef _WIN32
    if (util::is_pe(p)) return p;
    return {};
#else
    // На Linux принимаем нативные ELF-бинарики (flac, ffmpeg и т.д. из apt).
    return p;
#endif
}

std::vector<config::DownloadEntry> entries_for_os(const config::Format& fmt) {
    std::vector<config::DownloadEntry> out;
    const std::string os = util::current_os();
    auto want = [&](const std::string& eos) {
        if (eos == "any" || eos == os) return true;
        // Нативные .exe-кодеки (os=windows) на Linux запускаются под wine,
        // поэтому Windows-записи для не-Windows сборки тоже валидны.
#ifndef _WIN32
        if (eos == "windows") return true;
#endif
        return false;
    };
    for (const auto& e : fmt.downloads) {
        // `latest` — не рабочий рецепт, а шаблон для обновления (--update-codecs).
        // Обычная установка его не рассматривает.
        if (e.latest) continue;
        if (want(e.os)) out.push_back(e);
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
#ifdef _WIN32
    std::string local = util::join_path(util::join_path(config::bin_dir(), "7z"), "7z.exe");
    if (util::file_exists(local)) return local;
    local = util::join_path(util::exe_dir(), "7z.exe");
    if (util::file_exists(local)) return local;
    return util::find_in_path("7z.exe");
#else
    // На Linux: p7zip (apt install p7zip-full) даёт команду 7z.
    std::string p = util::find_in_path("7z");
    if (!p.empty()) return p;
    return util::find_in_path("7zr");
#endif
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
                          std::string* message,
                          const std::atomic<bool>* kill = nullptr) {
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
        proc::Result xr = proc::run({sevenz, "x", "-y", archive_path, "-o" + outdir}, 300,
                                        "", {}, kill);
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
    // Копируем файлы из files (если заданы) или соседей (DLL и пр.).
    if (!entry.files.empty()) {
        // Точечное копирование: только файлы из списка files.
        std::string parent = util::dir_name(binary);
        for (const auto& name : entry.files) {
            if (util::to_lower(name) == util::to_lower(util::base_name(binary))) continue;
            std::string found = find_recursive(parent, name);
            if (!found.empty()) {
                util::copy_file(found, util::join_path(cache, name));
            }
        }
    } else {
        // Обратная совместимость: копируем всех соседей (DLL и пр.).
        std::error_code ec;
        std::string parent = util::dir_name(binary);
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(parent), ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            std::string fname = e.path().filename().u8string();
            if (util::to_lower(fname) == util::to_lower(util::base_name(binary))) continue;
            util::copy_file(e.path().u8string(), util::join_path(cache, fname));
        }
    }
    return dest;
}

void cli_check(const config::Format& fmt, const std::string& binary, std::string* message,
                const std::atomic<bool>* kill = nullptr) {
    if (!fmt.cli_check.present) return;
    std::vector<std::string> args = {binary};
    args.insert(args.end(), fmt.cli_check.cmd.begin(), fmt.cli_check.cmd.end());
    proc::Result r = proc::run(args, 60, "", {}, kill);
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

// Ключ кэша cli_check: результат проверки зависит только от конфигурации
// (cmd/expect из formats/*.json) и самого бинарника (размер + mtime).
// Любое изменение инвалидирует кэш; окружение (wine/PATH) роли не играет.
std::string cli_check_key(const config::Format& fmt, const std::string& binary) {
    std::string raw;
    raw += binary;
    raw += "\ncmd:";
    for (const auto& s : fmt.cli_check.cmd) raw += s + " ";
    raw += "\nexpect:";
    for (const auto& s : fmt.cli_check.expect) raw += s + " ";
    raw += "\nsize:" + std::to_string(util::file_size(binary));
    raw += "\nmtime:" + std::to_string(util::file_mtime_ns(binary));
    auto data = std::vector<uint8_t>(raw.begin(), raw.end());
    return sha256::hex(data);
}

std::string cli_check_cache_path(const config::Format& fmt) {
    return util::join_path(cache_dir(fmt), ".cli-check");
}

// ---------------------------------------------------------------------------
// Обновление кодеков (--update-codecs)
// ---------------------------------------------------------------------------
//
// В formats/<id>.json лежат рецепты скачивания. Закреплённый — тот, по которому
// работает всё, у него есть проверенный checksum. Рецепт, помеченный
// `"latest": true`, — тот же, но без хэша и с адресом последней доступной версии;
// по нему кодек и обновляется. Рабочим он не является (см. entries_for_os).
//
// Порядок действий: скачать во временный каталог, сверить справку утилиты,
// посчитать хэш файла-источника и только потом подменить бинарники в кэше и
// переписать закреплённый рецепт. Сверка идёт до подмены по двум причинам:
// пользователь при расхождении остаётся с прежней рабочей версией и внятным
// сообщением, а не со сломанным кодеком; и запись в formats/*.json не меняется
// ни при каком исходе, кроме заведомо годного.
//
// Хэш считается по файлу, из которого получен бинарник: для extract7z это
// оставленный в кэше инсталлятор. Для archive архив удаляется после распаковки,
// и хэш бинарника не годится — пересборка архива даст другой файл при том же
// содержимом. В этом случае запись в конфиге не обновляется и возвращается
// failed с объяснением: записать бессмысленный хэш хуже, чем оставить старый.

std::string backup_dir(const config::Format& fmt) {
    return util::join_path(cache_dir(fmt), ".update-backup");
}

bool is_latest(const config::DownloadEntry& e) { return e.latest; }

// Рецепт, по которому сейчас работает кодек: первый подходящий под ОС, кроме
// помеченного latest.
bool pinned_entry(const config::Format& fmt, config::DownloadEntry* out) {
    const std::string os = util::current_os();
    for (const auto& e : fmt.downloads) {
        bool want = e.os == os || e.os == "any";
#ifndef _WIN32
        if (e.os == "windows") want = true;
#endif
        if (!want || is_latest(e)) continue;
        *out = e;
        return true;
    }
    return false;
}

bool latest_entry(const config::Format& fmt, config::DownloadEntry* out) {
    for (const auto& e : fmt.downloads) {
        if (is_latest(e)) {
            *out = e;
            return true;
        }
    }
    return false;
}

// Файл-источник, хэш которого можно записать в конфиг: инсталлятор extract7z
// остаётся в кэше. Для archive такого файла нет.
bool hashable_source(const config::Format& fmt, const config::DownloadEntry& entry,
                     std::string* path) {
    if (entry.kind != "extract7z") return false;
    std::string cache = cache_dir(fmt);
    std::string p = util::join_path(cache, download_name(entry.url));
    if (util::file_exists(p)) {
        *path = p;
        return true;
    }
    p += ".exe";
    if (util::file_exists(p)) {
        *path = p;
        return true;
    }
    return false;
}

// Резервная копия файлов кэша на время обновления. Внутренние файлы
// (.binary, .cli-check, .sha256) не копируются: они описывают проверку и должны
// пересчитаться для нового бинарника.
bool backup_cache(const config::Format& fmt) {
    std::string src = cache_dir(fmt);
    std::string dst = backup_dir(fmt);
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(dst), ec);
    if (!util::mkdirs(dst)) return false;
    for (const auto& e :
         std::filesystem::directory_iterator(std::filesystem::u8path(src), ec)) {
        if (ec) return false;
        if (!e.is_regular_file()) continue;
        std::string name = e.path().filename().u8string();
        if (!name.empty() && name[0] == '.') continue;
        if (!util::copy_file(e.path().u8string(), util::join_path(dst, name)))
            return false;
    }
    return true;
}

void restore_cache(const config::Format& fmt) {
    std::string src = backup_dir(fmt);
    std::error_code ec;
    if (!util::dir_exists(src)) return;
    for (const auto& e :
         std::filesystem::directory_iterator(std::filesystem::u8path(src), ec)) {
        if (ec) return;
        if (!e.is_regular_file()) continue;
        util::copy_file(e.path().u8string(),
                        util::join_path(cache_dir(fmt), e.path().filename().u8string()));
    }
    std::filesystem::remove_all(std::filesystem::u8path(src), ec);
}

// Перезаписывает закреплённый рецепт в formats/<id>.json: ставит проверенный url
// и хэш, а рядом добавляет шаблон `latest` для следующего обновления.
//
// Файл пересобирается целиком, но через ordered_json: он сохраняет порядок ключей
// при разборе, поэтому dump(2) даёт исходное форматирование, и в diff видно только
// смену версии. Обычный nlohmann::json сортирует ключи (std::map) — пересборка на
// нём превращала бы правку одного хэша в перестановку сотен строк.
bool repin(const config::Format& fmt, const config::DownloadEntry& latest,
           const std::string& checksum_hex) {
    std::string path = util::join_path(config::formats_dir(), fmt.id + ".json");
    nlohmann::ordered_json data;
    try {
        data = nlohmann::ordered_json::parse(util::read_text(path));
    } catch (const std::exception&) {
        return false;
    }
    if (!data.contains("downloads") || !data["downloads"].is_array()) return false;

    // Заметки прежнего закреплённого рецепта относятся к проверенной версии
    // (там, где это единственный источник сведений о версии), поэтому
    // сохраняются; шаблон latest получает собственные.
    std::string pinned_notes;
    for (const auto& dl : data["downloads"]) {
        bool is_latest_recipe =
            dl.contains("latest") && dl.at("latest").is_boolean() &&
            dl.at("latest").get<bool>();
        if (is_latest_recipe) continue;
        if (dl.contains("notes") && dl.at("notes").is_string()) {
            pinned_notes = dl.at("notes").get<std::string>();
            break;
        }
    }

    nlohmann::ordered_json fresh;
    fresh["os"] = latest.os;
    fresh["kind"] = latest.kind;
    fresh["url"] = latest.url;
    if (!latest.file_glob.empty()) fresh["file_glob"] = latest.file_glob;
    if (!latest.files.empty()) fresh["files"] = latest.files;
    if (!pinned_notes.empty()) fresh["notes"] = pinned_notes;
    else if (!latest.notes.empty()) fresh["notes"] = latest.notes;
    // Хэш закрепляем только там, где адрес указывает на конкретную версию. У
    // вечнозелёной ссылки (…/releases/latest/…) файл меняется при каждой загрузке,
    // и записанный хэш отверг бы следующую же установку.
    if (latest.pin_checksum) fresh["checksum"] = {{"type", "sha256"}, {"value", checksum_hex}};

    // Шаблон для следующего обновления: тот же рецепт без хэша.
    nlohmann::ordered_json tmpl;
    tmpl["os"] = latest.os;
    tmpl["latest"] = true;
    tmpl["pinned_kind"] = latest.kind;
    if (!latest.pin_checksum) tmpl["pin_checksum"] = false;
    tmpl["url"] = latest.url;
    if (!latest.file_glob.empty()) tmpl["file_glob"] = latest.file_glob;
    if (!latest.files.empty()) tmpl["files"] = latest.files;
    if (!latest.notes.empty()) tmpl["notes"] = latest.notes;

    nlohmann::ordered_json downloads = nlohmann::ordered_json::array();
    downloads.push_back(fresh);
    for (const auto& dl : data["downloads"]) {
        bool is_latest_recipe =
            dl.contains("latest") && dl.at("latest").is_boolean() &&
            dl.at("latest").get<bool>();
        if (is_latest_recipe) continue;
        // Прежний рабочий рецепт с тем же url больше не нужен: он указывает на
        // уже скачанный файл, а fresh заменил его. Без этой проверки повторное
        // обновление накапливало бы копии одной и той же записи.
        if (dl.value("url", std::string()) == latest.url &&
            dl.value("kind", std::string()) == latest.kind)
            continue;
        downloads.push_back(dl);
    }
    downloads.push_back(tmpl);
    data["downloads"] = downloads;
    return util::write_text(path, data.dump(2) + "\n");
}

// Сбросить маркеры, описывающие прежний бинарник: .binary указывает на старый
// файл, .cli-check закеширован по его размеру и mtime.
void forget_marker(const config::Format& fmt) {
    util::remove_file(util::join_path(cache_dir(fmt), ".binary"));
    util::remove_file(util::join_path(cache_dir(fmt), ".cli-check"));
}

void clear_backup(const config::Format& fmt) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(backup_dir(fmt)), ec);
}

UpdateResult update_one(const config::Format& fmt, const std::atomic<bool>* kill) {
    UpdateResult r;
    r.id = fmt.id;
    config::DownloadEntry pinned;
    if (!pinned_entry(fmt, &pinned)) {
        r.status = "failed";
        r.message = i18n::str("no pinned download entry in the config");
        return r;
    }
    config::DownloadEntry latest;
    if (!latest_entry(fmt, &latest)) {
        r.status = "skipped";
        r.message = i18n::str(
            "no 'latest' entry — add one to formats/<id>.json to make this codec updatable");
        return r;
    }
    r.from = util::base_name(pinned.url);
    r.to = util::base_name(latest.url);

    std::error_code ec;
    // Прежний хэш берём из закреплённого рецепта, а не с диска: для kind=archive
    // архив удаляется после распаковки, и файла-источника уже нет. Хэш в конфиге
    // — ровно то, с чем нужно сравнить новый файл.
    const std::string before_hex = pinned.checksum;

    if (!backup_cache(fmt)) {
        r.status = "failed";
        r.message = i18n::str("could not back up the codec cache");
        return r;
    }

    // prepare_entry кладёт файлы прямо в bin/<id>/, поэтому откат обеспечен
    // резервной копией: при любом неуспехе ниже файлы возвращаются на место, и
    // прежний рабочий кодек остаётся в силе.
    std::string message;
    std::string path;
    try {
        path = prepare_entry(fmt, latest, &message, kill);
    } catch (const std::exception& exc) {
        restore_cache(fmt);
        r.status = "failed";
        r.message = util::one_line(exc.what());
        return r;
    }
    if (path.empty()) {
        restore_cache(fmt);
        r.status = "failed";
        r.message = message.empty() ? i18n::str("nothing to install") : util::one_line(message);
        return r;
    }

    // Сверка справки: утилита другой версии может вести себя иначе, и запись с
    // рабочим кодеком лучше, чем запись с непредсказуемым.
    std::string check;
    cli_check(fmt, path, &check, kill);
    if (!check.empty()) {
        restore_cache(fmt);
        r.status = "failed";
        r.message = i18n::fmt("the new utility does not match cli_check.expect: %s",
                              util::one_line(check).c_str());
        return r;
    }

    // Хэш считаем по файлу-источнику — тому же, который качается при следующей
    // установке. Для extract7z это оставленный в кэше инсталлятор. Для archive
    // такого файла нет (архив удаляется после распаковки), и хэш бинарника
    // записывать нельзя: при следующей загрузке сверялся бы уже другой файл, и
    // установка падала бы с несовпадением. Поэтому для archive хэш не пишется,
    // и запись остаётся с пустым checksum — сверку обеспечивает сам загрузчик
    // (digest из API GitHub).
    std::string hash_path;
    if (!hashable_source(fmt, latest, &hash_path)) {
        if (!repin(fmt, latest, std::string())) {
            restore_cache(fmt);
            r.status = "failed";
            r.message = i18n::fmt("could not update formats/%s.json", fmt.id.c_str());
            return r;
        }
        forget_marker(fmt);
        clear_backup(fmt);
        r.status = "updated";
        r.message = i18n::str(
            "no checksum source for kind=archive — the checksum stays empty "
            "(the downloader verifies via the GitHub digest)");
        return r;
    }
    std::string hex = sha256::hex(util::read_file(hash_path));
    if (hex.empty()) {
        restore_cache(fmt);
        r.status = "failed";
        r.message = i18n::str(
            "could not compute the checksum of the downloaded file — the config is left unchanged");
        return r;
    }

    // Хэш совпал с прежним: это тот же файл, обновляться нечему. Для
    // вечнозелёного адреса (pin_checksum=false) сравнивать не с чем — там
    // «прежний» хэш в конфиге отсутствует, и каждый прогон качает заново.
    if (latest.pin_checksum && !before_hex.empty() && hex == before_hex) {
        restore_cache(fmt);
        r.status = "unchanged";
        r.message = i18n::str("the downloaded file is identical to the pinned one");
        return r;
    }

    if (!repin(fmt, latest, hex)) {
        restore_cache(fmt);
        r.status = "failed";
        r.message = i18n::fmt("could not update formats/%s.json", fmt.id.c_str());
        return r;
    }

    forget_marker(fmt);
    clear_backup(fmt);
    util::write_text(util::join_path(cache_dir(fmt), ".sha256"), hex + "\n");

    r.status = "updated";
    r.message = i18n::fmt("sha256 %s", hex.c_str());
    return r;
}

}  // namespace

// Проверка готовности утилиты формата: находит бинарник (кэш bin/<id>/.binary
// или PATH) и прогоняет cli_check.expect. Возвращает список проблем (пустой —
// утилита готова). Не скачивает и не изменяет state: только чтение.
// Результат проверки кэшируется в bin/<id>/.cli-check (ключ = sha256 от
// cmd/expect + размер/mtime бинарника) — повторный старт не гоняет утилиту
// (на Linux это запуск .exe через wine, дорого).
std::string check_config(const config::Format& fmt, const std::atomic<bool>* kill) {
    if (!fmt.cli_check.present) return {};
    std::string binary = cached_binary(fmt);
    if (binary.empty()) binary = in_path(fmt);
    if (binary.empty()) {
        return i18n::str("utility not found (run `llao tools` or install it into bin/<id>/)");
    }
    std::string cache_path = cli_check_cache_path(fmt);
    std::string key = cli_check_key(fmt, binary);
    if (util::trim(util::read_text(cache_path)) == key) return {};
    std::string message;
    cli_check(fmt, binary, &message, kill);
    if (message.empty()) util::write_text(cache_path, key);
    return message;
}

Status ensure(const config::Format& fmt, bool download, const std::string& log_prefix,
              const std::atomic<bool>* kill) {
    Status st;

    auto try_existing = [&]() -> bool {
        std::string cached = cached_binary(fmt);
        if (!cached.empty()) {
            st.path = cached;
            st.status = "cache";
            return true;
        }
        std::string inpath = in_path(fmt);
        if (!inpath.empty()) {
            st.path = inpath;
            st.status = "path";
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
            std::string path = prepare_entry(fmt, entry, &message, kill);
            if (!path.empty()) {
                util::write_text(util::join_path(cache_dir(fmt), ".binary"), path);
                st.path = path;
                st.status = "downloaded";
                st.message = message;
                return st;
            }
            if (!message.empty()) {
                st.message = message;
            }
        } catch (const config::Error& exc) {
            st.message = exc.what();
            obs::sink()->error(log_prefix + exc.what() + "\n");
        } catch (const std::exception& exc) {
            st.message = exc.what();
            obs::sink()->error(log_prefix + exc.what() + "\n");
        }
    }
    st.status = "missing";
    return st;
}

std::vector<UpdateResult> update_codecs(const std::vector<config::Format>& fmts,
                                        const std::atomic<bool>* kill) {
    std::vector<UpdateResult> out;
    // ffmpeg-форматы (alac/tta/mpeg4_als) делят один бинарник в bin/ffmpeg/.
    // Скачивать его на каждый формат заново — это сотня мегабайт впустую, поэтому
    // первым из них проходим обновление, остальным переносим тот же результат:
    // файлы те же, сверка та же, отличается только запись в formats/<id>.json.
    bool ffmpeg_done = false;
    UpdateResult ffmpeg_result;
    for (const auto& f : fmts) {
        if (!f.enabled) continue;
        if (kill && kill->load(std::memory_order_relaxed)) break;
        if (f.engine_kind == "ffmpeg" && ffmpeg_done) {
            UpdateResult r = ffmpeg_result;
            r.id = f.id;
            out.push_back(r);
            continue;
        }
        UpdateResult r = update_one(f, kill);
        if (f.engine_kind == "ffmpeg") {
            ffmpeg_done = true;
            ffmpeg_result = r;
        }
        out.push_back(r);
    }
    return out;
}

}  // namespace tool
