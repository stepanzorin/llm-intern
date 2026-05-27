// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <filesystem>

namespace stz::intern::util {

[[nodiscard]] std::filesystem::path application_directory_path();

void ensure_parent_directory_exists(const std::filesystem::path &filename);

} // namespace stz::intern::util