#include "llm/llama_client.hpp"

#include <cassert>
#include <exception>
#include <format>
#include <stdexcept>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace stz::intern::llm {

namespace {

using json = nlohmann::json;

class ChatCompletionStreamParser final {
public:
    explicit ChatCompletionStreamParser(const llama_stream_callback_t &callback) : m_callback{callback} {}

    void append(const std::string_view chunk) {
        m_buffer.append(chunk);

        while (true) {
            const auto line_end = m_buffer.find('\n');

            if (line_end == std::string::npos) {
                break;
            }

            auto line = m_buffer.substr(0, line_end);
            m_buffer.erase(0, line_end + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            process_line(line);
        }
    }

    void finish() {
        if (!m_buffer.empty()) {
            process_line(m_buffer);
            m_buffer.clear();
        }
    }

    [[nodiscard]] const std::string &content() const noexcept { return m_content; }

private:
    void process_line(const std::string_view line) {
        if (line.empty() || !line.starts_with("data:")) {
            return;
        }

        auto payload = line.substr(5);

        while (!payload.empty() && payload.front() == ' ') {
            payload.remove_prefix(1);
        }

        if (payload.empty() || payload == "[DONE]") {
            return;
        }

        const auto root = json::parse(payload);

        if (const auto error_it = root.find("error"); error_it != root.end()) {
            throw std::runtime_error{std::format("llama-server stream error: {}", error_it->dump())};
        }

        const auto choices_it = root.find("choices");

        if (choices_it == root.end() || !choices_it->is_array() || choices_it->empty()) {
            return;
        }

        const auto &choice = choices_it->at(0);
        const auto delta_it = choice.find("delta");

        if (delta_it == choice.end() || !delta_it->is_object()) {
            return;
        }

        const auto content_it = delta_it->find("content");

        if (content_it == delta_it->end() || content_it->is_null() || !content_it->is_string()) {
            return;
        }

        auto content = content_it->get<std::string>();

        if (content.empty()) {
            return;
        }

        m_content += content;

        if (m_callback) {
            m_callback(content);
        }
    }

    llama_stream_callback_t m_callback;
    std::string m_buffer;
    std::string m_content;
};

void validate_client_config(const llama_client_config_s &config) {
    if (config.temperature < 0.0) {
        throw std::runtime_error{"LLM temperature must not be negative"};
    }

    if (config.top_p <= 0.0 || config.top_p > 1.0) {
        throw std::runtime_error{"LLM top_p must be in range (0, 1]"};
    }

    if (config.max_tokens <= 0) {
        throw std::runtime_error{"LLM max_tokens must be greater than zero"};
    }

    if (config.connection_timeout <= std::chrono::seconds::zero()) {
        throw std::runtime_error{"LLM connection timeout must be greater than zero"};
    }

    if (config.read_timeout <= std::chrono::seconds::zero()) {
        throw std::runtime_error{"LLM read timeout must be greater than zero"};
    }

    if (config.write_timeout <= std::chrono::seconds::zero()) {
        throw std::runtime_error{"LLM write timeout must be greater than zero"};
    }
}

[[nodiscard]] json make_chat_messages_json(const std::span<const chat_message_s> messages) {
    auto result = json::array();
    result.get_ptr<json::array_t *>()->reserve(messages.size());

    for (const auto &message : messages) {
        result.push_back(json{
                {"role", std::string{to_string(message.role)}},
                {"content", message.content},
        });
    }

    return result;
}

[[nodiscard]] httplib::Headers make_headers(const llama_endpoint_config_s &endpoint) {
    auto headers = httplib::Headers{
            {"Content-Type", "application/json; charset=utf-8"},
            {"Accept", "text/event-stream"},
    };

    if (endpoint.api_key.has_value() && !endpoint.api_key->empty()) {
        headers.emplace("Authorization", std::format("Bearer {}", *endpoint.api_key));
    }

    return headers;
}

[[nodiscard]] std::string request_model_name(const llama_server_state_info_s &state) {
    if (!state.current_model_alias.empty()) {
        return state.current_model_alias;
    }

    return std::string{to_string(state.current_model)};
}

} // namespace

LlamaClient::LlamaClient(LlamaServer &server,
                         const llama_client_config_s &config,
                         std::shared_ptr<spdlog::logger> logger)
    : m_server{&server},
      m_config{config},
      m_logger{std::move(logger)} {
    assert(m_server != nullptr);
    assert(m_logger != nullptr);

    validate_client_config(m_config);
}

llama_client_response_s LlamaClient::complete_chat(const std::span<const chat_message_s> messages,
                                                   const llama_stream_callback_t &stream_callback) {
    assert(!messages.empty());
    assert(m_server != nullptr);

    if (!m_server->is_running()) {
        throw std::runtime_error{"llama-server is not ready"};
    }

    auto session = m_server->start_generation();

    const auto endpoint = m_server->endpoint_config();
    const auto state = m_server->state_info();

    auto client = httplib::Client{
            endpoint.host,
            endpoint.port,
    };

    client.set_connection_timeout(static_cast<time_t>(m_config.connection_timeout.count()), 0);

    client.set_read_timeout(static_cast<time_t>(m_config.read_timeout.count()), 0);

    client.set_write_timeout(static_cast<time_t>(m_config.write_timeout.count()), 0);

    client.set_keep_alive(false);

    const auto request_json = json{
            {"model", request_model_name(state)},
            {"messages", make_chat_messages_json(messages)},
            {"stream", true},
            {"temperature", m_config.temperature},
            {"top_p", m_config.top_p},
            {"max_tokens", m_config.max_tokens},
            {"cache_prompt", m_config.cache_prompt},
            {"id_slot", session->slot_id()},
    };

    auto parser = ChatCompletionStreamParser{stream_callback};

    auto http_status = -1;
    auto error_body = std::string{};
    auto request_cancelled = false;
    auto receiver_error = std::exception_ptr{};

    auto request = httplib::Request{};

    request.method = "POST";
    request.path = endpoint.chat_completions_path;
    request.headers = make_headers(endpoint);
    request.body = request_json.dump();

    request.response_handler = [&](const httplib::Response &response) {
        http_status = response.status;
        return true;
    };

    request.content_receiver =
            [&](const char *data, const std::size_t data_length, const std::uint64_t, const std::uint64_t) {
                if (session->stop_requested()) {
                    request_cancelled = true;
                    return false;
                }

                if (http_status < 200 || http_status >= 300) {
                    error_body.append(data, data_length);
                    return true;
                }

                try {
                    parser.append(std::string_view{data, data_length});
                    return true;
                } catch (...) {
                    receiver_error = std::current_exception();
                    return false;
                }
            };

    m_logger->debug("POST {}{} model='{}' slot={}",
                    m_server->url(),
                    endpoint.chat_completions_path,
                    request_model_name(state),
                    session->slot_id());

    const auto response = client.send(request);

    if (receiver_error != nullptr) {
        std::rethrow_exception(receiver_error);
    }

    if (request_cancelled || session->stop_requested()) {
        m_logger->info("llama-server generation was cancelled");

        return llama_client_response_s{
                .status = llama_completion_status_e::cancelled,
                .content = parser.content(),
        };
    }

    if (!response) {
        throw std::runtime_error{
                std::format("llama-server HTTP request failed: error={}", static_cast<int>(response.error()))};
    }

    if (http_status < 200 || http_status >= 300) {
        throw std::runtime_error{std::format("llama-server returned HTTP {}: {}", http_status, error_body)};
    }

    try {
        parser.finish();
    } catch (const std::exception &error) {
        throw std::runtime_error{std::format("Failed to parse llama-server streaming response: {}", error.what())};
    }

    if (parser.content().empty()) {
        throw std::runtime_error{"llama-server returned an empty assistant response"};
    }

    return llama_client_response_s{
            .status = llama_completion_status_e::completed,
            .content = parser.content(),
    };
}

} // namespace stz::intern::llm