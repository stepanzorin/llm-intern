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
        m_logger->debug("Retrieved knowledge: {} score={} role={} source={}",
                        item.filename,
                        item.score,
                        to_string(item.role),
                        to_string(item.source));
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
    auto prompt = std::format(
            "Ты — AI-помощник стажёра в сфере услуг.\n"
            "Роль стажёра: {}.\n"
            "Отвечай на русском языке, просто и по делу.\n"
            "Используй найденные Markdown-фрагменты как главный источник регламента.\n"
            "Если точного ответа в фрагментах нет, прямо скажи: точный регламент не найден.\n"
            "Не выдумывай правила компании, скидки, возвраты, компенсации и гарантии.\n"
            "В спорных, денежных, юридических, медицинских, опасных и конфликтных ситуациях советуй обратиться к "
            "старшему.\n"
            "Формат: короткий вывод + шаги списком.\n"
            "Если пользователь просит действия, инструкцию или по шагам — дай 6-10 шагов из фрагментов.\n"
            "Кратко объясняй, но не сокращай найденные шаги.\n"
            "Не пиши предупреждение и источники: программа добавит их сама.\n",
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

} // namespace stz::intern