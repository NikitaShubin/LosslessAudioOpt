// Unit-тесты FileSession и ResourceManager.
// Standalone: не линкуется с optimize.o, дублирует минимальную логику для проверки.
//
// Сборка:  make test-unit
// Запуск:  ./test-unit

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Минимальные утилиты (изолированы от util.h)
// ---------------------------------------------------------------------------

static std::string tmp_dir_env() {
    const char* v = std::getenv("TMPDIR");
    return v ? std::string(v) : "/tmp";
}

static uint64_t disk_free_bytes(const std::string& path) {
    std::error_code ec;
    auto s = fs::space(fs::u8path(path), ec);
    return ec ? 0 : s.available;
}

// ---------------------------------------------------------------------------
// FileSession — копия из optimize.cpp (без зависимостей от util.h)
// ---------------------------------------------------------------------------

class FileSession {
public:
    FileSession(const std::string& original_path, const std::string& tmp_base)
        : path_(original_path) {
        // Простой токен: hex от пути (для тестов — хэш через std::hash)
        size_t h = std::hash<std::string>{}(original_path);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016zx", h);
        dir_ = fs::u8path(tmp_base) / buf;
        fs::create_directories(dir_);
    }

    ~FileSession() { cleanup(); }

    FileSession(const FileSession&) = delete;
    FileSession& operator=(const FileSession&) = delete;

    FileSession(FileSession&& o) noexcept
        : dir_(std::move(o.dir_)), path_(std::move(o.path_)) {
        o.dir_.clear();
    }

    FileSession& operator=(FileSession&& o) noexcept {
        if (this != &o) {
            cleanup();
            dir_ = std::move(o.dir_);
            path_ = std::move(o.path_);
            o.dir_.clear();
        }
        return *this;
    }

    const fs::path& dir() const { return dir_; }
    const std::string& original_path() const { return path_; }

    fs::path ref_wav_path() const { return dir_ / "ref.wav"; }

    fs::path candidate_path(const std::string& fmt_id,
                            const std::string& variant_id,
                            const std::string& ext) const {
        return dir_ / (fmt_id + "." + variant_id + "." + ext);
    }

    fs::path sidecar_path(const std::string& fmt_id) const {
        return dir_ / (fmt_id + ".tags.zip");
    }

    void cleanup() {
        if (!dir_.empty()) {
            std::error_code ec;
            fs::remove_all(dir_, ec);
            dir_.clear();
        }
    }

    bool ok() const { return !dir_.empty(); }

private:
    fs::path dir_;
    std::string path_;
};

// ---------------------------------------------------------------------------
// DiskBudget — копия из optimize.cpp
// ---------------------------------------------------------------------------

struct DiskBudget {
    std::mutex m;
    uint64_t reserved = 0;
    uint64_t min_free = 1ull << 30;

    bool try_reserve(const std::string& tmp_path, uint64_t bytes) {
        std::lock_guard<std::mutex> lk(m);
        uint64_t free = disk_free_bytes(tmp_path);
        uint64_t avail = free > (reserved + min_free) ? free - reserved - min_free : 0;
        if (bytes > avail) return false;
        reserved += bytes;
        return true;
    }

    void release(uint64_t bytes) {
        std::lock_guard<std::mutex> lk(m);
        reserved = (bytes >= reserved) ? 0 : reserved - bytes;
    }
};

// ---------------------------------------------------------------------------
// ResourceRequest + ResourceManager — упрощённая версия (без FileJob)
// ---------------------------------------------------------------------------

struct ResourceRequest {
    uint64_t disk_bytes = 0;
    enum class Status { Granted, Deferred, Denied };
    Status status = Status::Deferred;
};

class ResourceManager {
public:
    ResourceManager() = default;
    ResourceManager(const std::string& tmp_path, int max_workers, uint64_t min_free = 1ull << 30)
        : tmp_path_(tmp_path), max_workers_(max_workers) {
        disk_.min_free = min_free;
    }

