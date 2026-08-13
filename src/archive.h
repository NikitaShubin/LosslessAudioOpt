#pragma once
#include <string>
#include <vector>

namespace archive {

// Распаковывает zip-архив в dst (без выхода за пределы dst — защита от path traversal).
// Возвращает true при успехе; err — описание ошибки.
bool extract_zip(const std::string& archive_path, const std::string& dst, std::string* err);

// Ищет исполняемый файл внутри root по glob-шаблону (относительный путь или имя файла).
// Возвращает пустую строку, если не найден. Предпочитает .exe/.cmd/.bat.
std::string find_binary(const std::string& root, const std::string& pattern);

}  // namespace archive
