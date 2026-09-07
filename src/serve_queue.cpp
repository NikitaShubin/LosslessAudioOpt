#include "serve.h"
#include "serve_internal.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "contract.h"
#include "util.h"

namespace dsvc {

namespace {

bool is_done_state(const std::string& st) {
    return st == "ok" || st == "stopped" || st == "error" || st == "removed";
}

}  // namespace

void DaemonSession::add_locked(const std::vector<std::string>& paths,
                               const std::string& mode, const std::string& target_dir,
                               nlohmann::json& result, std::vector<size_t>& new_ids) {
    bool mode_ok = false;
    optimize::JobMode jm = dsvc::parse_mode(mode, &mode_ok);
    if (!mode_ok) {
        result["rejected"].push_back(
            {{"path", "<mode>"}, {"reason", "mode must be optimize|restore"}});
        return;
    }
    bool have_target = !target_dir.empty();
    if (have_target) {
        // Целевая папка: должна существовать на хосте демона заранее. Исходная
        // структура пачки воспроизводится внутри неё, файлы вне её каталогами
        // не нуждаются в предварительном создании (mkdirs при записи).
        if (!util::dir_exists(target_dir)) {
            result["rejected"].push_back(
                {{"path", "<target_dir>"}, {"reason", "target folder not found"}});
            return;
        }
    }
    if (jm == optimize::JobMode::Restore) {
        if (restore_to_.empty()) {
            result["rejected"].push_back(
                {{"path", "<mode>"}, {"reason", "restore: target format not configured"}});
            return;
        }
        bool known = false;
        for (const auto& f : formats_cache_)
            if (f.contains("id") && f["id"] == restore_to_) { known = true; break; }
        if (!known) {
            result["rejected"].push_back(
                {{"path", "<mode>"},
                 {"reason", "restore: target format \"" + restore_to_ + "\" not found"}});
            return;
        }
    }
    std::vector<std::string> accepted;
    for (const auto& raw : paths) {
        std::string p = dsvc::normalize_path(raw);
        if (p.empty()) {
            result["rejected"].push_back({{"path", raw}, {"reason", "empty path"}});
            continue;
        }
        bool is_dir = util::dir_exists(p);
        if (!is_dir && !util::file_exists(p)) {
            result["rejected"].push_back(
                {{"path", raw},
                 {"reason", "path not found on daemon host (use --upload in client)"}});
            continue;
        }
        if (added_paths_.count(p)) {
            // Если путь был удалён/завершён — разрешить повторное добавление
            bool still_active = false;
            if (engine_) {
                auto snap = engine_->snapshot();
                for (const auto& f : snap) if (f.path == p) {
                    if (f.state == "queued" || f.state == "prep" || f.state == "running") still_active = true;
                    break;
                }
            }
            if (still_active) {
                result["rejected"].push_back({{"path", raw}, {"reason", "already in queue"}});
                continue;
            } else {
                added_paths_.erase(p);
            }
        }
        added_paths_.insert(p);
        accepted.push_back(p);
    }
    if (!accepted.empty() && engine_) {
        optimize::AddOptions ao;
        ao.mode = jm;
        ao.target_dir = target_dir;
        ao.to = (jm == optimize::JobMode::Restore) ? restore_to_ : std::string();
        new_ids = engine_->add(accepted, ao);
    }
}

void DaemonSession::add(const std::vector<std::string>& paths,
                        const std::string& mode, const std::string& target_dir,
                        nlohmann::json& result) {
    // Вся приёмка и само добавление — под mt_ (см. комментарий в serve.h).
    // Ответ строится по idx, возвращённым движком, а не по размеру зеркала.
    std::vector<size_t> new_ids;
    {
        std::lock_guard<std::mutex> lk(mt_);
        add_locked(paths, mode, target_dir, result, new_ids);
    }
    bool need_resume = !new_ids.empty() && paused_.load();
    if (need_resume) set_paused(false);

    // begin_file синхронен (append_files эмитит события до возврата),
    // поэтому строки уже в зеркале — ищем по id, без эвристики хвоста.
    auto rows = st_->snapshot();
    // Полные пути новых строк (из движка): для персистентности строка должна
    // знать исходный путь; label — это только rel (относительный).
    std::unordered_map<size_t, std::string> id2path;
    if (engine_) {
        for (const auto& f : engine_->snapshot()) id2path[f.idx] = f.path;
    }
    for (size_t id : new_ids) {
        for (const auto& r : rows)
            if (r.id == id) {
                result["added"].push_back({{"id", r.id}, {"label", r.label}});
                auto pit = id2path.find(id);
                if (pit != id2path.end() && !pit->second.empty()) {
                    st_->set_path(id, pit->second);
                    set_had_sidecar(id, pit->second);
                }
                break;
            }
    }
    persist(false);
}

bool DaemonSession::cancel_file(uint64_t id) {
    if (!engine_) return false;
    std::lock_guard<std::mutex> lk(mt_);
    auto snap = engine_->snapshot();
    for (const auto& f : snap)
        if (f.idx == (size_t)id) {
            bool active = !(f.state == "ok" || f.state == "stopped" ||
                            f.state == "error" || f.state == "removed");
            engine_->remove((size_t)id);
            if (active) {
                ev_->push_for(id, "stopped");
                st_->set_state(id, "stopped");
            }
            persist(false);
            return true;
        }
    return false;
}

bool DaemonSession::remove_locked(uint64_t id) {
    std::string path_to_remove;
    bool found = false;
    bool in_engine = false;
    {
        auto snap_before = engine_ ? engine_->snapshot() : std::vector<optimize::EngineFile>();
        for (const auto& f : snap_before)
            if (f.idx == (size_t)id) {
                found = true;
                in_engine = true;
                path_to_remove = f.path;
                break;
            }
    }
    if (!found) {
        // Строка вне движка (восстановленная из queue.json): удаляем из зеркала.
        auto rows = st_->snapshot();
        for (const auto& r : rows)
            if (r.id == (size_t)id) {
                found = true;
                path_to_remove = r.path.empty() ? r.label : r.path;
                break;
            }
    }
    if (!found) return false;
    if (in_engine) engine_->remove((size_t)id);
    st_->remove((size_t)id);
    if (!path_to_remove.empty()) added_paths_.erase(path_to_remove);
    ev_->push_for(id, "removed");
    persist(false);
    return true;
}

bool DaemonSession::remove(uint64_t id) {
    std::lock_guard<std::mutex> lk(mt_);
    return remove_locked(id);
}

uint64_t DaemonSession::bulk_remove(const std::vector<size_t>& ids) {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    uint64_t removed = 0;
    for (size_t id : ids)
        if (remove_locked(id)) removed++;
    return removed;
}

uint64_t DaemonSession::bulk_cancel(const std::vector<size_t>& ids) {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    auto snap = engine_->snapshot();
    uint64_t cancelled = 0;
    for (size_t id : ids) {
        bool active = false;
        for (const auto& f : snap)
            if (f.idx == id) {
                active = !(f.state == "ok" || f.state == "stopped" ||
                           f.state == "error" || f.state == "removed");
                break;
            }
        if (!active) continue;
        engine_->remove(id);
        ev_->push_for(id, "stopped");
        st_->set_state(id, "stopped");
        cancelled++;
    }
    persist(false);
    return cancelled;
}

uint64_t DaemonSession::cancel_all_active() {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    auto snap = engine_->snapshot();
    uint64_t cancelled = 0;
    for (const auto& f : snap) {
        bool active = !(f.state == "ok" || f.state == "stopped" ||
                        f.state == "error" || f.state == "removed");
        if (!active) continue;
        engine_->remove(f.idx);
        ev_->push_for(f.idx, "stopped");
        st_->set_state(f.idx, "stopped");
        cancelled++;
    }
    persist(false);
    return cancelled;
}

size_t DaemonSession::sort_by_path() {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    auto snap = engine_->snapshot();
    std::unordered_map<size_t, size_t> pos;
    pos.reserve(snap.size());
    for (size_t k = 0; k < snap.size(); k++) pos[snap[k].idx] = k;
    std::vector<size_t> order;
    order.reserve(snap.size());
    for (const auto& f : snap) order.push_back(f.idx);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const std::string& pa = snap[pos[a]].path;
        const std::string& pb = snap[pos[b]].path;
        if (pa.empty()) {
            if (pb.empty()) return false;
            return false;  // пустые пути — в конец
        }
        if (pb.empty()) return true;
        return pa < pb;
    });
    if (!engine_->reorder(order)) return 0;
    st_->reorder(order);
    ev_->push("reordered", {{"order", order}});
    persist(false);
    return order.size();
}

