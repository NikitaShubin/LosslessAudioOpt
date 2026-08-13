#pragma once

#include <cstdio>
#include <string>

namespace out {

// Вывод готовой (уже локализованной) UTF-8 строки. Для консольного дескриптора
// используется WriteConsoleW (wide) — единственный путь, работающий и в
// настоящей консоли Windows, и в tty-консоли wine (узкий путь + CP_UTF8 там
// ломается). Для pipe/файла — сырые UTF-8 байты.
void text(FILE* f, const std::string& s);

// Локализованный вывод в stdout: ключ переводится через i18n, затем форматируется.
void print(const char* key, ...);

// То же в stderr.
void error(const char* key, ...);

// Сырые байты без локализации и без wide-преобразования (вывод внешних утилит).
void raw(FILE* f, const std::string& s);

}  // namespace out
