#include <cstdio>

#include "obs.h"
#include "out.h"

// LinearSink: линейный (построчный) приёмник событий движка для headless-режима.
// log -> stdout, error/error_file -> stderr; прогресс-события игнорируются.
// Используется CLI-режимом (optimize/restore) — статусбар отсутствует.

class LinearSink final : public obs::Sink {
public:
    void error_file(size_t /*idx*/, const std::string& reason) override {
        out::text(stderr, reason);
    }
    void log(const std::string& line) override { out::text(stdout, line); }
    void error(const std::string& line) override { out::text(stderr, line); }
};

// Устанавливает глобальный LinearSink. Вызывается CLI-режимом до старта воркеров.
void install_linear_sink();