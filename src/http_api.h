#pragma once
#include <string>

#include "events.h"
#include "rpc.h"

// http_api — обработчики cpp-httplib для демона: авторизация Bearer,
// POST /rpc, GET /api/state, /api/events, /api/formats, GET / (страница логина).
// Монтируется на httplib::Server в serve.cpp. Чтобы не тащить httplib во все
// переводные единицы, объявление не зависит от httplib (только forward-decl).
namespace httplib {
class Server;
}

namespace dsvc {

// Контекст API: реализация Daemon (rpc+formats+counters), буфер событий,
// зеркало состояния и токен авторизации (пустой — авторизация выключена, dev).
struct ApiContext {
    Daemon* daemon = nullptr;
    EventBuffer* events = nullptr;
    StateMirror* state = nullptr;
    std::string token;  // Bearer-токен; пусто = без ограничений
};

// Монтирует обработчики на сервер. Возвращает 0 при успехе, иначе код ошибки.
int mount(httplib::Server& svr, const ApiContext& ctx);

}  // namespace dsvc
