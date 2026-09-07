#pragma once
#include <cstddef>
#include <string>
#include <vector>

// persist — персистентность состояния очереди демона.
//
// Файл queue.json (рядом с discovery-файлом: тот же каталог LLAO_DISCOVERY
// или <data home>/llao/) хранит РЕАЛЬНЫЕ статусы всех строк очереди:
//   { "version": 2, "rows": [ {path,mode,target_dir,state,pct,out_path,
//                               had_sidecar,has_sidecar,last_error}, ... ] }
// Порядок rows = порядок очереди (вид клиента). id строк НЕ хранятся:
// они назначаются заново при загрузке (см. serve.cpp).
//
// Два режима записи:
//   * промежуточный (persist(), инкрементный) — на мутациях сессии и из
//     obs::Sink-колбэков (prep/end_file/mark_stopped/error_file/mark_error);
//   * финальный (persist(true)) — первой строкой DaemonSession::shutdown(),
//     ДО engine_->shutdown(): active (queued|prep|running) -> "queued",
//     завершённые (ok|stopped|error) — как есть, и вся очередь достраивается
//     строками за пределами движка (восстановленные/ошибочные зеркальные).
//
// sidecar: <dir>/<base_no_ext> + ".tags.zip". В persist хранятся только
// булевы had_sidecar (был ли на входе оригинальный sidecar) и has_sidecar
// (есть ли рядом с итоговым файлом sidecar теперь).
namespace persist {

// Одна строка сохраняемого состояния очереди.
struct Row {
    std::string path;         // полный путь исходника (на хосте демона)
    std::string mode;         // "optimize" | "restore"
    std::string target_dir;   // целевая папка (пусто = замена на месте)
    std::string state;        // ok | stopped | error | queued | prep | running
    double pct = 0;           // выигрыш в сжатии (после ok)
    std::string out_path;     // фактический путь результата (после ok)
    bool had_sidecar = false; // на входе был sidecar <base>.tags.zip
    bool has_sidecar = false; // рядом с итогом есть sidecar
    std::string last_error;   // текст последней ошибки (error/stopped)
};

// Путь к sidecar для исходника path: <dir>/<base_no_ext(path)>.tags.zip.
std::string sidecar_path_for(const std::string& path);

// Сериализация всего состояния очереди (rows — в порядке очереди) в JSON.
std::string to_json(const std::vector<Row>& rows);

// Десериализация. Возвращает false при некорректном документе (не version 2
// или битый JSON) — тогда содержимое игнорируется.
bool from_json(const std::string& text, std::vector<Row>* rows);

// Атомарная запись: JSON рядом с <path>.tmp, затем rename поверх <path>
// (ретраи на случай антивируса). false при ошибке.
bool write_file(const std::string& path, const std::vector<Row>& rows);

// Чтение (пусто/rows пуст при отсутствии файла или ошибке; флаг вернёт false,
// если файл был, но не читается как корректный state v2).
bool read_file(const std::string& path, std::vector<Row>* rows);

}  // namespace persist