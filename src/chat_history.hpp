// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/logger.h>

namespace stz::intern {

enum class chat_role_e {
    system,
    user,
    assistant,
};

enum class chat_answer_kind_e {
    unknown,
    direct_knowledge,
    llm,
    policy_rejection,
};

enum class chat_message_status_e {
    pending,
    completed,
    cancelled,
    failed,
};

[[nodiscard]] std::string_view to_string(chat_role_e role) noexcept;

[[nodiscard]] std::string_view to_string(chat_answer_kind_e answer_kind) noexcept;

[[nodiscard]] std::string_view to_string(chat_message_status_e status) noexcept;

[[nodiscard]] chat_role_e chat_role_from_string(std::string_view role);

[[nodiscard]] chat_answer_kind_e chat_answer_kind_from_string(std::string_view answer_kind);

[[nodiscard]] chat_message_status_e chat_message_status_from_string(std::string_view status);

[[nodiscard]] std::string make_chat_model_content(std::string_view text, chat_role_e role);

struct chat_message_s {
    chat_role_e role = chat_role_e::user;
    std::string name = "Я";
    std::string content = {};
    std::string created_at = {};
    std::vector<std::string> source_files = {};
};

struct chat_visible_message_s {
    std::string content = {};
    std::string created_at = {};
    std::string model_content = {};
    std::string name = {};
};

struct chat_context_state_s {
    std::string topic = {};
    std::vector<std::string> recent_user_messages = {};
    std::string last_answer_excerpt = {};
    std::vector<std::string> source_files = {};
};

struct chat_history_entry_s {
    std::uint64_t id = {};
    chat_answer_kind_e answer_kind = chat_answer_kind_e::unknown;
    chat_message_status_e status = chat_message_status_e::pending;

    std::optional<chat_visible_message_s> user = std::nullopt;
    std::optional<chat_visible_message_s> assistant = std::nullopt;

    std::vector<std::string> source_files = {};
    std::vector<std::uint64_t> relatives = {};

    std::optional<chat_context_state_s> context_state = std::nullopt;
};

[[nodiscard]] std::vector<chat_history_entry_s> load_chat_history(const std::filesystem::path &filename,
                                                                  const std::shared_ptr<spdlog::logger> &logger);

void save_chat_history(const std::filesystem::path &filename, std::span<const chat_history_entry_s> entries);

[[nodiscard]] std::uint64_t make_next_chat_entry_id(std::span<const chat_history_entry_s> entries) noexcept;

} // namespace stz::intern