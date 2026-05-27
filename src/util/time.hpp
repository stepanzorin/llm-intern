// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <string>

namespace stz::intern::util {

constexpr inline auto dmy_hms_format = "%d/%m/%Y %H:%M:%S";

[[nodiscard]] std::string make_local_timestamp();

} // namespace stz::intern::util