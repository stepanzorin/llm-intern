#include "chat_history.hpp"

#include <cassert>
#include <format>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "util/file_io.hpp"

namespace stz::intern {

namespace {

using json = nlohmann::json;

[[nodiscard]] json message_to_json(const chat_message_s &message) {
    return json{
            {"role", std::string{to_string(message.role)}},
            {"name", message.name},
            {"content", message.content},
            {"created_at", message.created_at},
            {"source_files", message.source_files},
    };
}

[[nodiscard]] chat_message_s message_from_json(const json &object) {
    auto message = chat_message_s{};

    message.role = chat_role_from_string(object.at("role").get<std::string>());
    message.name = object.value("name", message.role == chat_role_e::assistant ? "AI-бот" : "Я");
    message.content = object.value("content", "");
    message.created_at = object.value("created_at", "");

    if (const auto it = object.find("source_files"); it != object.end() && it->is_array()) {
        message.source_files = it->get<std::vector<std::string>>();
    }

    return message;
}

} // namespace

std::string_view to_string(const chat_role_e role) noexcept {
    switch (role) {
        case chat_role_e::system: return "system";
        case chat_role_e::user: return "user";
        case chat_role_e::assistant: return "assistant";
    }

    return "user";
}

chat_role_e chat_role_from_string(std::string_view role) {
    if (role == "system") {
        return chat_role_e::system;
    }

    if (role == "user") {
        return chat_role_e::user;
    }

    if (role == "assistant") {
        return chat_role_e::assistant;
    }

    throw std::runtime_error{std::format("Unknown chat role '{}'", role)};
}

ChatHistory::ChatHistory(std::filesystem::path filename, std::shared_ptr<spdlog::logger> logger)
    : m_filename{std::move(filename)},
      m_logger{std::move(logger)} {
    assert(!m_filename.empty());
    assert(m_logger != nullptr);
}

void ChatHistory::load() {
    m_messages.clear();

    if (!std::filesystem::exists(m_filename)) {
        m_logger->info("Chat history file does not exist yet: {}", m_filename.string());
        return;
    }

    const auto content = util::read_text_file(m_filename);

    if (content.empty()) {
        m_logger->warn("Chat history file is empty: {}", m_filename.string());
        return;
    }

    try {
        const auto root = json::parse(content);
        const auto *messages_json = &root;

        if (root.is_object()) {
            messages_json = &root.at("messages");
        }

        if (!messages_json->is_array()) {
            throw std::runtime_error{"History messages must be JSON array"};
        }

        for (const auto &item : *messages_json) {
            m_messages.push_back(message_from_json(item));
        }

        m_logger->info("Loaded {} chat messages from '{}'", m_messages.size(), m_filename.string());
    } catch (const std::exception &error) {
        throw std::runtime_error{
                std::format("Failed to parse chat history '{}': {}", m_filename.string(), error.what())};
    }
}

void ChatHistory::save() const {
    auto messages_array = json::array();
    messages_array.get_ptr<json::array_t *>()->reserve(m_messages.size());

    for (const auto &message : m_messages) {
        messages_array.push_back(message_to_json(message));
    }

    const auto output = json{{"version", 1}, {"messages", std::move(messages_array)}};

    util::write_text_file_atomic(m_filename, output.dump(2, ' ', false));
}

void ChatHistory::append(chat_message_s message) { m_messages.push_back(std::move(message)); }

void ChatHistory::clear() { m_messages.clear(); }

std::span<const chat_message_s> ChatHistory::messages() const noexcept {
    return {m_messages.data(), m_messages.size()};
}

std::vector<chat_message_s> ChatHistory::recent_messages(const std::size_t max_count) const {
    if (max_count >= m_messages.size()) {
        return m_messages;
    }

    const auto first = m_messages.end() - static_cast<std::ptrdiff_t>(max_count);

    return {first, m_messages.end()};
}

bool ChatHistory::empty() const noexcept { return m_messages.empty(); }

} // namespace stz::intern