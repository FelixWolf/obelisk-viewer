#pragma once

// Stateless logging helpers. Deliberately not a logger object: there is no
// global instance to configure or tear down. If we later need sinks (file,
// in-viewer console), they will be passed explicitly to whoever needs them.

#include <format>
#include <print>
#include <cstdio>

namespace viewer::log {

template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args)
{
    std::println(stderr, "[info] {}", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args)
{
    std::println(stderr, "[warn] {}", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args)
{
    std::println(stderr, "[error] {}", std::format(fmt, std::forward<Args>(args)...));
}

} // namespace viewer::log
