#pragma once

#include <cstdarg>
#include <string>

namespace i18n {

// Разрешение языка (флаг --lang > env LLAO_LANG > llao.json > автоопределение)
// и загрузка каталога lang/<code>.json (если язык не английский). Вызывать
// первой в main() — до любого вывода и до команд, которые могут бросать.
// code должен быть "ru", "en" или "auto"; settings_lang — значение "language"
// из llao.json ("auto" по умолчанию).
void init(const std::string& cli_flag, const std::string& settings_lang);

// Выбранный код языка: "en" | "ru" | ...
std::string code();

// Локализованная строка по английскому ключу; если ключа нет в каталоге —
// возвращается сам ключ (английский — база).
std::string str(const std::string& key);

// Форматирование строки printf-стилем (работает и с MSVCRT, где vsnprintf
// возвращает -1 при нехватке буфера).
std::string vfmt(const std::string& fmt, va_list ap);

// Локализованная строка с printf-форматированием.
std::string fmt(const char* key, ...);

}  // namespace i18n
