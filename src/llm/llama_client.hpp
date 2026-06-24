// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <spdlog/logger.h>

#include "chat_history.hpp"
#include "llm/llama_server.hpp"

namespace stz::intern::llm {

enum class llama_completion_status_e {
    completed,
    cancelled,
};

struct llama_client_config_s {
    double temperature = 0.2;
    double top_p = 0.9;

    std::int32_t max_tokens = 512;

    // Logical prompt + response budget. Keep it not greater than llama-server n_ctx.
    std::int32_t context_window = 2048;
    std::int32_t context_safety_margin = 128;

    bool cache_prompt = true;

    std::chrono::seconds connection_timeout = std::chrono::seconds{10};
    std::chrono::seconds read_timeout = std::chrono::minutes{10};
    std::chrono::seconds write_timeout = std::chrono::seconds{600};
};

struct llama_client_response_s {
    llama_completion_status_e status = llama_completion_status_e::completed;
    std::string content = {};
};

using llama_stream_callback_t = std::function<void(std::string_view)>;

class LlamaClient final {
public:
    LlamaClient(LlamaServer &server, const llama_client_config_s &config, std::shared_ptr<spdlog::logger> logger);

    [[nodiscard]] llama_client_response_s complete_chat(std::span<const chat_message_s> messages,
                                                        const llama_stream_callback_t &stream_callback = {});

private:
    LlamaServer *m_server = nullptr;
    llama_client_config_s m_config;
    std::shared_ptr<spdlog::logger> m_logger;
};

} // namespace stz::intern::llm