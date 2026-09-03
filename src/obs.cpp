#include "obs.h"

namespace obs {

namespace {
Sink* g_sink = nullptr;
}

Sink* sink() { return g_sink; }

void set_sink(Sink* s) { g_sink = s; }

}  // namespace obs
