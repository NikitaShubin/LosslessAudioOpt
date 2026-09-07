#include "obs.h"

namespace obs {

namespace {
Sink* g_sink = nullptr;
// Нулевой приёмник: до установки боевого sink (start-up, CLI-проверки)
// движок должен получать валидный объект, а не nullptr.
Sink& dummy() {
    static Sink d;
    return d;
}
}  // namespace

Sink* sink() { return g_sink ? g_sink : &dummy(); }

void set_sink(Sink* s) { g_sink = s; }

}  // namespace obs
