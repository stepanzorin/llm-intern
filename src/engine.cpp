#include "engine.hpp"

#include <cassert>
#include <format>
#include <ranges>
#include <stdexcept>

#include "util/string_helpers.hpp"
#include "util/time.hpp"

namespace stz::intern {

namespace {

[[nodiscard]] std::string join_source_filenames(std::span<const std::string> source_filenames) {
    if (source_filenames.empty()) {
        return "подходящих Markdown-файлов не найдено";
    }

    auto result = std::string{};

    for (const auto &[index, filename] : source_filenames | std::views::enumerate) {
        if (index != 0) {
            result += ", ";
        }

        result += filename;
    }

    return result;
}

[[nodiscard]] bool answer_already_has_sources_block(const std::string_view answer) noexcept {
    return answer.find("Опирался на файлы") != std::string_view::npos ||
           answer.find("Источники") != std::string_view::npos;
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
}

std::string LlamaEngine::ask(std::string_view user_text) {
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

    const auto knowledge = m_knowledge.retrieve(user_text,
                                                m_engine_config.max_knowledge_documents,
                                                m_engine_config.max_knowledge_chars_per_document);

    const auto request_messages = build_request_messages(knowledge);
    auto response = m_client.complete_chat(request_messages);

    auto source_filenames = make_source_filenames(knowledge);
    auto answer = ensure_sources_block(std::move(response.content), source_filenames);

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

std::string LlamaEngine::build_system_prompt(const std::span<const retrieved_knowledge_s> knowledge) {
    auto prompt = std::string{
            "Ты — AI-бот помощи стажёрам в сфере УСЛУГ (адинистраторы на респешене, баристы, продавцы, консультанты): "
            "Отвечай всегда на **русском** языке простыми, точными и практически полезными формулировками.\n"
            "Если вопрос связан с предоставленными Markdown-знаниями, используй их как **приоритетный** источник.\n"
            "Если знаний недостаточно, **честно скажи** об этом и отдели предположение от факта.\n"
            "В конце **каждого** ответа обязательно добавляй строку: "
            "\"Опирался на файлы: <список файлов>\".\n"};
    if (knowledge.empty()) {
        prompt += "\nПодходящих Markdown-файлов для текущего вопроса не найдено.\n";
        return prompt;
    }

    prompt += "\nДоступные фрагменты базы знаний:\n";

    for (const auto &item : knowledge) {
        prompt += std::format("\n---\nФайл: {}\nЗаголовок: {}\nРелевантность: {}\n\n{}\n",
                              item.filename,
                              item.title,
                              item.score,
                              item.content);
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
        messages.push_back(std::move(message));
    }

    return messages;
}

std::vector<std::string> LlamaEngine::make_source_filenames(const std::span<const retrieved_knowledge_s> knowledge) {
    auto result = std::vector<std::string>{};
    result.reserve(knowledge.size());

    for (const auto &item : knowledge) {
        result.push_back(item.filename);
    }

    return result;
}

std::string LlamaEngine::ensure_sources_block(std::string answer, const std::span<const std::string> source_filenames) {
    auto normalized = std::move(answer);
    util::trim(normalized);

    if (normalized.empty()) {
        normalized = "Не удалось получить содержательный ответ от модели.";
    }

    if (answer_already_has_sources_block(normalized)) {
        return normalized;
    }

    normalized += std::format("\n\nОпирался на файлы: {}", join_source_filenames(source_filenames));

    return normalized;
}

} // namespace stz::intern