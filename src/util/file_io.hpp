// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

// Disable MSVC security warnings for standard C functions (like std::fopen)
#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace stz::intern::util {

class FileHandle {
public:
    FileHandle() = delete;
    explicit FileHandle(FILE *file) noexcept;

    [[nodiscard]] FILE *handle() noexcept;
    [[nodiscard]] const FILE *handle() const noexcept;

    [[nodiscard]] explicit operator bool() const noexcept;

private:
    using file_closer_t = decltype([](FILE *const ptr) { std::fclose(ptr); });
    using unique_file_ptr_t = std::unique_ptr<FILE, file_closer_t>;

    unique_file_ptr_t m_handle;
};


[[nodiscard]] std::string read_text_file(const std::filesystem::path &filename);

void write_text_file(const std::filesystem::path &filename, std::string_view content);
void write_text_file_atomic(const std::filesystem::path &filename, std::string_view content);

} // namespace stz::intern::util