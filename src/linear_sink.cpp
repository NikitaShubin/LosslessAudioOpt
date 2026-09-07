#include "linear_sink.h"
#include "obs.h"
#include "out.h"

namespace {
LinearSink g_sink;
}

void install_linear_sink() { obs::set_sink(&g_sink); }