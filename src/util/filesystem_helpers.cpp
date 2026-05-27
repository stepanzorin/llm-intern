#include "filesystem_helpers.hpp"

#include <format>
#include <stdexcept>
#include <system_error>

namespace stz::intern::util {

std::filesystem::path application_directory_path() { return {STZ_INTERN_APPLICATION_DIR_PATH}; }

void ensure_parent_directory_exists(const std::filesystem::path &filename) {
    const auto parent = filename.parent_path();

    if (parent.empty()) {
        return;
    }

    auto error = std::error_code{};
    std::filesystem::create_directories(parent, error);

    if (error) {
        throw std::runtime_error{std::format("Failed to create directory '{}': {}", parent.string(), error.message())};
    }
}

} // namespace stz::intern::util