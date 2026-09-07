#pragma once

// Приватные хелперы serve-слоя, разделяемые между несколькими translation
// units декомпозированного serve.cpp (сессия/очередь/персистентность/entry).
// Публичный интерфейс DaemonSession см. в serve.h.
namespace dsvc {

// Монотонное время в секундах (uptime-база и таймстампы сессии).
double monotonic_s();

}  // namespace dsvc
