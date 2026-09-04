#pragma once
#include <atomic>
#include <string>

#include "config.h"

namespace tool {

struct Status {
    std::string path;    // пусто, если утилита не найдена
    std::string status;  // cache | path | downloaded | missing
    std::string message; // предупреждения (например, cli_check расходится)
};

// Обеспечивает доступность утилиты формата:
//   кэш bin/<id>/.binary -> PATH -> скачивание по downloads[].
// kill — опциональный флаг мгновенной остановки (см. proc::run).
Status ensure(const config::Format& fmt, bool download, const std::string& log_prefix = "",
              const std::atomic<bool>* kill = nullptr);

}  // namespace tool
