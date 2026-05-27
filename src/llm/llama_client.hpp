// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <memory>
#include <span>
#include <string>

#include <spdlog/logger.h>

#include "chat_history.hpp"
#include "config.hpp"

namespace stz::intern {

struct llama_client_response_s {
    std::string content{};
};

class LlamaClient final {
public:
    LlamaClient(llama_server_config_s config, std::shared_ptr<spdlog::logger> logger);

    [[nodiscard]] llama_client_response_s complete_chat(std::span<const chat_message_s> messages) const;

private:
    llama_server_config_s m_config;
    std::shared_ptr<spdlog::logger> m_logger;
};

} // namespace stz::intern