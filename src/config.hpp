// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace stz::intern {

struct database_config_s {
    bool enabled = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 5432;
    std::string database = "intern";
    std::string user = "intern";
    std::string password = {};
};


struct auth_config_s {
    bool enabled = false;
    bool is_available = true;
    std::string login = {};
    std::string password = {};
};

} // namespace stz::intern