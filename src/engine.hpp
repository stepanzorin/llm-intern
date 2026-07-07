// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <atomic>
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

#include "assistant_profile.hpp"
#include "chat_history.hpp"
#include "knowledge_storage.hpp"
#include "llm/llama_client.hpp"
#include "llm/llama_server.hpp"
#include "organization_config.hpp"

namespace stz::intern {

struct engine_config_s {
    llm::llama_server_config_s server = {};
    llm::llama_client_config_s client = {};

    // Existing history_file remains the workflow history for backward compatibility.
    std::filesystem::path history_file = "message_history.json";
    std::filesystem::path texting_history_file = "message_history_texting.json";
    std::filesystem::path assistant_profile_state_file = "backend/assistant_profile.json";
    std::filesystem::path organization_config_file = "organization_config.json";
    std::filesystem::path knowledge_directory = "knowledge";

    assistant_profile_e initial_profile = assistant_profile_e::workflow;
    workplace_role_e workplace_role = workplace_role_e::general;
    texting_style_e texting_style = texting_style_e::neutral;

    std::size_t max_knowledge_documents = 2;
    std::size_t max_knowledge_chars_per_document = 2500;
    std::size_t min_ranked_knowledge_score = 512;

    // Texting uses two bounded LLM passes when a script is found:
    // scenario selection and a surgical edit plan. The final response is assembled on CPU
    // from the original corporate script, so untouched text stays byte-for-byte unchanged.
    std::size_t max_texting_selected_scripts = 3;
    std::size_t max_texting_selector_input_chars = 1200;
    std::int32_t max_texting_selector_tokens = 96;

    std::size_t max_texting_adaptation_input_chars = 1200;
    std::size_t max_texting_adaptation_chars_per_script = 1200;
    std::size_t max_texting_script_edits = 5;
    std::int32_t max_texting_adaptation_tokens = 256;

    std::int32_t max_texting_answer_tokens = 448;
    std::size_t max_texting_prompt_chars_per_script = 1800;

    double texting_formal_temperature = 0.20;
    double texting_neutral_temperature = 0.30;
    double texting_friendly_temperature = 0.45;
    double texting_top_p = 0.90;

    // The full retrieved text remains available for direct answers; only the LLM prompt is shortened.
    std::size_t max_prompt_knowledge_chars_per_document = 1200;
    std::size_t max_expansion_knowledge_chars_per_document = 700;

    std::size_t max_context_user_messages = 3;
    std::size_t max_context_chars_per_user_message = 240;
    std::size_t max_context_answer_excerpt_chars = 480;
    std::size_t max_context_source_files = 2;

    std::size_t max_contextual_retrieval_chars = 700;
    std::size_t max_transform_answer_chars = 1200;
};


struct assistant_context_s {
    assistant_context_s(assistant_profile_e profile,
                        std::filesystem::path history_file,
                        std::filesystem::path knowledge_directory,
                        workplace_role_e workplace_role,
                        const std::shared_ptr<spdlog::logger> &logger);

    assistant_context_s(const assistant_context_s &) = delete;
    assistant_context_s &operator=(const assistant_context_s &) = delete;

    assistant_context_s(assistant_context_s &&) noexcept = default;
    assistant_context_s &operator=(assistant_context_s &&) noexcept = default;

    assistant_profile_e profile = assistant_profile_e::workflow;
    std::filesystem::path history_file = {};
    workplace_role_e workplace_role = workplace_role_e::general;

    std::shared_ptr<spdlog::logger> history_logger = {};

    std::vector<chat_history_entry_s> history = {};
    std::vector<std::uint64_t> last_topic_anchor_ids = {};
    std::optional<chat_context_state_s> context_state = std::nullopt;

    KnowledgeStorage knowledge;

    bool loaded = false;
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

    [[nodiscard]] assistant_profile_e active_profile() const noexcept;

    [[nodiscard]] bool change_profile(assistant_profile_e profile);

    [[nodiscard]] bool change_model(llm::model_e model);

    void store_model_cache();

    void load_model_cache();

    [[nodiscard]] std::string server_url() const;

    [[nodiscard]] llm::llama_server_state_info_s server_state() const;

    [[nodiscard]] std::span<const chat_history_entry_s> history() const noexcept;

    void clear_history();

private:
    [[nodiscard]] engine_answer_s ask_workflow(
            std::string_view user_text,
            const llm::llama_stream_callback_t &stream_callback);

    [[nodiscard]] engine_answer_s ask_texting(
            std::string_view user_text,
            const llm::llama_stream_callback_t &stream_callback);

    [[nodiscard]] std::optional<engine_answer_s> try_answer_from_organization_config(
            std::string_view user_text);

    void load_organization_config();

    void reload_organization_config_if_changed();

    void load_context(assistant_context_s &context);

    void save_history() const;

    void load_active_profile();

    void save_active_profile() const;

    void finalize_interrupted_history_entries(assistant_context_s &context);

    void rebuild_last_topic_anchor(assistant_context_s &context);

    [[nodiscard]] bool contexts_loaded() const noexcept;

    [[nodiscard]] assistant_context_s &active_context() noexcept;

    [[nodiscard]] const assistant_context_s &active_context() const noexcept;

    [[nodiscard]] const chat_history_entry_s *find_history_entry(std::uint64_t id) const noexcept;

    [[nodiscard]] std::size_t append_pending_user_entry(std::string_view user_text);

    [[nodiscard]] std::string build_system_prompt(std::span<const retrieved_knowledge_s> knowledge) const;

    [[nodiscard]] std::string build_texting_adaptation_system_prompt(
            std::span<const retrieved_knowledge_s> knowledge) const;

    [[nodiscard]] std::string build_texting_system_prompt(
            std::span<const retrieved_knowledge_s> knowledge) const;

    [[nodiscard]] std::string build_knowledge_base_block(
            std::span<const retrieved_knowledge_s> knowledge,
            std::size_t max_chars_per_document) const;

    [[nodiscard]] std::string build_context_state_prompt() const;

    [[nodiscard]] std::string build_previous_answer_transform_prompt(
            std::string_view user_text,
            std::span<const retrieved_knowledge_s> knowledge) const;

    [[nodiscard]] std::string build_contextual_retrieval_query(std::string_view user_text) const;

    [[nodiscard]] std::string previous_answer_for_transform() const;

    [[nodiscard]] chat_context_state_s make_context_state(
            bool follow_up,
            bool remember_user_message,
            std::string_view user_text,
            std::string_view answer_body,
            std::span<const std::string> source_filenames,
            std::span<const retrieved_knowledge_s> knowledge) const;

    [[nodiscard]] std::vector<chat_message_s> build_request_messages(
            const chat_history_entry_s &current_user_entry,
            std::span<const retrieved_knowledge_s> knowledge,
            bool transform_previous_answer) const;

    [[nodiscard]] std::vector<std::string> make_context_source_filenames(
            std::span<const retrieved_knowledge_s> knowledge) const;

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

    llm::LlamaServer m_server;
    llm::LlamaClient m_client;

    assistant_context_s m_active_context;
    assistant_context_s m_inactive_context;

    organization_config_s m_organization_config = {};
    std::optional<std::filesystem::file_time_type> m_organization_config_write_time = std::nullopt;

    std::atomic_bool m_request_active = false;
};

} // namespace stz::intern
