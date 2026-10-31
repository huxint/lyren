#pragma once
#include <functional>
#include <type_traits>
#include <utility>

#include "core/result.hpp"
#include "core/task.hpp"

namespace lyren {

    // Keeps the continuation alive until its coroutine finishes, including coroutine lambda captures.
    template <class T, class F>
    auto and_then(Result<T> result, F next) -> std::invoke_result_t<F, T> {
        if (!result) {
            co_return std::unexpected(std::move(result).error());
        }
        co_return co_await std::invoke(next, *std::move(result));
    }

    template <class F>
    auto and_then(Result<void> result, F next) -> std::invoke_result_t<F> {
        if (!result) {
            co_return std::unexpected(std::move(result).error());
        }
        co_return co_await std::invoke(next);
    }

    template <class T, class F>
    auto and_then(Task<Result<T>> task, F next) -> decltype(and_then(std::declval<Result<T>>(), std::declval<F>())) {
        co_return co_await and_then(co_await std::move(task), std::move(next));
    }

} // namespace lyren
