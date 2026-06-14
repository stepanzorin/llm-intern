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
#include "knowledge_storage.hpp"
#include "llm/llama_client.hpp"
#include "llm/llama_server.hpp"

namespace stz::intern {

struct engine_config_s {
    llm::llama_server_config_s server = {};
    llm::llama_client_config_s client = {};

    std::filesystem::path history_file = "message_history.json";
    std::filesystem::path knowledge_directory = "knowledge";

    workplace_role_e workplace_role = workplace_role_e::general;

    std::size_t max_knowledge_documents = 2;
    std::size_t max_knowledge_chars_per_document = 2500;
    std::size_t min_ranked_knowledge_score = 512;
};

struct engine_answer_s {
    chat_message_status_e status = chat_message_status_e::completed;

    std::string content = {};
};

class LlamaEngine {
public:
    LlamaEngine(engine_config_s config,
                llm::llama_server_config_s server_config,
                const std::shared_ptr<spdlog::logger> &logger);

    LlamaEngine(const LlamaEngine &) = delete;
    LlamaEngine &operator=(const LlamaEngine &) = delete;

    LlamaEngine(LlamaEngine &&) = delete;
    LlamaEngine &operator=(LlamaEngine &&) = delete;

    void load();

    void start();

    void stop() noexcept;

    [[nodiscard]] engine_answer_s ask(std::string_view user_text,
                                      const llm::llama_stream_callback_t &stream_callback = {});

    void stop_generating() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    [[nodiscard]] bool model_generates() const noexcept;

    [[nodiscard]] bool change_model(llm::model_e model);

    void store_model_cache();

    void load_model_cache();

    [[nodiscard]] std::string server_url() const;

    [[nodiscard]] llm::llama_server_state_info_s server_state() const;

    [[nodiscard]] std::span<const chat_history_entry_s> history() const noexcept;

    void clear_history();

private:
    void load_history();

    void save_history() const;

    void finalize_interrupted_history_entries();

    void rebuild_last_topic_anchor();

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

    [[nodiscard]] static std::string ensure_sources_block(std::string answer,
                                                          std::span<const std::string> source_filenames);

    [[nodiscard]] static bool can_answer_without_llm(std::span<const retrieved_knowledge_s> knowledge) noexcept;

    [[nodiscard]] static std::string make_direct_answer(std::span<const retrieved_knowledge_s> knowledge);

    engine_config_s m_config;

    std::shared_ptr<spdlog::logger> m_logger;
    std::shared_ptr<spdlog::logger> m_history_logger;

    llm::LlamaServer m_server;
    llm::LlamaClient m_client;

    std::vector<chat_history_entry_s> m_history;
    std::vector<std::uint64_t> m_last_topic_anchor_ids;

    KnowledgeStorage m_knowledge;

    bool m_loaded = false;
};

} // namespace stz::intern