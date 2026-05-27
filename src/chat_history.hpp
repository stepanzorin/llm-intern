// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
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

[[nodiscard]] std::string_view to_string(chat_role_e role) noexcept;

[[nodiscard]] chat_role_e chat_role_from_string(std::string_view role);

struct chat_message_s {
    chat_role_e role = chat_role_e::user;
    std::string name = "Я";
    std::string content = {};
    std::string created_at = {};
    std::vector<std::string> source_files = {};
};

class ChatHistory final {
public:
    ChatHistory(std::filesystem::path filename, std::shared_ptr<spdlog::logger> logger);

    void load();

    void save() const;

    void append(chat_message_s message);

    void clear();

    [[nodiscard]] std::span<const chat_message_s> messages() const noexcept;

    [[nodiscard]] std::vector<chat_message_s> recent_messages(std::size_t max_count) const;

    [[nodiscard]] bool empty() const noexcept;

private:
    std::filesystem::path m_filename;
    std::vector<chat_message_s> m_messages;
    std::shared_ptr<spdlog::logger> m_logger;
};

} // namespace stz::intern