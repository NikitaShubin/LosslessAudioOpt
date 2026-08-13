#include "archive.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "miniz/miniz.h"
#include "i18n.h"
#include "util.h"

namespace fs = std::filesystem;

namespace archive {

// Нормализует имя члена архива: разделители '\' -> '/', убирает ведущие '/', './' и
// компоненты '..'. Возвращает пустую строку при небезопасном имени.
static std::string safe_name(const char* raw) {
    std::string name = raw ? raw : "";
    for (auto& c : name) {
        if (c == '\\') c = '/';
    }
    // Убираем дублирующиеся и ведущие '/'.
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= name.size()) {
        size_t pos = name.find('/', start);
        std::string part = name.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!part.empty() && part != ".") {
            if (part == "..") return {};  // выход за пределы
            if (part.size() >= 2 && part[1] == ':') return {};  // drive letter
            parts.push_back(part);
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += "/";
        out += parts[i];
    }
    return out;
}

bool extract_zip(const std::string& archive_path, const std::string& dst, std::string* err) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_file(&z, archive_path.c_str(), 0)) {
        *err = i18n::fmt("could not open zip: %s", archive_path.c_str());
        return false;
    }
    mz_uint n = mz_zip_reader_get_num_files(&z);
    bool ok = true;
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&z, i, &st)) {
            *err = i18n::fmt("could not read archive member #%d", (int)i);
            ok = false;
            break;
        }
        std::string name = safe_name(st.m_filename);
        if (name.empty()) {
            *err = i18n::fmt("unsafe name in the archive: %s", std::string(st.m_filename).c_str());
            ok = false;
            break;
        }
        if (st.m_is_directory) continue;
        std::string out_path = util::join_path(dst, name);
        util::mkdirs(util::dir_name(out_path));
        if (!mz_zip_reader_extract_to_file(&z, i, out_path.c_str(), 0)) {
            *err = i18n::fmt("extraction error: %s (possibly a corrupted archive)", name.c_str());
            ok = false;
            break;
        }
    }
    mz_zip_reader_end(&z);
    return ok;
}

static int binary_priority(const std::string& path) {
    std::string lower = util::to_lower(path);
    int score = 0;
    std::string name = util::base_name(path);
    std::string ln = util::to_lower(name);
    if (ln.size() >= 4 && ln.substr(ln.size() - 4) == ".exe") score = 0;
    else if (ln.size() >= 4 && (ln.substr(ln.size() - 4) == ".cmd" || ln.substr(ln.size() - 4) == ".bat")) score = 1;
    else score = 2;
    // Предпочитаем 64-битные сборки (под wine32 может не работать).
    if (lower.find("win64") != std::string::npos || lower.find("/x64") != std::string::npos ||
        lower.find("x64/") != std::string::npos || lower.find("amd64") != std::string::npos ||
        lower.find("win_x64") != std::string::npos) {
        score -= 10;
    }
    if (lower.find("win32") != std::string::npos || lower.find("/x86") != std::string::npos ||
        lower.find("i386") != std::string::npos) {
        score += 10;
    }
    return score;
}

std::string find_binary(const std::string& root, const std::string& pattern) {
    std::string tail;
    if (!pattern.empty()) {
        size_t pos = pattern.find_last_of('/');
        tail = pos == std::string::npos ? pattern : pattern.substr(pos + 1);
    }
    std::string pat_l = util::to_lower(pattern);
    std::string tail_l = util::to_lower(tail);
    std::vector<std::string> hits;
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::u8path(root), fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        std::string full = it->path().u8string();
        std::string rel = it->path().lexically_relative(fs::u8path(root)).u8string();
        std::replace(rel.begin(), rel.end(), '\\', '/');
        std::string base = util::base_name(full);
        bool ok = pattern.empty() || util::fnmatch(pat_l, util::to_lower(rel)) ||
                  util::fnmatch(tail_l, util::to_lower(base));
        if (ok) hits.push_back(full);
    }
    if (hits.empty()) return {};
    std::sort(hits.begin(), hits.end(), [](const std::string& a, const std::string& b) {
        int pa = binary_priority(a), pb = binary_priority(b);
        if (pa != pb) return pa < pb;
        return a < b;
    });
    return hits.front();
}

}  // namespace archive
