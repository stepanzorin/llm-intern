#include "llm/llama_client.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <exception>
#include <format>
#include <ranges>
#include <stdexcept>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace stz::intern::llm {

namespace {

using json = nlohmann::json;

constexpr auto minimum_response_tokens = 128;
constexpr auto short_response_tokens = 256;
constexpr auto normal_response_tokens = 384;
constexpr auto detailed_response_tokens = 640;
constexpr auto large_response_tokens = 896;


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

    if (config.context_window <= 0) {
        throw std::runtime_error{"LLM context_window must be greater than zero"};
    }

    if (config.context_safety_margin < 0) {
        throw std::runtime_error{"LLM context_safety_margin must not be negative"};
    }

    if (config.context_safety_margin >= config.context_window) {
        throw std::runtime_error{"LLM context_safety_margin must be smaller than context_window"};
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

[[nodiscard]] const chat_message_s *find_last_user_message(const std::span<const chat_message_s> messages) noexcept {
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == chat_role_e::user) {
            return &*it;
        }
    }

    return nullptr;
}

[[nodiscard]] bool contains_any(const std::string_view text, const std::span<const std::string_view> phrases) noexcept {
    return std::ranges::any_of(phrases, [text](const std::string_view phrase) { return text.contains(phrase); });
}

[[nodiscard]] bool looks_like_short_answer_request(const std::string_view text) noexcept {
    constexpr auto phrases = std::array{std::string_view{"кратко"},
                                        std::string_view{"коротко"},
                                        std::string_view{"короче"},
                                        std::string_view{"покороче"},
                                        std::string_view{"в двух словах"},
                                        std::string_view{"по сути"},
                                        std::string_view{"суть"},
                                        std::string_view{"основное"},
                                        std::string_view{"главное"},
                                        std::string_view{"без воды"},
                                        std::string_view{"самое важное"},
                                        std::string_view{"в общих чертах"},
                                        std::string_view{"сжато"},
                                        std::string_view{"одним предложением"},
                                        std::string_view{"тезисно"},
                                        std::string_view{"конспективно"},
                                        std::string_view{"вкратце"},
                                        std::string_view{"выжимка"},
                                        std::string_view{"без подробностей"},
                                        std::string_view{"без деталей"},
                                        std::string_view{"быстро"},
                                        std::string_view{"быстрее"},
                                        std::string_view{"фастом"},
                                        std::string_view{"шустро"},
                                        std::string_view{"без духоты"},
                                        std::string_view{"не растекаться"},
                                        std::string_view{"без предисловий"},
                                        std::string_view{"сухо"}};

    return contains_any(text, phrases);
}

[[nodiscard]] bool looks_like_detailed_answer_request(const std::string_view text) noexcept {
    constexpr auto phrases = std::array{std::string_view{"подробно"},
                                        std::string_view{"подробнее"},
                                        std::string_view{"объясни"},
                                        std::string_view{"почему"},
                                        std::string_view{"пример"},
                                        std::string_view{"примеры"},
                                        std::string_view{"развернуто"},
                                        std::string_view{"распиши"},
                                        std::string_view{"по шагам"},
                                        std::string_view{"пошагово"},
                                        std::string_view{"детально"},
                                        std::string_view{"что делать если"},
                                        std::string_view{"что делать, если"},
                                        std::string_view{"инструкция"},
                                        std::string_view{"объясни подробно"},
                                        std::string_view{"расскажи подробнее"},
                                        std::string_view{"простыми словами"},
                                        std::string_view{"опиши"},
                                        std::string_view{"как работает"},
                                        std::string_view{"зачем"},
                                        std::string_view{"в чём причина"},
                                        std::string_view{"с чего начать"},
                                        std::string_view{"алгоритм"},
                                        std::string_view{"последовательность"},
                                        std::string_view{"поэтапно"},
                                        std::string_view{"шаг за шагом"},
                                        std::string_view{"разбор"},
                                        std::string_view{"разбери"},
                                        std::string_view{"разжуй"},
                                        std::string_view{"объясни на пальцах"},
                                        std::string_view{"разъясни"},
                                        std::string_view{"объясни популярно"},
                                        std::string_view{"углублённо"},
                                        std::string_view{"анализ"},
                                        std::string_view{"полный разбор"},
                                        std::string_view{"максимально подробно"},
                                        std::string_view{"объясни причину"},
                                        std::string_view{"почему так"},
                                        std::string_view{"в чём смысл"},
                                        std::string_view{"как устроено"},
                                        std::string_view{"что делать сначала"},
                                        std::string_view{"расскажи как для новичка"},
                                        std::string_view{"напиши как для чайников"},
                                        std::string_view{"расскажи всё что знаешь"},
                                        std::string_view{"полностью"},
                                        std::string_view{"в деталях"},
                                        std::string_view{"во всех деталях"},
                                        std::string_view{"исчерпывающе"},
                                        std::string_view{"со всеми подробностями"},
                                        std::string_view{"в мельчайших деталях"},
                                        std::string_view{"дотошно"},
                                        std::string_view{"всесторонне"},
                                        std::string_view{"растолкуй"},
                                        std::string_view{"шаг-за-шагом"},
                                        std::string_view{"step by step"},
                                        std::string_view{"step-by-step"},
                                        std::string_view{"откуда"},
                                        std::string_view{"расскажи как для идиота"},
                                        std::string_view{"расскажи как для стажёра"},
                                        std::string_view{"вывали всё"},
                                        std::string_view{"вникни"},
                                        std::string_view{"просвети"}};

    return contains_any(text, phrases);
}

