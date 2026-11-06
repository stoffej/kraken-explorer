/*
  Copyright (c) 2026 Schildkroet

  This file is part of Kraken Explorer.

  Kraken Explorer is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  Kraken Explorer is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Kraken Explorer.  If not, see <http://www.gnu.org/licenses/>.
*/

// REST API: HTTP/JSON control of the running app on 127.0.0.1 (`--api PORT`), for scripts
// and remote control (ssh tunnel for other hosts). Routes: docs/manual.md "REST API".

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

struct App;

struct RestApi
{
    std::jthread thread;
    std::atomic<bool> done{false}; // the thread has left its loop
    int fd = -1;                   // listening socket
    uint16_t port = 0;             // bound port (port 0 in rest_api_start picks a free one)
};

// Listens on 127.0.0.1:port; every request runs on the main thread through app.tasks.
bool rest_api_start(RestApi& api, App& app, uint16_t port);
// Stops the thread. Drains app.tasks meanwhile, so a request in flight still gets its reply.
void rest_api_stop(RestApi& api, App& app);

// One request -> JSON body, HTTP status in `status`. Main thread only (the server thread
// hands over through tasks_run_on_main_blocking).
[[nodiscard]] std::string rest_api_handle(App& app, std::string_view method, std::string_view path,
                                          std::string_view body, int& status);

// Value of "key" in a flat JSON object as text: strings unescaped, numbers / true / false as
// written, "" when missing. ponytail: flat objects only, no \uXXXX; nest when a route needs it.
[[nodiscard]] std::string json_get(std::string_view body, std::string_view key);

// s as a JSON string literal, quotes and escapes included.
[[nodiscard]] std::string json_string(std::string_view s);
