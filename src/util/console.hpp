// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <string>

namespace stz::intern::util {

void configure_console();

[[nodiscard]] bool read_console_line_utf8(std::string &line);

} // namespace stz::intern::util