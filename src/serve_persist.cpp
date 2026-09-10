#include "serve.h"
#include "serve_internal.h"

#include <cstdio>
#include <mutex>
#include <vector>

#include "contract.h"
#include "persist.h"
#include "util.h"

namespace dsvc {

// Строка вне движка: всего строк в persist-файле может быть больше, чем
// вмещает движок (движок хранит только запускаемые). id строк, которые не
// попадают в движок, берутся из высокого диапазона, чтобы не пересекаться с
// монотонными idx движка (jobs растёт от 0).
static constexpr size_t kRestoredIdBase = 1ULL << 40;

std::vector<persist::Row> DaemonSession::snapshot_for_persist(bool final) const {
    // Только зеркало: ни движок, ни qm не трогаем. При наличии корня строки
    // хранят относительный от корня путь в label; path — абсолютный полный.
    // В файл пишется root + rel (label), чтобы очередь была независимой от cwd.
    std::vector<persist::Row> rows;
    for (const auto& r : st_->snapshot()) {
        persist::Row p;
        p.root = r.root;
        p.path = r.label.empty() ? r.path : r.label;
        if (p.path.empty()) continue;  // гонка add: строка ещё без пути — пропускаем
        p.mode = dsvc::mode_str(dsvc::parse_mode(r.mode));
        p.target_dir = r.target_dir;
        p.out_path = r.out_path;
        p.pct = r.pct;
        p.last_error = r.last_error;
        p.had_sidecar = r.had_sidecar;
        std::string st = r.state;
        if (final && (st == "queued" || st == "prep" || st == "running")) st = "queued";
        p.state = st;
        // has_sidecar: актуально только для ok и вычисляется по диску (живёт ли
        // sidecar рядом с итогом). Для остальных состояний — false.
        if (p.state == "ok" && !p.out_path.empty()) {
            p.has_sidecar = util::file_exists(persist::sidecar_path_for(p.out_path));
        }
        rows.push_back(std::move(p));
    }
    return rows;
}

void DaemonSession::persist_rows(const std::vector<persist::Row>& rows) {
    if (persist_path_.empty()) return;
    std::string err;
    if (!persist::write_file(persist_path_, rows)) {
        std::fprintf(stderr, "WARNING: could not persist queue to %s\n",
                     persist_path_.c_str());
    }
}

void DaemonSession::persist(bool final) {
    // Собственный мьютекс: persist() вызывается и из RPC-потоков (держащих
    // mt_), и из воркеров движка (держащих qm) — сериализуем запись здесь,
    // без вложенных блокировок движка/сессии.
    std::lock_guard<std::mutex> lk(persist_m_);
    if (shutting_down_.load() && !final) return;  // после shutdown — только финальный
    persist_rows(snapshot_for_persist(final));
}

void DaemonSession::set_had_sidecar(size_t id, const std::string& full_path) {
    bool had = util::file_exists(persist::sidecar_path_for(full_path));
    st_->set_sidecar_flags(id, had, false);
}

void DaemonSession::load_persisted(std::string* err) {
    if (persist_path_.empty()) return;
    std::vector<persist::Row> rows;
    if (!persist::read_file(persist_path_, &rows)) {
        // Нет файла или он не читается (v1/битый) — новый старт без очереди.
        // Не считаем это ошибкой: v1 и обрыв между записями здесь не фатальны.
        if (err) *err = std::string();
        return;
    }
    if (rows.empty()) return;
    size_t rid = kRestoredIdBase;
    std::vector<size_t> order;  // итоговый порядок строк в зеркале
    order.reserve(rows.size());

    auto restore_to_mirror = [&](const persist::Row& pr, const std::string& full,
                                 const std::string& st, const std::string& why) {
        dsvc::Row r;
        r.id = rid++;
        r.label = pr.path;      // относительный от корня (или старый полный)
        r.root = pr.root;
        r.path = full;          // абсолютный полный путь (для движка/валидации)
        r.state = st;
        r.pct = pr.pct;
        r.mode = dsvc::mode_str(dsvc::parse_mode(pr.mode));
        r.target_dir = pr.target_dir;
        r.out_path = pr.out_path;
        r.last_error = why;
        r.had_sidecar = pr.had_sidecar;
        r.has_sidecar = pr.has_sidecar;
        st_->upsert(r);
        added_paths_.insert(full);
        order.push_back(r.id);
    };

    // Полный путь исходника: при наличии root (новый формат) — join(root, rel);
    // без root (старый формат) — попытка абсолютизации относительно текущего
    // cwd; если не существует — исходная строка (строка уйдёт в stopped).
    auto full_path = [](const persist::Row& pr) -> std::string {
        if (!pr.root.empty()) return util::join_path(pr.root, pr.path);
        if (!util::path_is_absolute(pr.path)) {
            std::string a = util::abs_path(pr.path);
            if (util::file_exists(a) || util::dir_exists(a)) return a;
        }
        return pr.path;
    };

    // Валидация путей-источников (для не-ok строк): файл и, при необходимости,
    // его sidecar обязаны существовать на хосте демона.
    auto source_ok = [](const std::string& full, bool had_sidecar,
                        std::string* why) -> bool {
        if (!util::dir_exists(full) && !util::file_exists(full)) {
            *why = "исходный файл не найден при перезапуске: " + full;
            return false;
        }
        if (had_sidecar &&
            !util::file_exists(persist::sidecar_path_for(full))) {
            *why = "sidecar (теги) исходника не найден при перезапуске: " +
                   persist::sidecar_path_for(full);
            return false;
        }
        return true;
    };

    for (const auto& pr : rows) {
        std::string full = full_path(pr);
        if (pr.state == "queued") {
            // Продолжаем только файлы, до которых очередь ещё не дошла; все
            // проверки — как в add_locked. При любой неудаче строка остаётся
            // в зеркале со статусом stopped и причиной, в движок не заносится.
            std::string why;
            if (!source_ok(full, pr.had_sidecar, &why)) {
                restore_to_mirror(pr, full, "stopped", why);
                continue;
            }
            nlohmann::json tmp = {{"added", nlohmann::json::array()},
                                  {"rejected", nlohmann::json::array()}};
            std::vector<size_t> nids;
            add_locked({full}, pr.mode, pr.target_dir, tmp, nids);
            if (!nids.empty()) {
                // Строка попала в движок: движок уже эмитил begin_file/добавил
                // в зеркало. Позиция в порядке очереди — по persist-порядку.
                for (size_t nid : nids) order.push_back(nid);
                // Полный путь (из движка) в зеркало, а отображение/персист —
                // по сохранённому rel и корню (иначе rel заменился бы на имя
                // файла после повторного add).
                for (size_t nid : nids) {
                    auto snap = engine_->snapshot();
                    for (const auto& f : snap)
                        if (f.idx == nid && !f.path.empty()) {
                            st_->set_path(nid, f.path);
                            st_->set_label(nid, pr.path);
                            st_->set_root(nid, pr.root);
                            set_had_sidecar(nid, f.path);
                            break;
                        }
                }
                ev_->push("restored", {{"type", "queued"}, {"id", nids.front()},
                                       {"path", pr.path}});
                continue;
            }
            std::string reason =
                !tmp["rejected"].empty() && tmp["rejected"][0].contains("reason")
                    ? tmp["rejected"][0]["reason"].get<std::string>()
                    : "не удалось восстановить задачу из queue.json";
            restore_to_mirror(pr, full, "stopped", reason);
        } else if (pr.state == "ok") {
            // Итог проверяем по out_path (и по sidecar, если has_sidecar).
            std::string why;
            if (!pr.out_path.empty()) {
                if (!util::file_exists(pr.out_path))
                    why = "результат не найден при перезапуске: " + pr.out_path;
                else if (pr.has_sidecar &&
                         !util::file_exists(persist::sidecar_path_for(pr.out_path)))
                    why = "sidecar результата не найден при перезапуске: " +
                          persist::sidecar_path_for(pr.out_path);
            }
            restore_to_mirror(pr, full, "ok", why);
            ev_->push("restored", {{"type", "ok"}, {"path", pr.path}});
        } else if (pr.state == "prep" || pr.state == "running") {
            // Прервано во время обработки — подозрительная строка; в движок не
            // заносим, пользователь явно перезапустит через «Запустить».
            std::string why = "остановлено при перезапуске (обработка не завершена)";
            restore_to_mirror(pr, full, "stopped", why);
            ev_->push("restored", {{"type", "interrupted"}, {"path", pr.path}});
        } else {
            // stopped | error: сохраняем статус, но исходник + sidecar должны
            // существовать; иначе отмечаем причину (после перезапуска файл мог
            // исчезнуть — «Запустить» всё равно откажет без него).
            std::string why;
            if (!source_ok(full, pr.had_sidecar, &why)) {
                restore_to_mirror(pr, full, pr.state, why);
                continue;
            }
            restore_to_mirror(pr, full, pr.state, pr.last_error);
        }
    }

    // Итоговый порядок очереди по строкам из queue.json (движок-строки уже в
    // зеркале в порядке add; восстановленные — в порядке обхода).
    if (!order.empty()) st_->reorder(order);
    ev_->push("restored", {{"rows", (int)rows.size()}});
    if (err) *err = std::string();
    // Сразу пишем consolidated-состояние, чтобы в файле была одна актуальная
    // картина (новые id вместо персист-состояний) — но только если есть движок.
    persist(false);
}

}  // namespace dsvc
