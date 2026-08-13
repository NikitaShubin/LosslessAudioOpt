#pragma once
#include <string>

namespace download {

// Скачивает url в dest. Возвращает true при успехе, иначе false (err — описание).
bool get(const std::string& url, const std::string& dest, std::string* err);

}  // namespace download
