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

// REST routes split by area so they can grow independently; rest_api_handle tries each in turn.
// Each returns the JSON reply (status set) for a route it owns, std::nullopt for any other path.
// Main thread only, like rest_api_handle.

#pragma once

#include <optional>
#include <string>
#include <string_view>

struct App;

// Interfaces and setup: bitrates, FD, listen-only, LIN, LDF, link up/down.
std::optional<std::string> rest_setup_routes(App& app, std::string_view method, std::string_view path,
                                             std::string_view body, int& status);
// Replay, trace filter, value search hits / raw search.
std::optional<std::string> rest_replay_routes(App& app, std::string_view method, std::string_view path,
                                              std::string_view body, int& status);
// Generator, macros, scripts, workspaces, graph export, convert.
std::optional<std::string> rest_control_routes(App& app, std::string_view method, std::string_view path,
                                               std::string_view body, int& status);
