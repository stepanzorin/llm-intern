// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/logger.h>

#include "chat_history.hpp"
#include "config.hpp"
#include "knowledge_storage.hpp"
#include "llm/llama_client.hpp"

namespace stz::intern {

struct engine_config_s {
    std::filesystem::path history_file = "message_history.json";
    std::filesystem::path knowledge_directory = "knowledge";

    std::size_t max_history_messages_for_request = 24;
    std::size_t max_knowledge_documents = 3;
    std::size_t max_knowledge_chars_per_document = 6000;
};

class LlamaEngine final {
public:
    LlamaEngine(llama_server_config_s server_config,
                engine_config_s engine_config,
                const std::shared_ptr<spdlog::logger> &logger);

    void load();

    [[nodiscard]] std::string ask(std::string_view user_text);

    [[nodiscard]] const ChatHistory &history() const noexcept;

private:
    [[nodiscard]] static std::string build_system_prompt(std::span<const retrieved_knowledge_s> knowledge);

    [[nodiscard]] std::vector<chat_message_s> build_request_messages(
            std::span<const retrieved_knowledge_s> knowledge) const;

    [[nodiscard]] static std::vector<std::string> make_source_filenames(
            std::span<const retrieved_knowledge_s> knowledge);

    [[nodiscard]] static std::string ensure_sources_block(const std::string& answer,
                                                          std::span<const std::string> source_filenames);

    llama_server_config_s m_server_config;
    engine_config_s m_engine_config;

    ChatHistory m_history;
    KnowledgeStorage m_knowledge;
    LlamaClient m_client;

    std::shared_ptr<spdlog::logger> m_logger;
};

} // namespace stz::intern