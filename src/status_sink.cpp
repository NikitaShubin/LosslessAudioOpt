#include "status_sink.h"
#include "obs.h"

namespace {
StatusSink g_sink;
}

void install_status_sink() { obs::set_sink(&g_sink); }
