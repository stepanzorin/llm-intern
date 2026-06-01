#include "engine.hpp"

#include <cassert>
#include <format>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "util/string_helpers.hpp"
#include "util/time.hpp"

namespace stz::intern {

namespace {

constexpr auto service_warning_text =
        "⚠️ В первую очередь руководствуйтесь правилами и инструкциями вашей компании или заведения.";

[[nodiscard]] std::string join_source_filenames(const std::span<const std::string> source_filenames) {
    if (source_filenames.empty()) {
        return "подходящих Markdown-файлов не найдено";
    }

    auto result = std::string{};

    for (auto index = std::size_t{}; index < source_filenames.size(); ++index) {
        if (index != 0) {
            result += ", ";
        }

        result += source_filenames[index];
    }

    return result;
}

[[nodiscard]] bool is_generated_service_line(const std::string_view line) {
    auto copy = std::string{line};
    util::trim(copy);

    return copy == service_warning_text || copy.starts_with("Опирался на файлы:") || copy.starts_with("Источники:") ||
           copy.starts_with("Источник:");
}

[[nodiscard]] std::string remove_generated_service_lines(const std::string_view text) {
    auto stream = std::istringstream{std::string{text}};
    auto line = std::string{};
    auto result = std::string{};

    while (std::getline(stream, line)) {
        if (is_generated_service_line(line)) {
            continue;
        }

        if (!result.empty()) {
            result.push_back('\n');
        }

        result += line;
    }

    util::trim(result);

    return result;
}

[[nodiscard]] chat_message_s make_model_visible_history_message(chat_message_s message) {
    message.content = remove_generated_service_lines(message.content);
    return message;
}

[[nodiscard]] bool is_markdown_heading(const std::string_view line) {
    auto trimmed = std::string{line};
    util::trim(trimmed);

    return trimmed.starts_with("#");
}

[[nodiscard]] std::string markdown_heading_text(const std::string_view line) {
    auto text = std::string{line};
    util::trim(text);

    while (!text.empty() && text.front() == '#') {
        text.erase(text.begin());
    }

    util::trim(text);

    return text;
}

[[nodiscard]] bool is_target_answer_heading(const std::string_view line) {
    auto text = markdown_heading_text(line);
    util::trim(text);

    return text == "Пошаговая инструкция что делать";
}

[[nodiscard]] std::string extract_direct_answer_section(const std::string_view markdown) {
    auto inside_section = false;
    auto result = std::string{};
    auto position = std::size_t{};

    while (position <= markdown.size()) {
        const auto line_end = markdown.find('\n', position);
        const auto line = markdown.substr(
                position,
                line_end == std::string_view::npos ? markdown.size() - position : line_end - position);

        if (!inside_section) {
            if (is_markdown_heading(line) && is_target_answer_heading(line)) {
                inside_section = true;
            }
        } else {
            if (is_markdown_heading(line)) {
                break;
            }

            if (!result.empty()) {
                result.push_back('\n');
            }

            result += line;
        }

        if (line_end == std::string_view::npos) {
            break;
        }

        position = line_end + 1;
    }

    util::trim(result);

    if (!result.empty()) {
        return result;
    }

    auto fallback = std::string{markdown};
    util::trim(fallback);

    return fallback;
}

} // namespace

LlamaEngine::LlamaEngine(llama_server_config_s server_config,
                         engine_config_s engine_config,
                         const std::shared_ptr<spdlog::logger> &logger)
    : m_server_config{std::move(server_config)},
      m_engine_config{std::move(engine_config)},
      m_history{m_engine_config.history_file, logger->clone("ChatHistory")},
      m_knowledge{m_engine_config.knowledge_directory, logger->clone("KnowledgeStorage")},
      m_client{m_server_config, logger->clone("LlamaClient")},
      m_logger{logger->clone("LlamaEngine")} {
    assert(logger != nullptr);
}

void LlamaEngine::load() {
    m_history.load();
    m_knowledge.load();

    if (m_knowledge.empty()) {
        m_logger->warn("Knowledge storage is empty");
    }

    m_logger->info("Workplace role: {}", to_string(m_engine_config.workplace_role));
}

std::string LlamaEngine::ask(const std::string_view user_text) {
    if (util::is_blank(user_text)) {
        throw std::runtime_error{"User message is empty"};
    }

    auto user_message = chat_message_s{
            .role = chat_role_e::user,
            .name = "Я",
            .content = std::string{user_text},
            .created_at = util::make_local_timestamp(),
            .source_files = {},
    };

    m_history.append(std::move(user_message));
    m_history.save();

    const auto knowledge = m_knowledge.retrieve(
            user_text,
            knowledge_retrieve_options_s{
                    .workplace_role = m_engine_config.workplace_role,
                    .include_general = true,
                    .include_policy = false,
                    .include_custom = true,
                    .limit = m_engine_config.max_knowledge_documents,
                    .max_chars_per_document = m_engine_config.max_knowledge_chars_per_document,
            });

    for (const auto &item : knowledge) {
        m_logger->info("Retrieved knowledge: {} score={} role={} source={} match={}",
                       item.filename,
                       item.score,
                       to_string(item.role),
                       to_string(item.source),
                       to_string(item.match));
    }

    if (can_answer_without_llm(knowledge)) {
        auto source_filenames = make_source_filenames(knowledge);
        auto answer_body = make_direct_answer(knowledge);
        auto answer = ensure_sources_block(answer_body, source_filenames);

        auto assistant_message = chat_message_s{
                .role = chat_role_e::assistant,
                .name = "AI-бот",
                .content = answer,
                .created_at = util::make_local_timestamp(),
                .source_files = std::move(source_filenames),
        };

        m_history.append(std::move(assistant_message));
        m_history.save();

        m_logger->info("Answered without LLM by direct knowledge match");

        return answer;
    }

    const auto request_messages = build_request_messages(knowledge);
    auto response = m_client.complete_chat(request_messages);

    auto source_filenames = make_source_filenames(knowledge);
    auto answer = ensure_sources_block(response.content, source_filenames);

    auto assistant_message = chat_message_s{
            .role = chat_role_e::assistant,
            .name = "AI-бот",
            .content = answer,
            .created_at = util::make_local_timestamp(),
            .source_files = std::move(source_filenames),
    };

    m_history.append(std::move(assistant_message));
    m_history.save();

    return answer;
}

const ChatHistory &LlamaEngine::history() const noexcept { return m_history; }

std::string LlamaEngine::build_system_prompt(const std::span<const retrieved_knowledge_s> knowledge) const {
    auto prompt = std::format("Ты — AI-помощник стажёра в сфере услуг.\n"
                              "Роль стажёра: {}.\n"
                              "Отвечай на русском языке, просто, понятно и по делу.\n"
                              "Используй найденные Markdown-фрагменты как главный источник регламента.\n"
                              "Если точного ответа в фрагментах нет, прямо скажи: точный регламент не найден.\n"
                              "Не выдумывай шаги, правила компании, скидки, возвраты, компенсации и гарантии.\n"
                              "Формат: полноценный список понятных шагов из файла.\n",
                              to_string(m_engine_config.workplace_role));

    if (knowledge.empty()) {
        prompt += "\nФрагменты базы знаний не найдены.\n";
        return prompt;
    }

    prompt += "\nФрагменты базы знаний:\n";

    for (const auto &item : knowledge) {
        prompt += std::format("\n---\nФайл: {}\n{}\n", item.filename, item.content);
    }

    return prompt;
}

std::vector<chat_message_s> LlamaEngine::build_request_messages(
        const std::span<const retrieved_knowledge_s> knowledge) const {
    auto messages = std::vector<chat_message_s>{};

    messages.push_back(chat_message_s{
            .role = chat_role_e::system,
            .name = "system",
            .content = build_system_prompt(knowledge),
            .created_at = util::make_local_timestamp(),
            .source_files = {},
    });

    auto history_messages = m_history.recent_messages(m_engine_config.max_history_messages_for_request);

    for (auto &message : history_messages) {
        auto model_visible_message = make_model_visible_history_message(std::move(message));

        if (util::is_blank(model_visible_message.content)) {
            continue;
        }

        messages.push_back(std::move(model_visible_message));
    }

    return messages;
}

std::vector<std::string> LlamaEngine::make_source_filenames(const std::span<const retrieved_knowledge_s> knowledge) {
    auto result = std::vector<std::string>{};
    auto seen = std::unordered_set<std::string>{};

    result.reserve(knowledge.size());

    for (const auto &item : knowledge) {
        if (seen.insert(item.filename).second) {
            result.push_back(item.filename);
        }
    }

    return result;
}

std::string LlamaEngine::ensure_sources_block(const std::string &answer,
                                              const std::span<const std::string> source_filenames) {
    auto normalized = remove_generated_service_lines(answer);

    if (normalized.empty()) {
        normalized = "Не удалось получить содержательный ответ от модели.";
    }

    auto result = std::string{service_warning_text};
    result += "\n\n";
    result += normalized;
    result += std::format("\n\nОпирался на файлы: {}", join_source_filenames(source_filenames));

    return result;
}

bool LlamaEngine::can_answer_without_llm(const std::span<const retrieved_knowledge_s> knowledge) noexcept {
    if (knowledge.size() != 1) {
        return false;
    }

    return knowledge.front().match == knowledge_match_e::exact_frequent_query ||
           knowledge.front().match == knowledge_match_e::unordered_frequent_query ||
           knowledge.front().match == knowledge_match_e::unordered_fuzzy_frequent_query;
}

std::string LlamaEngine::make_direct_answer(const std::span<const retrieved_knowledge_s> knowledge) {
    assert(can_answer_without_llm(knowledge));

    return extract_direct_answer_section(knowledge.front().content);
}

} // namespace stz::intern