[[nodiscard]] int estimate_text_tokens(const std::string_view text) noexcept {
    auto ascii_bytes = std::size_t{};
    auto non_ascii_codepoints = std::size_t{};

    for (auto index = std::size_t{}; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);

        if (first < 0x80) {
            ++ascii_bytes;
            ++index;
            continue;
        }

        ++non_ascii_codepoints;

        if ((first & 0xE0) == 0xC0 && index + 1 < text.size()) {
            index += 2;
        } else if ((first & 0xF0) == 0xE0 && index + 2 < text.size()) {
            index += 3;
        } else if ((first & 0xF8) == 0xF0 && index + 3 < text.size()) {
            index += 4;
        } else {
            ++index;
        }
    }

    const auto ascii_tokens = (ascii_bytes + 3) / 4;
    const auto non_ascii_tokens = (non_ascii_codepoints + 1) / 2;

    return static_cast<int>(ascii_tokens + non_ascii_tokens + 1);
}

[[nodiscard]] int estimate_prompt_tokens(const std::span<const chat_message_s> messages) noexcept {
    auto result = 16;

    for (const auto &message : messages) {
        result += 8;
        result += estimate_text_tokens(message.content);
    }

    return result;
}

[[nodiscard]] int clamp_response_tokens(const int desired_tokens, const int configured_max_tokens) noexcept {
    assert(configured_max_tokens > 0);

    if (configured_max_tokens < minimum_response_tokens) {
        return configured_max_tokens;
    }

    return std::clamp(desired_tokens, minimum_response_tokens, configured_max_tokens);
}

[[nodiscard]] int estimate_response_max_tokens(const std::span<const chat_message_s> messages,
                                               const int configured_max_tokens) noexcept {
    assert(configured_max_tokens > 0);

    const auto *last_user_message = find_last_user_message(messages);

    if (last_user_message == nullptr) {
        return clamp_response_tokens(normal_response_tokens, configured_max_tokens);
    }

    const auto user_text = std::string_view{last_user_message->content};

    if (user_text.empty()) {
        return clamp_response_tokens(short_response_tokens, configured_max_tokens);
    }

    const auto input_bytes = user_text.size();

    auto desired_tokens = normal_response_tokens;

    if (input_bytes <= 80) {
        desired_tokens = short_response_tokens;
    } else if (input_bytes <= 300) {
        desired_tokens = normal_response_tokens;
    } else if (input_bytes <= 900) {
        desired_tokens = detailed_response_tokens;
    } else {
        desired_tokens = large_response_tokens;
    }

    if (looks_like_short_answer_request(user_text)) {
        desired_tokens = std::min(desired_tokens, short_response_tokens);
    }

    if (looks_like_detailed_answer_request(user_text)) {
        desired_tokens = std::max(desired_tokens, detailed_response_tokens);
    }

    return clamp_response_tokens(desired_tokens, configured_max_tokens);
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

    const auto desired_response_tokens = estimate_response_max_tokens(messages, m_config.max_tokens);
    const auto estimated_prompt_tokens = estimate_prompt_tokens(messages);
    const auto available_response_tokens =
            m_config.context_window - m_config.context_safety_margin - estimated_prompt_tokens;

    if (available_response_tokens <= 0) {
        throw std::runtime_error{std::format(
                "LLM prompt does not fit the configured context budget: estimated_prompt_tokens={}, "
                "context_window={}, safety_margin={}",
                estimated_prompt_tokens,
                m_config.context_window,
                m_config.context_safety_margin)};
    }

    const auto response_max_tokens = std::min(desired_response_tokens, available_response_tokens);

    if (response_max_tokens < minimum_response_tokens) {
        m_logger->warn("Only {} estimated tokens remain for the response; prompt={} context={} safety={}",
                       response_max_tokens,
                       estimated_prompt_tokens,
                       m_config.context_window,
                       m_config.context_safety_margin);
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
            {"max_tokens", response_max_tokens},
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

    m_logger->debug("POST {}{} model='{}' slot={} prompt_tokens~{} max_tokens={} context={}",
                    m_server->url(),
                    endpoint.chat_completions_path,
                    request_model_name(state),
                    session->slot_id(),
                    estimated_prompt_tokens,
                    response_max_tokens,
                    m_config.context_window);

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