    ResourceRequest request_disk(uint64_t bytes) {
        ResourceRequest req;
        req.disk_bytes = bytes;
        req.status = disk_.try_reserve(tmp_path_, bytes)
                         ? ResourceRequest::Status::Granted
                         : ResourceRequest::Status::Deferred;
        return req;
    }

    void release_disk(uint64_t bytes) { disk_.release(bytes); }

    void on_prep_started() { active_preps_++; }
    void on_prep_completed() { if (active_preps_ > 0) active_preps_--; }

    const std::string& tmp_path() const { return tmp_path_; }
    int max_workers() const { return max_workers_; }
    int active_preps() const { return active_preps_; }

    void set_max_workers(int w) { max_workers_ = w; }
    void set_tmp_path(const std::string& p) { tmp_path_ = p; }

private:
    DiskBudget disk_;
    std::string tmp_path_;
    int max_workers_ = 1;
    int active_preps_ = 0;
};

// ---------------------------------------------------------------------------
// Тесты
// ---------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) static void test_##name(); \
    struct Register_##name { Register_##name() { test_##name(); } } reg_##name; \
    static void test_##name()

#define ASSERT(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", #expr, __LINE__); \
        g_failed++; \
        return; \
    } \
    g_passed++; \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s == %s (line %d)\n", #a, #b, __LINE__); \
        g_failed++; \
        return; \
    } \
    g_passed++; \
} while(0)

// --- FileSession tests ---

TEST(file_session_creates_dir) {
    std::string base = "/tmp/llao_test_unit";
    fs::create_directories(base);
    fs::path session_dir;

    {
        FileSession s("/some/path/audio.flac", base);
        ASSERT(s.ok());
        ASSERT(fs::is_directory(s.dir()));
        session_dir = s.dir();
    }
    // После деструктора session-subdir удалена, но base остаётся
    ASSERT(!fs::exists(session_dir));
    std::error_code ec;
    fs::remove_all(fs::u8path(base), ec);
}

TEST(file_session_cleanup_explicit) {
    std::string base = "/tmp/llao_test_unit2";
    fs::create_directories(base);

    FileSession s("/some/path/audio.flac", base);
    fs::path d = s.dir();
    ASSERT(fs::is_directory(d));

    // Создаём файл внутри
    std::ofstream f(d / "ref.wav");
    f << "test"; f.close();
    ASSERT(fs::exists(d / "ref.wav"));

    s.cleanup();
    ASSERT(!fs::exists(d));
    ASSERT(!s.ok());
    // Повторный вызов — не падает
    s.cleanup();
}

TEST(file_session_raii_removes_files) {
    std::string base = "/tmp/llao_test_unit3";
    fs::create_directories(base);
    fs::path session_dir;

    {
        FileSession s("/some/path/audio.flac", base);
        session_dir = s.dir();
        // Создаём несколько файлов
        std::ofstream(s.dir() / "ref.wav") << "x";
        std::ofstream(s.dir() / "flac.default.flac") << "y";
        std::ofstream(s.dir() / "flac.default.flac.dec.wav") << "z";
        ASSERT(fs::exists(s.dir() / "ref.wav"));
        ASSERT(fs::exists(s.dir() / "flac.default.flac"));
    }
    ASSERT(!fs::exists(session_dir));
    std::error_code ec;
    fs::remove_all(fs::u8path(base), ec);
}

TEST(file_session_path_generation) {
    std::string base = "/tmp/llao_test_unit4";
    fs::create_directories(base);

    FileSession s("/music/track.flac", base);
    ASSERT_EQ(s.ref_wav_path().filename().string(), "ref.wav");
    ASSERT_EQ(s.candidate_path("flac", "default", "flac").filename().string(),
              "flac.default.flac");
    ASSERT_EQ(s.sidecar_path("flac").filename().string(), "flac.tags.zip");

    s.cleanup();
    std::error_code ec;
    fs::remove_all(fs::u8path(base), ec);
}

