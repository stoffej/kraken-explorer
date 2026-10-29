/*

  Copyright (c) 2026 The Kraken Explorer contributors

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


#pragma once

// std::move_only_function where the standard library has it (libstdc++, MSVC). Apple's libc++
// does not (Xcode 26), so there this is a minimal stand-in: move-only, non-const call, heap
// storage. ponytail: no small-buffer optimisation or cv/ref/noexcept qualifiers; drop the
// fallback once Apple's libc++ ships std::move_only_function.

#include <functional>

#ifdef __cpp_lib_move_only_function

template <typename Signature>
using MoveOnlyFunction = std::move_only_function<Signature>;

#else

#ifndef _LIBCPP_VERSION
#error "std::move_only_function missing: compile this translation unit as C++23 (kraken_tasks exports it PUBLIC)"
#endif

#include <memory>
#include <type_traits>
#include <utility>

template <typename Signature>
class MoveOnlyFunction;

template <typename R, typename... Args>
class MoveOnlyFunction<R(Args...)>
{
    // The callable on the heap, with a deleter and a caller that know its type.
    std::unique_ptr<void, void (*)(void*)> target{nullptr, nullptr};
    R (*call)(void*, Args&&...) = nullptr;

public:
    MoveOnlyFunction() noexcept = default;
    MoveOnlyFunction(std::nullptr_t) noexcept {}

    template <typename F, typename T = std::decay_t<F>>
        requires(!std::is_same_v<T, MoveOnlyFunction> && std::is_invocable_r_v<R, T&, Args...>)
    MoveOnlyFunction(F&& fn)
        : target(new T(std::forward<F>(fn)), [](void* p) { delete static_cast<T*>(p); }),
          call([](void* p, Args&&... args) -> R { return std::invoke(*static_cast<T*>(p), std::forward<Args>(args)...); })
    {
    }

    MoveOnlyFunction(MoveOnlyFunction&&) noexcept = default;
    MoveOnlyFunction& operator=(MoveOnlyFunction&&) noexcept = default;

    R operator()(Args... args) { return call(target.get(), std::forward<Args>(args)...); }
    explicit operator bool() const noexcept { return target != nullptr; }
};

#endif