uint64_t DaemonSession::clear_done() {
    if (!engine_) return 0;
    std::lock_guard<std::mutex> lk(mt_);
    auto rows = st_->snapshot();
    uint64_t removed = 0;
    for (const auto& r : rows)
        if (r.state == "ok" && remove_locked(r.id)) removed++;
    persist(false);
    return removed;
}

bool DaemonSession::restart(uint64_t id) {
    if (!engine_) return false;
    // Универсальный перезапуск: активный файл сначала останавливается
    // (cancel + мгновенный kill процессов), ожидается завершение. Затем
    // НОВАЯ строка добавляется СНАЧАЛА, и только после успешного добавления
    // удаляется старая. Так исключено «исчезновение файла из списка» даже
    // когда старая строка уже была автоубрана движком после отмены.
    // Восстановленные из queue.json строки (вне движка, id из высокого
    // диапазона) перезапускаются без движковых операций: только пересоздание
    // зеркальной строки через add_locked.
    std::string path;
    std::string mode, target_dir;  // режим и целевая папка исходной строки
    bool was_active = false;
    bool in_engine = false;
    {
        auto snap = engine_->snapshot();
        for (const auto& f : snap)
            if (f.idx == (size_t)id) {
                in_engine = true;
                path = f.path;
                mode = f.mode;
                target_dir = f.target_dir;
                was_active = !is_done_state(f.state);
                break;
            }
        if (!in_engine) {
            // Строка вне движка: параметры — из зеркала (label = полный путь).
            auto rows = st_->snapshot();
            for (const auto& r : rows)
                if (r.id == (size_t)id) {
                    path = r.path.empty() ? r.label : r.path;
                    mode = r.mode;
                    target_dir = r.target_dir;
                    break;
                }
        }
        if (path.empty()) return false;
        if (in_engine && was_active) engine_->remove((size_t)id);  // cancel + kill, без зеркала
        if (in_engine && was_active) {
            bool settled = false;
            for (int i = 0; i < 200; i++) {
                auto s2 = engine_->snapshot();
                for (const auto& f : s2)
                    if (f.idx == (size_t)id && is_done_state(f.state)) {
                        settled = true;
                        break;
                    }
                if (settled) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!settled) return false;
        } else if (in_engine) {
            // Готовая строка (ok/stopped/error): процессов нет, но повторное
            // добавление спотыкается о seen_paths_ движка, поэтому старую строку
            // убираем СРАЗУ (до add); позицию восстановим в хвосте restart.
            {
                std::lock_guard<std::mutex> lk(mt_);
                auto s2 = engine_->snapshot();
                for (const auto& f : s2)
                    if (f.idx == (size_t)id) {
                        engine_->remove((size_t)id);
                        break;
                    }
            }
        }
    }
    // Исходник должен существовать: после успешной обработки (ok) файл
    // заменён другим форматом, перезапускать нечего.
    if (!util::dir_exists(path) && !util::file_exists(path)) return false;
    std::lock_guard<std::mutex> lk(mt_);
    nlohmann::json tmp = {{"added", nlohmann::json::array()},
                          {"rejected", nlohmann::json::array()}};
    std::vector<size_t> new_ids;
    // Перезапуск сохраняет режим и целевую папку исходной строки (optimize
    // остаётся optimize, restore — restore в ту же папку), иначе семантика
    // задания изменилась бы «под ногами» пользователя.
    add_locked({path}, mode, target_dir, tmp, new_ids);
    if (new_ids.empty()) return false;  // старую строку не трогаем
    size_t new_id = new_ids.front();
    // Полный путь новой строки — из движка (label — только rel). Без него
    // snapshot_for_persist() уйдёт в label, и после перезапуска демона с
    // другой cwd путь в queue.json окажется неверным.
    {
        auto snap = engine_->snapshot();
        for (const auto& f : snap)
            if (f.idx == new_id && !f.path.empty()) {
                st_->set_path(new_id, f.path);
                set_had_sidecar(new_id, f.path);
                break;
            }
    }
    // Позицию старой строки берём из ВИДИМОГО списка (зеркало) в текущий
    // момент — она уже учитывает все удалённые/завершённые строки. Позиция в
    // движке может отличаться (движок хранит и «зомби»-строки), поэтому
    // движок переупорядочиваем по его же позиции.
    size_t mirror_pos = SIZE_MAX;
    size_t engine_pos = SIZE_MAX;
    {
        auto rows = st_->snapshot();
        for (size_t k = 0; k < rows.size(); k++)
            if (rows[k].id == (size_t)id) {
                mirror_pos = k;
                break;
            }
        if (in_engine) {
            auto snap = engine_->snapshot();
            for (size_t k = 0; k < snap.size(); k++)
                if (snap[k].idx == (size_t)id) {
                    engine_pos = k;
                    break;
                }
        }
    }
    // Старая строка может отсутствовать (автоуборка после отмены) — не ошибка.
    if (in_engine) {
        auto snap = engine_->snapshot();
        for (const auto& f : snap)
            if (f.idx == (size_t)id) {
                remove_locked(id);
                break;
            }
    } else {
        // Вне движка: старую зеркальную строку снимаем напрямую (remove_locked
        // умеет и такие), чтобы не было дубля после повторного add.
        auto rows = st_->snapshot();
        for (const auto& r : rows)
            if (r.id == (size_t)id) {
                remove_locked(r.id);
                break;
            }
    }
    // Восстанавливаем прежнюю позицию строки, чтобы перезапуск не выглядел
    // как «файл удалён из списка» и не ломал ручную перетасовку очереди.
    // Зеркало (видимый список) и движок переупорядочиваются отдельно: движок
    // хранит отменённые строки-«зомби» (idx, которые из зеркала уже ушли),
    // поэтому подавать туда зеркальный порядок нельзя.
    {
        auto rows = st_->snapshot();
        std::vector<size_t> morder;
        morder.reserve(rows.size());
        for (const auto& r : rows)
            if (r.id != new_id) morder.push_back(r.id);
        if (!morder.empty()) {
            size_t goal = mirror_pos != SIZE_MAX && mirror_pos < morder.size()
                              ? mirror_pos
                              : morder.size();
            morder.insert(morder.begin() + goal, new_id);
            if (st_->reorder(morder)) ev_->push("reordered", {{"order", morder}});
        }
        if (in_engine) {
            auto snap = engine_->snapshot();
            std::vector<size_t> eorder;
            eorder.reserve(snap.size());
            for (const auto& f : snap)
                if (f.idx != new_id) eorder.push_back(f.idx);
            if (!eorder.empty()) {
                size_t goal = engine_pos != SIZE_MAX && engine_pos < eorder.size()
                                  ? engine_pos
                                  : eorder.size();
                eorder.insert(eorder.begin() + goal, new_id);
                engine_->reorder(eorder);
            }
        }
    }
    // Ручной перезапуск при остановленной очереди должен снимать паузу,
    // иначе файл добавится и останется в «queued» навсегда.
    if (paused_.load()) set_paused(false);
    persist(false);
    return true;
}

bool DaemonSession::reorder(const std::vector<size_t>& order) {
    if (!engine_) return false;
    bool ok = engine_->reorder(order);
    if (ok) {
        st_->reorder(order);
        ev_->push("reordered", {{"order", order}});
        persist(false);
    }
    return ok;
}

}  // namespace dsvc
