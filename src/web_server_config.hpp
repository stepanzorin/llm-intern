// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace stz::intern {

struct web_server_config_s {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3000;

    std::filesystem::path web_directory = "web";

    std::size_t worker_threads = 4;

    bool open_browser_on_start = true;
};

} // namespace stz::intern