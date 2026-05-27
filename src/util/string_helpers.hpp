// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <string>
#include <string_view>

namespace stz::intern::util {

namespace detail {

inline constexpr auto *default_whitespace = " \t\n\r\f\v";

} // namespace detail

std::string &ltrim(std::string &s, const char *t = detail::default_whitespace);

std::string &rtrim(std::string &s, const char *t = detail::default_whitespace);

std::string &trim(std::string &s, const char *t = detail::default_whitespace);

[[nodiscard]] std::string trimmed(std::string s, const char *t = detail::default_whitespace);

[[nodiscard]] bool is_blank(std::string_view text) noexcept;

} // namespace stz::intern::util