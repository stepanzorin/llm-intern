#include "engine.hpp"

#include <algorithm>
#include <cassert>
#include <format>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "util/string_helpers.hpp"
#include "util/time.hpp"

namespace stz::intern {

namespace {

constexpr auto service_warning_text = "⚠️ Бот может допускать ошибки. Рекомендуется перепроверять важную информацию.";

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

[[nodiscard]] bool is_completed_assistant_entry(const chat_history_entry_s &entry) noexcept {
    return entry.assistant.has_value() && entry.status == chat_message_status_e::completed;
}

[[nodiscard]] bool is_completed_model_visible_entry(const chat_history_entry_s &entry) noexcept {
    if (entry.status != chat_message_status_e::completed) {
        return false;
    }

    return entry.user.has_value() || entry.assistant.has_value();
}

void append_unique_source_files(std::vector<std::string> &target, const std::span<const std::string> source) {
    auto seen = std::unordered_set<std::string>{};
    seen.reserve(target.size() + source.size());

    for (const auto &filename : target) {
        seen.insert(filename);
    }

    for (const auto &filename : source) {
        if (seen.insert(filename).second) {
            target.push_back(filename);
        }
    }
}

} // namespace

LlamaEngine::LlamaEngine(engine_config_s config, const std::shared_ptr<spdlog::logger> &logger)
    : m_config{std::move(config)},
      m_server{
              m_config.server,
              logger->clone("LlamaServer"),
      },
      m_client{
              m_server,
              m_config.client,
              logger->clone("LlamaClient"),
      },
      m_knowledge{
              m_config.knowledge_directory,
              logger->clone("KnowledgeStorage"),
      },
      m_logger{logger->clone("LlamaEngine")},
      m_history_logger{logger->clone("ChatHistory")} {
    assert(logger != nullptr);
}

void LlamaEngine::load() {
    load_history();
    m_knowledge.load();
    rebuild_model_relatives();

    if (m_knowledge.empty()) {
        m_logger->warn("Knowledge storage is empty");
    }

    m_logger->info("Workplace role: {}", to_string(m_config.workplace_role));

    m_logger->info("Model relatives restored: {}", m_model_relative_ids.size());

    start_server();
}

void LlamaEngine::start_server() { m_server.start(); }

void LlamaEngine::stop_server() noexcept { m_server.stop(); }

engine_answer_s LlamaEngine::ask(const std::string_view user_text,
                                 const llm::llama_stream_callback_t &stream_callback) {
    if (util::is_blank(user_text)) {
        throw std::runtime_error{"User message is empty"};
    }

    const auto user_index = append_pending_user_entry(user_text);
    save_history();

    const auto knowledge = m_knowledge.retrieve(
            user_text,
            knowledge_retrieve_options_s{
                    .workplace_role = m_config.workplace_role,
                    .include_general = true,
                    .include_custom = true,
                    .limit = m_config.max_knowledge_documents,
                    .max_chars_per_document = m_config.max_knowledge_chars_per_document,
                    .min_ranked_score = m_config.min_ranked_knowledge_score,
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

        const auto user_id = m_history[user_index].id;

        m_history[user_index].answer_kind = chat_answer_kind_e::direct_knowledge;

        m_history[user_index].status = chat_message_status_e::completed;

        m_history[user_index].relatives.clear();

        auto assistant_relatives = std::vector<std::uint64_t>{
                user_id,
        };

        const auto assistant_id = make_next_chat_entry_id(m_history);

        m_history.push_back(chat_history_entry_s{
                .id = assistant_id,
                .answer_kind = chat_answer_kind_e::direct_knowledge,
                .status = chat_message_status_e::completed,
                .user = std::nullopt,
                .assistant =
                        chat_visible_message_s{
                                .content = answer,
                                .created_at = util::make_local_timestamp(),
                                .model_content = answer_body,
                                .name = "AI-бот",
                        },
                .source_files = source_filenames,
                .relatives = assistant_relatives,
        });

        m_model_relative_ids = std::move(assistant_relatives);
        m_model_relative_ids.push_back(assistant_id);

        save_history();

        m_logger->info("Answered without LLM by direct knowledge match");

        return engine_answer_s{
                .status = chat_message_status_e::completed,
                .content = std::move(answer),
        };
    }

    if (!m_server.is_running()) {
        m_history[user_index].status = chat_message_status_e::failed;

        save_history();

        throw std::runtime_error{"Cannot generate answer: llama-server is not ready"};
    }

    m_history[user_index].answer_kind = chat_answer_kind_e::llm;
    m_history[user_index].relatives = m_model_relative_ids;

    save_history();

    auto response = llm::llama_client_response_s{};

    try {
        const auto request_messages = build_request_messages(m_history[user_index], knowledge);

        response = m_client.complete_chat(request_messages, stream_callback);
    } catch (...) {
        m_history[user_index].status = chat_message_status_e::failed;

        save_history();
        throw;
    }

    if (response.status == llm::llama_completion_status_e::cancelled) {
        m_history[user_index].status = chat_message_status_e::cancelled;

        save_history();

        return engine_answer_s{
                .status = chat_message_status_e::cancelled,
                .content = std::move(response.content),
        };
    }

    auto source_filenames = make_context_source_filenames(knowledge, m_history[user_index].relatives);

    auto answer_body = remove_generated_service_lines(response.content);

    if (answer_body.empty()) {
        answer_body = "Не удалось получить содержательный ответ от модели.";
    }

    auto answer = ensure_sources_block(answer_body, source_filenames);

    const auto user_id = m_history[user_index].id;

    auto assistant_relatives = m_history[user_index].relatives;

    assistant_relatives.push_back(user_id);

    m_history[user_index].status = chat_message_status_e::completed;

    const auto assistant_id = make_next_chat_entry_id(m_history);

    m_history.push_back(chat_history_entry_s{
            .id = assistant_id,
            .answer_kind = chat_answer_kind_e::llm,
            .status = chat_message_status_e::completed,
            .user = std::nullopt,
            .assistant =
                    chat_visible_message_s{
                            .content = answer,
                            .created_at = util::make_local_timestamp(),
                            .model_content = answer_body,
                            .name = "AI-бот",
                    },
            .source_files = source_filenames,
            .relatives = assistant_relatives,
    });

    m_model_relative_ids = std::move(assistant_relatives);
    m_model_relative_ids.push_back(assistant_id);

    save_history();

    return engine_answer_s{
            .status = chat_message_status_e::completed,
            .content = std::move(answer),
    };
}

void LlamaEngine::stop_generating() noexcept { m_server.stop_generating(); }

bool LlamaEngine::is_running() const noexcept { return m_server.is_running(); }

bool LlamaEngine::model_generates() const noexcept { return m_server.model_generates(); }

bool LlamaEngine::change_model(const llm::model_e model) { return m_server.change_model(model); }

void LlamaEngine::store_model_cache() { m_server.store_model_cache(); }

void LlamaEngine::load_model_cache() { m_server.load_model_cache(); }

std::string LlamaEngine::server_url() const { return m_server.url(); }

llm::llama_server_state_info_s LlamaEngine::server_state() const { return m_server.state_info(); }

std::span<const chat_history_entry_s> LlamaEngine::history() const noexcept {
    return {
            m_history.data(),
            m_history.size(),
    };
}

void LlamaEngine::load_history() { m_history = load_chat_history(m_config.history_file, m_history_logger); }

void LlamaEngine::save_history() const { save_chat_history(m_config.history_file, m_history); }

void LlamaEngine::rebuild_model_relatives() {
    m_model_relative_ids.clear();

    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (!is_completed_assistant_entry(*it)) {
            continue;
        }

        m_model_relative_ids = it->relatives;
        m_model_relative_ids.push_back(it->id);
        return;
    }
}

const chat_history_entry_s *LlamaEngine::find_history_entry(const std::uint64_t id) const noexcept {
    const auto it = std::ranges::find_if(m_history,
                                         [id](const chat_history_entry_s &entry) noexcept { return entry.id == id; });

    if (it == m_history.end()) {
        return nullptr;
    }

    return &*it;
}

std::size_t LlamaEngine::append_pending_user_entry(const std::string_view user_text) {
    const auto id = make_next_chat_entry_id(m_history);

    m_history.push_back(chat_history_entry_s{
            .id = id,
            .answer_kind = chat_answer_kind_e::unknown,
            .status = chat_message_status_e::pending,
            .user =
                    chat_visible_message_s{
                            .content = std::string{user_text},
                            .created_at = util::make_local_timestamp(),
                            .model_content = std::string{user_text},
                            .name = "Я",
                    },
            .assistant = std::nullopt,
            .source_files = {},
            .relatives = {},
    });

    return m_history.size() - 1;
}

std::string LlamaEngine::build_system_prompt(const std::span<const retrieved_knowledge_s> knowledge) const {
    auto prompt = std::format(
            "Ты — AI-помощник стажёра в сфере услуг.\n"
            "Роль стажёра: {}.\n"
            "Отвечай на русском языке, просто, понятно и по делу.\n"
            "Если вопрос не относится к работе стажёра, обслуживанию клиентов или доступной базе знаний, "
            "не отвечай по теме вопроса, сразу отказывай. Скажи, что бот помогает только по рабочим вопросам стажёра.\n"
            "Используй найденные Markdown-фрагменты как главный источник регламента.\n"
            "Если точного ответа в фрагментах нет, прямо скажи: точный регламент не найден "
            "и составь ответ из своей базы знаний.\n"
            "Не выдумывай шаги, правила компании, скидки, возвраты, компенсации и гарантии.\n"
            "Формат: полноценный список понятных шагов из файла.\n",
            to_string(m_config.workplace_role));

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
        const chat_history_entry_s &current_user_entry,
        const std::span<const retrieved_knowledge_s> knowledge) const {
    auto messages = std::vector<chat_message_s>{};

    messages.push_back(chat_message_s{
            .role = chat_role_e::system,
            .name = "system",
            .content = build_system_prompt(knowledge),
            .created_at = util::make_local_timestamp(),
            .source_files = {},
    });

    for (const auto relative_id : current_user_entry.relatives) {
        const auto *entry = find_history_entry(relative_id);

        if (entry == nullptr || !is_completed_model_visible_entry(*entry)) {
            continue;
        }

        if (entry->user.has_value() && !util::is_blank(entry->user->model_content)) {
            messages.push_back(chat_message_s{
                    .role = chat_role_e::user,
                    .name = entry->user->name,
                    .content = entry->user->model_content,
                    .created_at = entry->user->created_at,
                    .source_files = entry->source_files,
            });

            continue;
        }

        if (entry->assistant.has_value() && !util::is_blank(entry->assistant->model_content)) {
            messages.push_back(chat_message_s{
                    .role = chat_role_e::assistant,
                    .name = entry->assistant->name,
                    .content = entry->assistant->model_content,
                    .created_at = entry->assistant->created_at,
                    .source_files = entry->source_files,
            });
        }
    }

    if (!current_user_entry.user.has_value()) {
        throw std::runtime_error{"Current chat history entry is not a user message"};
    }

    messages.push_back(chat_message_s{
            .role = chat_role_e::user,
            .name = current_user_entry.user->name,
            .content = current_user_entry.user->model_content,
            .created_at = current_user_entry.user->created_at,
            .source_files = {},
    });

    return messages;
}

std::vector<std::string> LlamaEngine::make_context_source_filenames(
        const std::span<const retrieved_knowledge_s> knowledge,
        const std::span<const std::uint64_t> relatives) const {
    auto result = make_source_filenames(knowledge);

    if (!result.empty()) {
        return result;
    }

    return make_source_filenames_from_relatives(relatives);
}

std::vector<std::string> LlamaEngine::make_source_filenames_from_relatives(
        const std::span<const std::uint64_t> relatives) const {
    auto result = std::vector<std::string>{};

    for (const auto relative_id : relatives) {
        const auto *entry = find_history_entry(relative_id);

        if (entry == nullptr) {
            continue;
        }

        append_unique_source_files(result, entry->source_files);
    }

    return result;
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

    const auto match = knowledge.front().match;

    return match == knowledge_match_e::exact_frequent_query || match == knowledge_match_e::unordered_frequent_query ||
           match == knowledge_match_e::unordered_fuzzy_frequent_query;
}

std::string LlamaEngine::make_direct_answer(const std::span<const retrieved_knowledge_s> knowledge) {
    assert(can_answer_without_llm(knowledge));

    return extract_direct_answer_section(knowledge.front().content);
}

} // namespace stz::intern