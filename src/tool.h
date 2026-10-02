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

// Проверка готовности утилиты формата (кэш/PATH + cli_check.expect из
// formats/*.json). Возвращает список проблем; пустой результат — готово.
// Не скачивает и не изменяет состояние (полезно для стартового гейта сервера).
std::string check_config(const config::Format& fmt, const std::atomic<bool>* kill = nullptr);

// Что произошло при обновлении одного кодека.
struct UpdateResult {
    std::string id;
    std::string status;      // unchanged | updated | failed | skipped
    std::string from;        // прежняя версия/хэш, если известны
    std::string to;
    std::string message;     // причина для failed/skipped и предупреждения
};

// Обновляет кодеки до последней доступной версии.
//
// Работает по рецепту `latest` из formats/<id>.json: он отличается от закреплённого
// (`pinned`) тем, что не несёт checksum — версия ещё не проверена. Каждый шаг:
// скачать, сверить cli_check.expect, при расхождении справки вернуть прежний
// бинарник, иначе перезаписать закреплённый рецепт в formats/<id>.json новым
// url и посчитанным sha256.
//
// checksum в pinned-записи обязателен после обновления: без него бинарник,
// скачанный из сети, ничем не отличается от подделанного. Если посчитать
// хэш не удалось, запись не трогается и возвращается failed.
std::vector<UpdateResult> update_codecs(const std::vector<config::Format>& fmts,
                                        const std::atomic<bool>* kill = nullptr);

}  // namespace tool
