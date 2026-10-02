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
        // Отрисовка вариантов переживает рестарт: без этого восстановленная
        // строка теряла точки вариантов и кольцо победителя.
        p.tasks = r.tasks;
        for (const auto& ti : r.task_infos)
            p.task_infos.push_back({ti.fmt_id, ti.variant_id, ti.note});
        for (const auto& v : r.excluded)
            p.excluded.push_back({v.fmt_id, v.variant_id, v.reason});
        if (r.has_winner) {
            p.has_winner = true;
            p.winner_fmt = r.winner_fmt;
            p.winner_variant = r.winner_variant;
        }
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
    // Пока идёт восстановление очереди из queue.json, файл не переписывается.
    // Восстановление добавляет строки по одной, а persist() к тому моменту
    // вызывается уже и воркерами движка (файл начали обрабатывать), и с
    // RPC-потоков. Любая из этих записей сохраняла бы в queue.json только
    // восстановленный на этот момент префикс — а если демон в этот миг падает,
    // префикс остаётся в файле навсегда: при следующем старте демон
    // восстанавливает уже усечённую очередь, и остальные строки исчезают
    // бесследно. На реальной библиотеке это стоило 4 745 строк из 5 256.
    if (reloading_ && !final) return;
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

    // Прежний queue.json сохраняем рядом: перезапись начнётся сразу после
    // восстановления, и если что-то пойдёт не так (обрыв, битая запись),
    // исходный состав очереди останется доступен для ручного возврата.
    {
        std::error_code ec;
        util::copy_file(persist_path_, persist_path_ + ".bak");
        (void)ec;
    }

    reloading_ = true;
    struct ReloadGuard {
        bool* flag;
        ~ReloadGuard() { *flag = false; }
    } reload_guard{&reloading_};

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
        r.tasks = pr.tasks;
        for (const auto& ti : pr.task_infos)
            r.task_infos.push_back({ti.fmt, ti.variant, {}, ti.note});
        for (const auto& s : pr.excluded)
            r.excluded.push_back({s.fmt, s.variant, s.reason});
        if (pr.has_winner) {
            r.has_winner = true;
            r.winner_fmt = pr.winner_fmt;
            r.winner_variant = pr.winner_variant;
            // Индекс победителя не сохраняем: после рестарта он не нужен —
            // веб находит точку по паре формат/вариант.
            r.winner_task = SIZE_MAX;
        }
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

    // Очередь восстанавливается пачками, а не по одной строке: add_locked на
    // каждую строку заново проверяет путь, эмитит события и пишет в зеркало, а
    // движок на 5+ тысяч строк так не восстановить — reload занимал минуты, и
    // всё это время файл очереди оставался незаписанным. Пачка собирается из
    // подряд идущих строк с одинаковыми mode/target_dir (это и есть add-ка,
    // которым их и добавляли), чтобы порядок в очереди сохранился.
    std::vector<std::string> batch_paths;
    std::vector<const persist::Row*> batch_rows;  // параллельно batch_paths
    std::string batch_mode, batch_target;
    auto flush_batch = [&]() {
        if (batch_paths.empty()) return;
        nlohmann::json tmp = {{"added", nlohmann::json::array()},
                              {"rejected", nlohmann::json::array()}};
        std::vector<size_t> nids;
        add_locked(batch_paths, batch_mode, batch_target, tmp, nids);
        for (size_t nid : nids) order.push_back(nid);
        // Полный путь (из движка) в зеркало, а отображение/персист — по
        // сохранённому rel и корню (иначе rel заменился бы на имя файла, и
        // очередь перестала бы быть независимой от cwd).
        if (!nids.empty()) {
            auto snap = engine_->snapshot();
            for (size_t i = 0; i < nids.size() && i < batch_rows.size(); i++) {
                const size_t nid = nids[i];
                for (const auto& f : snap)
                    if (f.idx == nid && !f.path.empty()) {
                        st_->set_path(nid, f.path);
                        break;
                    }
                st_->set_label(nid, batch_rows[i]->path);
                st_->set_root(nid, batch_rows[i]->root);
                set_had_sidecar(nid, batch_paths[i]);
            }
        }
        batch_paths.clear();
        batch_rows.clear();
    };

    for (const auto& pr : rows) {
        std::string full = full_path(pr);
        if (pr.state == "queued") {
            // Продолжаем только файлы, до которых очередь ещё не дошла; все
            // проверки — как в add_locked. При любой неудаче строка остаётся
            // в зеркале со статусом stopped и причиной, в движок не заносится.
            std::string why;
            if (!source_ok(full, pr.had_sidecar, &why)) {
                flush_batch();
                restore_to_mirror(pr, full, "stopped", why);
                continue;
            }
            if (batch_paths.empty()) {
                batch_mode = pr.mode;
                batch_target = pr.target_dir;
            } else if (pr.mode != batch_mode || pr.target_dir != batch_target) {
                flush_batch();
                batch_mode = pr.mode;
                batch_target = pr.target_dir;
            }
            batch_paths.push_back(full);
            batch_rows.push_back(&pr);
            continue;
        }
        flush_batch();
        if (pr.state == "ok") {
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
    flush_batch();

    // Итоговый порядок очереди по строкам из queue.json (движок-строки уже в
    // зеркале в порядке add; восстановленные — в порядке обхода).
    if (!order.empty()) st_->reorder(order);
    ev_->push("restored", {{"rows", (int)rows.size()}});
    if (err) *err = std::string();
    // Сразу пишем consolidated-состояние, чтобы в файле была одна актуальная
    // картина (новые id вместо персист-состояний) — но только если есть движок.
    // Флаг reloading_ к этому моменту уже снят (ReloadGuard), иначе запись
    // была бы подавлена вместе со всеми остальными.
    persist(false);
}

}  // namespace dsvc
