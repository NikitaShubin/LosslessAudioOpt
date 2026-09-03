#include "web_assets.h"

#include <cstring>
#include <mutex>

#include "miniz/miniz.h"

namespace web_assets {

namespace {
std::unordered_map<std::string, std::string> g_files;
std::once_flag g_once;
bool g_ok = false;
std::string g_err;
}  // namespace

static std::string lower_ext(const std::string& p) {
    size_t dot = p.rfind('.');
    if (dot == std::string::npos) return "";
    std::string e = p.substr(dot + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

std::string mime_type(const std::string& path) {
    std::string e = lower_ext(path);
    if (e == "html" || e == "htm") return "text/html; charset=utf-8";
    if (e == "js") return "application/javascript; charset=utf-8";
    if (e == "css") return "text/css; charset=utf-8";
    if (e == "json") return "application/json; charset=utf-8";
    if (e == "svg") return "image/svg+xml";
    if (e == "png") return "image/png";
    if (e == "ico") return "image/x-icon";
    return "application/octet-stream";
}

bool init(std::string* err) {
    std::call_once(g_once, []() {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_mem(&zip, zip_data, zip_size, 0)) {
            g_err = "mz_zip_reader_init_mem failed";
            return;
        }
        mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n; i++) {
            mz_zip_archive_file_stat st{};
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            if (st.m_is_directory) continue;
            size_t sz = (size_t)st.m_uncomp_size;
            std::string buf;
            buf.resize(sz);
            if (!mz_zip_reader_extract_to_mem(&zip, i, buf.data(), sz, 0)) continue;
            g_files[st.m_filename] = std::move(buf);
        }
        mz_zip_reader_end(&zip);
        g_ok = !g_files.empty();
        if (!g_ok && g_err.empty()) g_err = "zip пуст или распаковка не дала файлов";
    });
    if (!g_ok && err) *err = g_err;
    return g_ok;
}

const std::unordered_map<std::string, std::string>& files() {
    return g_files;
}

const std::string* get(const std::string& path) {
    auto it = g_files.find(path);
    if (it == g_files.end()) return nullptr;
    return &it->second;
}

}  // namespace web_assets
