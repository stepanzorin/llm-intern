#include "llm/llama_client.hpp"

#include <cassert>
#include <format>
#include <stdexcept>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace stz::intern {

namespace {

using json = nlohmann::json;

[[nodiscard]] std::string model_name_for_request(const llama_server_config_s &config) {
    if (config.model_alias.has_value() && !config.model_alias->empty()) {
        return *config.model_alias;
    }

    return "local-model";
}

[[nodiscard]] json make_chat_messages_json(const std::span<const chat_message_s> messages) {
    auto result = json::array();

    for (const auto &message : messages) {
        result.push_back(json{
                {"role", std::string{to_string(message.role)}},
                {"content", message.content},
        });
    }

    return result;
}

[[nodiscard]] std::string extract_assistant_content(const std::string &body) {
    const auto root = json::parse(body);

    const auto &choices = root.at("choices");

    if (!choices.is_array() || choices.empty()) {
        throw std::runtime_error{"llama-server response has no choices"};
    }

    const auto &message = choices.at(0).at("message");

    return message.at("content").get<std::string>();
}

} // namespace

LlamaClient::LlamaClient(llama_server_config_s config, std::shared_ptr<spdlog::logger> logger)
    : m_config{std::move(config)},
      m_logger{std::move(logger)} {
    assert(m_logger != nullptr);
    validate_server_config(m_config);
}

llama_client_response_s LlamaClient::complete_chat(const std::span<const chat_message_s> messages) const {
    assert(!messages.empty());

    auto client = httplib::Client{m_config.host, m_config.port};

    client.set_connection_timeout(m_config.connection_timeout_seconds, 0);
    client.set_read_timeout(m_config.read_timeout_seconds, 0);
    client.set_write_timeout(m_config.write_timeout_seconds, 0);

    const auto request = json{
            {"model", model_name_for_request(m_config)},
            {"messages", make_chat_messages_json(messages)},
            {"stream", false},
            {"temperature", m_config.temperature},
            {"top_p", m_config.top_p},
            {"max_tokens", m_config.max_tokens},
            {"cache_prompt", true},
            {"id_slot", 0},
    };

    const auto body = request.dump();

    m_logger->debug("POST http://{}:{}{}", m_config.host, m_config.port, m_config.api_path);

    auto response = client.Post(m_config.api_path, body, "application/json; charset=utf-8");

    if (!response) {
        throw std::runtime_error{std::format("llama-server HTTP request failed. httplib error code: {}",
                                             static_cast<int>(response.error()))};
    }

    if (response->status < 200 || response->status >= 300) {
        throw std::runtime_error{std::format("llama-server returned HTTP {}: {}", response->status, response->body)};
    }

    try {
        auto content = extract_assistant_content(response->body);

        return llama_client_response_s{
                .content = std::move(content),
        };
    } catch (const std::exception &error) {
        throw std::runtime_error{std::format("Failed to parse llama-server response: {}", error.what())};
    }
}

} // namespace stz::intern