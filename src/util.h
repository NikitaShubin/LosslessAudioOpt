#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace util {

std::wstring u2w(const std::string& s);
std::string  w2u(const std::wstring& s);

std::string join_path(const std::string& a, const std::string& b);
std::string dir_name(const std::string& p);
std::string base_name(const std::string& p);
std::string to_lower(const std::string& s);
std::string trim(const std::string& s);
bool ends_with(const std::string& s, const std::string& suffix);

std::string exe_path();               // полный путь к исполняемому файлу
std::string exe_dir();                // каталог, где лежит исполняемый файл

bool file_exists(const std::string& p);
bool dir_exists(const std::string& p);
bool mkdirs(const std::string& p);
bool remove_file(const std::string& p);
uint64_t file_size(const std::string& p);

std::vector<uint8_t> read_file(const std::string& p);   // пусто при ошибке
bool write_file(const std::string& p, const std::vector<uint8_t>& data);
std::string read_text(const std::string& p);            // пусто при ошибке
bool write_text(const std::string& p, const std::string& s);
bool copy_file(const std::string& src, const std::string& dst);

// Безопасная замена файла на месте.
struct ReplaceResult {
    bool ok = false;               // файл успешно заменён (original теперь = tmp)
    bool original_lost = false;    // оригинал НЕ восстановлен, лежит в backup
    std::string backup;            // путь, где лежит оригинал (при original_lost)
    std::string error;             // описание причины (при !ok)
};

// Заменяет original на tmp, никогда не перезаписывая содержимое существующих
// файлов через копирование: original переносится в backup, затем tmp переносится
// на место original (fs::rename с повторами — антивирус/индексатор могут
// короткое время держать файл). При сбое второго шага выполняется rollback
// (backup возвращается в original). original и tmp должны лежать на одном томе.
ReplaceResult replace_file(const std::string& original, const std::string& tmp,
                           const std::string& backup);

// Свободное место на диске, содержащем путь (байты; 0 при ошибке).
// Используется для адаптивного лимита параллелизма (бюджет tmp).
uint64_t disk_free_bytes(const std::string& path);
// Доступная физическая память (байты; 0 при ошибке).
uint64_t avail_ram_bytes();

// Поиск исполняемого файла в PATH (добавляет .exe при необходимости).
std::string find_in_path(const std::string& name);

// Текущая ОС для выбора записей downloads[].
std::string current_os();  // "windows" | "linux" | ...

// Проверяет, что файл — Windows PE (первые байты MZ). Нужно, чтобы под wine
// не подхватить Linux ELF-бинарник вместо Windows-версии.
bool is_pe(const std::string& path);

// Glob-сопоставление: '*' — любая последовательность (включая '/'), '?' — один символ.
bool fnmatch(const std::string& pattern, const std::string& name);

std::vector<std::string> split(const std::string& s, char sep);

// Декодирует base64 (MIME, с переносами) в бинарные данные. false при ошибке.
bool from_base64(const std::string& s, std::vector<uint8_t>* out);

// Заменяет невалидные UTF-8 байты на '?' (строки из внешних источников:
// вывод утилит, теги в кодовой странице и т.п.). Позволяет безопасно
// сериализовать такие строки в JSON (иначе nlohmann::json::dump бросает
// type_error 316 "invalid UTF-8").
std::string sanitize_utf8(const std::string& s);

// Рекурсивно санитизирует все строки (включая ключи) в JSON-дереве.
// Вызывать перед dump(), чтобы строки из внешних источников (вывод утилит,
// теги в кодовой странице) не бросали type_error 316.
void sanitize_json(nlohmann::json& j);

}  // namespace util
