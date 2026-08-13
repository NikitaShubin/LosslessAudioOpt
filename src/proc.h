#pragma once
#include <string>
#include <vector>

namespace proc {

struct Result {
    bool started = false;    // процесс создан
    bool timed_out = false;  // превышен таймаут, процесс завершён принудительно
    int exit_code = -1;
    std::string output;      // stdout+stderr (объединено)
    std::string error;       // текст ошибки при неудачном запуске
};

// Запуск процесса без шелла. args[0] — исполняемый файл (путь или имя из PATH).
// timeout_sec == 0 — без ограничения времени. cwd — рабочий каталог (пусто = текущий).
Result run(const std::vector<std::string>& args, int timeout_sec = 0, const std::string& cwd = "");

}  // namespace proc
