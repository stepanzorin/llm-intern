#include "chat_history.hpp"

#include <algorithm>
#include <cassert>
#include <format>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "util/file_io.hpp"

namespace stz::intern {

namespace {

using json = nlohmann::json;

[[nodiscard]] json visible_message_to_json(const chat_visible_message_s &message) {
    return json{
            {"content", message.content},
            {"created_at", message.created_at},
            {"model_content", message.model_content},
            {"name", message.name},
    };
}

[[nodiscard]] chat_visible_message_s visible_message_from_json(const json &object,
                                                               const std::string_view fallback_name) {
    auto message = chat_visible_message_s{};

    message.content = object.value("content", "");
    message.created_at = object.value("created_at", "");
    message.model_content = object.value("model_content", message.content);
    message.name = object.value("name", std::string{fallback_name});

    return message;
}

[[nodiscard]] json history_entry_to_json(const chat_history_entry_s &entry) {
    auto object = json{
            {"id", entry.id},
            {"answer_kind", std::string{to_string(entry.answer_kind)}},
            {"status", std::string{to_string(entry.status)}},
            {"source_files", entry.source_files},
            {"relatives", entry.relatives},
    };

    if (entry.user.has_value()) {
        object["user"] = visible_message_to_json(*entry.user);
    }

    if (entry.assistant.has_value()) {
        object["assistant"] = visible_message_to_json(*entry.assistant);
    }

    return object;
}

[[nodiscard]] chat_history_entry_s history_entry_from_json(const json &object) {
    auto entry = chat_history_entry_s{};

    entry.id = object.value("id", std::uint64_t{});
    entry.answer_kind = chat_answer_kind_from_string(object.value("answer_kind", "unknown"));
    entry.status = chat_message_status_from_string(object.value("status", "pending"));

    if (const auto it = object.find("source_files"); it != object.end() && it->is_array()) {
        entry.source_files = it->get<std::vector<std::string>>();
    }

    if (const auto it = object.find("relatives"); it != object.end() && it->is_array()) {
        entry.relatives = it->get<std::vector<std::uint64_t>>();
    }

    if (const auto it = object.find("user"); it != object.end() && it->is_object()) {
        entry.user = visible_message_from_json(*it, "Я");
    }

    if (const auto it = object.find("assistant"); it != object.end() && it->is_object()) {
        entry.assistant = visible_message_from_json(*it, "AI-бот");
    }

    if (!entry.user.has_value() && !entry.assistant.has_value()) {
        throw std::runtime_error{"History entry must contain user or assistant object"};
    }

    return entry;
}

[[nodiscard]] chat_history_entry_s legacy_history_entry_from_json(const json &object, const std::uint64_t id) {
    auto entry = chat_history_entry_s{};

    const auto role = chat_role_from_string(object.at("role").get<std::string>());

    entry.id = id;
    entry.answer_kind = chat_answer_kind_e::unknown;
    entry.status = chat_message_status_e::completed;

    if (const auto it = object.find("source_files"); it != object.end() && it->is_array()) {
        entry.source_files = it->get<std::vector<std::string>>();
    }

    auto visible_message = chat_visible_message_s{
            .content = object.value("content", ""),
            .created_at = object.value("created_at", ""),
            .model_content = object.value("content", ""),
            .name = object.value("name", role == chat_role_e::assistant ? "AI-бот" : "Я"),
    };

    if (role == chat_role_e::user) {
        entry.user = std::move(visible_message);
    } else if (role == chat_role_e::assistant) {
        entry.assistant = std::move(visible_message);
    } else {
        throw std::runtime_error{"Legacy system messages are not supported in visible chat history"};
    }

    return entry;
}

void fix_empty_or_duplicate_ids(std::vector<chat_history_entry_s> &entries) {
    auto used_ids = std::vector<std::uint64_t>{};
    used_ids.reserve(entries.size());

    auto next_id = std::uint64_t{1};

    for (auto &entry : entries) {
        if (entry.id == 0 || std::ranges::find(used_ids, entry.id) != used_ids.end()) {
            while (std::ranges::find(used_ids, next_id) != used_ids.end()) {
                ++next_id;
            }

            entry.id = next_id;
        }

        used_ids.push_back(entry.id);
        next_id = std::max(next_id, entry.id + 1);
    }
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

std::string_view to_string(const chat_answer_kind_e answer_kind) noexcept {
    switch (answer_kind) {
        case chat_answer_kind_e::unknown: return "unknown";
        case chat_answer_kind_e::direct_knowledge: return "direct_knowledge";
        case chat_answer_kind_e::llm: return "llm";
        case chat_answer_kind_e::policy_rejection: return "policy_rejection";
    }

    return "unknown";
}

std::string_view to_string(const chat_message_status_e status) noexcept {
    switch (status) {
        case chat_message_status_e::pending: return "pending";
        case chat_message_status_e::completed: return "completed";
        case chat_message_status_e::cancelled: return "cancelled";
        case chat_message_status_e::failed: return "failed";
    }

    return "pending";
}

chat_role_e chat_role_from_string(const std::string_view role) {
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

chat_answer_kind_e chat_answer_kind_from_string(const std::string_view answer_kind) {
    if (answer_kind == "unknown") {
        return chat_answer_kind_e::unknown;
    }

    if (answer_kind == "direct_knowledge") {
        return chat_answer_kind_e::direct_knowledge;
    }

    if (answer_kind == "llm") {
        return chat_answer_kind_e::llm;
    }

    if (answer_kind == "policy_rejection") {
        return chat_answer_kind_e::policy_rejection;
    }

    throw std::runtime_error{std::format("Unknown chat answer kind '{}'", answer_kind)};
}

chat_message_status_e chat_message_status_from_string(const std::string_view status) {
    if (status == "pending") {
        return chat_message_status_e::pending;
    }

    if (status == "completed") {
        return chat_message_status_e::completed;
    }

    if (status == "cancelled") {
        return chat_message_status_e::cancelled;
    }

    if (status == "failed") {
        return chat_message_status_e::failed;
    }

    throw std::runtime_error{std::format("Unknown chat message status '{}'", status)};
}

std::vector<chat_history_entry_s> load_chat_history(const std::filesystem::path &filename,
                                                    const std::shared_ptr<spdlog::logger> &logger) {
    assert(logger != nullptr);

    auto entries = std::vector<chat_history_entry_s>{};

    if (!std::filesystem::exists(filename)) {
        logger->info("Chat history file does not exist yet: {}", filename.string());
        return entries;
    }

    const auto content = util::read_text_file(filename);

    if (content.empty()) {
        logger->warn("Chat history file is empty: {}", filename.string());
        return entries;
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

        auto legacy_next_id = std::uint64_t{1};

        for (const auto &item : *messages_json) {
            if (item.contains("user") || item.contains("assistant")) {
                entries.push_back(history_entry_from_json(item));
                continue;
            }

            entries.push_back(legacy_history_entry_from_json(item, legacy_next_id));
            ++legacy_next_id;
        }

        fix_empty_or_duplicate_ids(entries);

        logger->info("Loaded {} chat history entries from '{}'", entries.size(), filename.string());

        return entries;
    } catch (const std::exception &error) {
        throw std::runtime_error{std::format("Failed to parse chat history '{}': {}", filename.string(), error.what())};
    }
}

void save_chat_history(const std::filesystem::path &filename, const std::span<const chat_history_entry_s> entries) {
    auto messages_array = json::array();
    messages_array.get_ptr<json::array_t *>()->reserve(entries.size());

    for (const auto &entry : entries) {
        messages_array.push_back(history_entry_to_json(entry));
    }

    const auto output = json{
            {"version", 3},
            {"messages", std::move(messages_array)},
    };

    util::write_text_file_atomic(filename, output.dump(2, ' ', false));
}

std::uint64_t make_next_chat_entry_id(const std::span<const chat_history_entry_s> entries) noexcept {
    auto max_id = std::uint64_t{};

    for (const auto &entry : entries) {
        max_id = std::max(max_id, entry.id);
    }

    return max_id + 1;
}

} // namespace stz::intern