TEST(file_session_move_semantics) {
    std::string base = "/tmp/llao_test_unit5";
    fs::create_directories(base);

    FileSession s1("/a.wav", base);
    fs::path d1 = s1.dir();
    ASSERT(fs::is_directory(d1));

    FileSession s2 = std::move(s1);
    ASSERT_EQ(s2.dir(), d1);
    ASSERT(!s1.ok());  // s1 опустошён
    ASSERT(fs::is_directory(d1));  // директория жива

    s2.cleanup();
    std::error_code ec;
    fs::remove_all(fs::u8path(base), ec);
}

TEST(file_session_unique_tokens) {
    std::string base = "/tmp/llao_test_unit6";
    fs::create_directories(base);

    FileSession s1("/a.wav", base);
    FileSession s2("/b.wav", base);
    ASSERT(s1.dir() != s2.dir());

    s1.cleanup();
    s2.cleanup();
    std::error_code ec;
    fs::remove_all(fs::u8path(base), ec);
}

// --- DiskBudget tests ---

TEST(disk_budget_reserve_release) {
    DiskBudget db;
    db.min_free = 0;  // отключаем страховку для теста

    std::string tmp = tmp_dir_env();
    // Резервируем маленький кусок — должен пройти
    ASSERT(db.try_reserve(tmp, 1024));
    ASSERT_EQ(db.reserved, 1024u);

    db.release(512);
    ASSERT_EQ(db.reserved, 512u);

    db.release(512);
    ASSERT_EQ(db.reserved, 0u);
}

TEST(disk_budget_release_overflow) {
    DiskBudget db;
    db.min_free = 0;

    std::string tmp = tmp_dir_env();
    ASSERT(db.try_reserve(tmp, 100));
    db.release(9999);  // больше чем reserved
    ASSERT_EQ(db.reserved, 0u);
}

TEST(disk_budget_min_free_blocks) {
    DiskBudget db;
    db.min_free = UINT64_MAX;  // гарантированно блокирует

    std::string tmp = tmp_dir_env();
    ASSERT(!db.try_reserve(tmp, 1));
}

// --- ResourceManager tests ---

TEST(rm_request_disk_granted) {
    ResourceManager rm("/tmp", 4, 0);
    auto req = rm.request_disk(1024);
    ASSERT_EQ(static_cast<int>(req.status),
              static_cast<int>(ResourceRequest::Status::Granted));
    rm.release_disk(1024);
}

TEST(rm_prep_counter) {
    ResourceManager rm("/tmp", 4, 0);
    ASSERT_EQ(rm.active_preps(), 0);
    rm.on_prep_started();
    ASSERT_EQ(rm.active_preps(), 1);
    rm.on_prep_started();
    ASSERT_EQ(rm.active_preps(), 2);
    rm.on_prep_completed();
    ASSERT_EQ(rm.active_preps(), 1);
    rm.on_prep_completed();
    ASSERT_EQ(rm.active_preps(), 0);
    // Не уходит в минус
    rm.on_prep_completed();
    ASSERT_EQ(rm.active_preps(), 0);
}

TEST(rm_setters) {
    ResourceManager rm;
    rm.set_tmp_path("/custom");
    ASSERT_EQ(rm.tmp_path(), "/custom");
    rm.set_max_workers(8);
    ASSERT_EQ(rm.max_workers(), 8);
}

// ---------------------------------------------------------------------------

int main() {
    // Тесты запускаются через статические регистрации выше.
    // Если дошли сюда — всё прошло.
    if (g_failed == 0) {
        fprintf(stderr, "ALL PASSED (%d assertions)\n", g_passed);
        return 0;
    } else {
        fprintf(stderr, "%d/%d FAILED\n", g_failed, g_passed + g_failed);
        return 1;
    }
}
