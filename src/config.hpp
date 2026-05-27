// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace stz::intern {

struct llama_server_config_s {
    std::filesystem::path executable_path = "backend/llama-server";
    std::filesystem::path model_path = "models/qwen2.5-3b-instruct-q4_k_m.gguf";

    std::string scheme = "http";
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string api_path = "/v1/chat/completions";

    std::optional<std::string> model_alias = {"intern-local-model"};

    std::int32_t context_size = 8192;
    std::int32_t parallel_slots = 1;

    std::int32_t max_tokens = 1024;
    double temperature = 0.2;
    double top_p = 0.9;

    std::int32_t connection_timeout_seconds = 10;
    std::int32_t read_timeout_seconds = 600;
    std::int32_t write_timeout_seconds = 600;
};

[[nodiscard]] llama_server_config_s load_server_config(const std::filesystem::path &filename);

void validate_server_config(const llama_server_config_s &config);


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