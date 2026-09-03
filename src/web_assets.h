#pragma once
#include <string>
#include <unordered_map>

namespace web_assets {

// Сгенерированные данные (web_assets_data.cpp)
extern const unsigned char zip_data[];
extern const size_t zip_size;

// Распаковывает zip_data в память (один раз). Возвращает true при успехе.
bool init(std::string* err);

// Доступ к распакованным файлам (ключ — путь относительно web/, напр. "index.html").
const std::unordered_map<std::string, std::string>& files();
const std::string* get(const std::string& path);
std::string mime_type(const std::string& path);

}  // namespace web_assets
