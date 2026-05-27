#include "file_io.hpp"

#include <array>
#include <cassert>
#include <format>

#include "platform.hpp"
#include "util/filesystem_helpers.hpp"

namespace stz::intern::util {

namespace {

[[nodiscard]] FILE *open_file(const std::filesystem::path &filename, const char *mode) {
#ifdef STZ_INTERN_PLATFORM_WINDOWS
    wchar_t wide_mode[8]{};

    for (auto i = 0; i < 7 && mode[i] != '\0'; ++i) {
        wide_mode[i] = static_cast<wchar_t>(mode[i]);
    }

    return _wfopen(filename.wstring().c_str(), wide_mode);
#else
    return std::fopen(filename.string().c_str(), mode);
#endif
}

[[nodiscard]] std::string make_errno_message(const std::filesystem::path &filename, std::string_view action) {
    return std::format("Failed to {} file '{}': {}", action, filename.string(), std::strerror(errno));
}

} // namespace

FileHandle::FileHandle(FILE *file) noexcept : m_handle{file} {}

FILE *FileHandle::handle() noexcept { return m_handle.get(); }

const FILE *FileHandle::handle() const noexcept { return m_handle.get(); }

FileHandle::operator bool() const noexcept { return m_handle != nullptr; }


std::string read_text_file(const std::filesystem::path &filename) {
    assert(!filename.empty());

    auto file = FileHandle{open_file(filename, "rb")};

    if (!file) {
        throw std::runtime_error{make_errno_message(filename, "open for reading")};
    }

    auto result = std::string{};
    auto buffer = std::array<char, 64 * 1024>{}; // around 4 500 – 5 500 words

    while (true) {
        const auto read_bytes = std::fread(buffer.data(), 1, buffer.size(), file.handle());

        if (read_bytes > 0) {
            result.append(buffer.data(), read_bytes);
        }

        if (read_bytes < buffer.size()) {
            if (std::ferror(file.handle()) != 0) {
                throw std::runtime_error{make_errno_message(filename, "read")};
            }

            break;
        }
    }

    return result;
}

void write_text_file(const std::filesystem::path &filename, const std::string_view content) {
    assert(!filename.empty());

    ensure_parent_directory_exists(filename);

    auto file = FileHandle{open_file(filename, "wb")};

    if (!file) {
        throw std::runtime_error{make_errno_message(filename, "open for writing")};
    }

    const auto written_byte_count = std::fwrite(content.data(), 1, content.size(), file.handle());
    if (written_byte_count != content.size()) {
        throw std::runtime_error{make_errno_message(filename, "write")};
    }

    if (std::fflush(file.handle()) != 0) {
        throw std::runtime_error{make_errno_message(filename, "flush")};
    }
}

void write_text_file_atomic(const std::filesystem::path &filename, const std::string_view content) {
    assert(!filename.empty());

    ensure_parent_directory_exists(filename);

    auto temp_filename = filename;
    temp_filename += ".tmp";

    write_text_file(temp_filename, content);

    auto error = std::error_code{};

    if (std::filesystem::exists(filename, error)) {
        std::filesystem::remove(filename, error);

        if (error) {
            throw std::runtime_error{
                    std::format("Failed to replace file '{}': {}", filename.string(), error.message())};
        }
    }

    std::filesystem::rename(temp_filename, filename, error);

    if (error) {
        throw std::runtime_error{std::format("Failed to rename temp file '{}' to '{}': {}",
                                             temp_filename.string(),
                                             filename.string(),
                                             error.message())};
    }
}

} // namespace stz::intern::util