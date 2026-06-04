// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
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

    workplace_role_e workplace_role = workplace_role_e::general;

    std::size_t max_knowledge_documents = 2;
    std::size_t max_knowledge_chars_per_document = 2500;
    std::size_t min_ranked_knowledge_score = 512;
};

class LlamaEngine final {
public:
    LlamaEngine(llama_server_config_s server_config,
                engine_config_s engine_config,
                const std::shared_ptr<spdlog::logger> &logger);

    void load();

    [[nodiscard]] std::string ask(std::string_view user_text);

    [[nodiscard]] std::span<const chat_history_entry_s> history() const noexcept;

private:
    llama_server_config_s m_server_config;
    engine_config_s m_engine_config;

    std::vector<chat_history_entry_s> m_history;
    std::vector<std::uint64_t> m_model_relative_ids;

    KnowledgeStorage m_knowledge;
    LlamaClient m_client;

    std::shared_ptr<spdlog::logger> m_logger;
    std::shared_ptr<spdlog::logger> m_history_logger;


    void load_history();

    void save_history() const;

    void rebuild_model_relatives();

    [[nodiscard]] const chat_history_entry_s *find_history_entry(std::uint64_t id) const noexcept;

    [[nodiscard]] std::size_t append_pending_user_entry(std::string_view user_text);

    [[nodiscard]] std::string build_system_prompt(std::span<const retrieved_knowledge_s> knowledge) const;

    [[nodiscard]] std::vector<chat_message_s> build_request_messages(
            const chat_history_entry_s &current_user_entry,
            std::span<const retrieved_knowledge_s> knowledge) const;

    [[nodiscard]] std::vector<std::string> make_context_source_filenames(
            std::span<const retrieved_knowledge_s> knowledge,
            std::span<const std::uint64_t> relatives) const;

    [[nodiscard]] std::vector<std::string> make_source_filenames_from_relatives(
            std::span<const std::uint64_t> relatives) const;

    [[nodiscard]] static std::vector<std::string> make_source_filenames(
            std::span<const retrieved_knowledge_s> knowledge);

    [[nodiscard]] static std::string ensure_sources_block(const std::string &answer,
                                                          std::span<const std::string> source_filenames);

    [[nodiscard]] static bool can_answer_without_llm(std::span<const retrieved_knowledge_s> knowledge) noexcept;

    [[nodiscard]] static std::string make_direct_answer(std::span<const retrieved_knowledge_s> knowledge);
};

} // namespace stz::intern