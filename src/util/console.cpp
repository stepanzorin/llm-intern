#include "util/console.hpp"


#include <cassert>
#include <clocale>
#include <iostream>
#include <stdexcept>
#include <string>


#include "platform.hpp"

#ifdef STZ_INTERN_PLATFORM_WINDOWS
    #include <array>
    #include <cstddef>
    #include <format>
    #include <limits>
#define NOMINMAX
    #include <windows.h>
#endif

namespace stz::intern::util {

namespace {

#ifdef STZ_INTERN_PLATFORM_WINDOWS

[[nodiscard]] std::string make_windows_error_message(const char *action, DWORD error_code) {
    return std::format("{} failed. Windows error code: {}", action, error_code);
}

[[nodiscard]] bool stdin_is_windows_console() noexcept {
    const auto handle = GetStdHandle(STD_INPUT_HANDLE);

    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return false;
    }

    auto mode = DWORD{};

    return GetConsoleMode(handle, &mode) != 0;
}

[[nodiscard]] std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }

    if (text.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw std::runtime_error{"Input line is too large for UTF-16 to UTF-8 conversion"};
    }

    const auto wide_size = static_cast<int>(text.size());

    const auto utf8_size =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), wide_size, nullptr, 0, nullptr, nullptr);

    if (utf8_size <= 0) {
        throw std::runtime_error{make_windows_error_message("WideCharToMultiByte size calculation", GetLastError())};
    }

    auto result = std::string(static_cast<std::size_t>(utf8_size), '\0');

    const auto converted_size = WideCharToMultiByte(CP_UTF8,
                                                    WC_ERR_INVALID_CHARS,
                                                    text.data(),
                                                    wide_size,
                                                    result.data(),
                                                    utf8_size,
                                                    nullptr,
                                                    nullptr);

    if (converted_size <= 0) {
        throw std::runtime_error{make_windows_error_message("WideCharToMultiByte conversion", GetLastError())};
    }

    return result;
}

void remove_console_line_end(std::wstring &line) {
    if (!line.empty() && line.back() == L'\n') {
        line.pop_back();
    }

    if (!line.empty() && line.back() == L'\r') {
        line.pop_back();
    }
}

[[nodiscard]] bool read_windows_console_line_utf8(std::string &line) {
    const auto handle = GetStdHandle(STD_INPUT_HANDLE);

    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return false;
    }

    auto wide_line = std::wstring{};
    auto buffer = std::array<wchar_t, 512>{};

    while (true) {
        auto read_chars = DWORD{};

        const auto ok = ReadConsoleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read_chars, nullptr);

        if (ok == 0) {
            throw std::runtime_error{make_windows_error_message("ReadConsoleW", GetLastError())};
        }

        if (read_chars == 0) {
            return false;
        }

        wide_line.append(buffer.data(), buffer.data() + read_chars);

        if (wide_line.find(L'\n') != std::wstring::npos) {
            break;
        }
    }

    remove_console_line_end(wide_line);

    line = wide_to_utf8(wide_line);

    return true;
}

#endif

} // namespace

void configure_console() {
#ifdef STZ_INTERN_PLATFORM_WINDOWS
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::setlocale(LC_ALL, ".UTF-8");
#else
    std::setlocale(LC_ALL, "");
#endif
}

bool read_console_line_utf8(std::string &line) {
#ifdef STZ_INTERN_PLATFORM_WINDOWS
    if (stdin_is_windows_console()) {
        return read_windows_console_line_utf8(line);
    }
#endif

    return static_cast<bool>(std::getline(std::cin, line));
}

} // namespace stz::intern::util