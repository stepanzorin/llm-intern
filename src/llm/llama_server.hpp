// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <spdlog/logger.h>

namespace stz::intern::llm {

enum class model_e {
    economy,
    standard,
    premium,
};

[[nodiscard]] std::string_view to_string(model_e model) noexcept;

[[nodiscard]] model_e model_from_string(std::string_view text);

struct llama_endpoint_config_s {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string chat_completions_path = "/v1/chat/completions";
    std::optional<std::string> api_key = {};
};

struct llama_model_config_s {
    model_e model = model_e::standard;

    std::filesystem::path filename = {};
    std::optional<std::string> alias = {};

    std::string cache_filename = {};
    std::vector<std::string> extra_arguments = {};
};

struct llama_server_config_s {
    std::filesystem::path application_directory = ".";
    std::filesystem::path executable = "backend/llama-server";

    std::filesystem::path log_file = "backend/server.log";

    llama_endpoint_config_s endpoint = {};

    std::vector<llama_model_config_s> models = {};
    model_e initial_model = model_e::standard;

    std::size_t threads = 24;
    std::size_t threads_batch = 32;
    std::size_t context_size = 2048;
    std::size_t parallel_slots = 1;
    std::size_t slot_id = 0;

    bool use_jinja = true;
    bool enable_prompt_cache = true;
    bool enable_slots = true;
    bool enable_metrics = true;
    bool disable_builtin_ui = true;
    bool allow_remote_connections = false;

    bool load_cache_on_start = true;
    bool store_cache_on_stop = true;

    std::filesystem::path slot_cache_directory = "cache/llama_slots";
    std::filesystem::path state_file = "state/llama_server_state.json";

    std::chrono::milliseconds startup_timeout = std::chrono::seconds{180};
    std::chrono::milliseconds health_poll_interval = std::chrono::milliseconds{250};
    std::chrono::milliseconds shutdown_timeout = std::chrono::seconds{5};
    std::chrono::milliseconds http_timeout = std::chrono::seconds{120};

    std::vector<std::string> extra_arguments = {};
};

[[nodiscard]] llama_server_config_s load_server_config(std::filesystem::path application_directory,
                                                       const std::filesystem::path &filename);

void validate_server_config(const llama_server_config_s &config);


struct llama_server_state_info_s {
    bool process_started = false;
    bool running = false;
    bool model_generates = false;

    model_e current_model = model_e::standard;
    std::string current_model_alias = {};

    std::string url = {};

    std::vector<std::string> models_used = {};

    std::uint64_t warning_count = {};
    std::uint64_t error_count = {};

    std::string started_at = {};
    std::string updated_at = {};
    std::string last_error = {};
};

namespace detail {

struct native_process_s;

} // namespace detail

class LlamaServer;

class LlamaServerSession final {
public:
    ~LlamaServerSession();

    LlamaServerSession(const LlamaServerSession &) = delete;
    LlamaServerSession &operator=(const LlamaServerSession &) = delete;

    LlamaServerSession(LlamaServerSession &&) = delete;
    LlamaServerSession &operator=(LlamaServerSession &&) = delete;

    [[nodiscard]] bool stop_requested() const noexcept;

    void request_stop() noexcept;

    [[nodiscard]] std::size_t slot_id() const noexcept;

private:
    friend class LlamaServer;

    LlamaServerSession(LlamaServer &server, std::uint64_t generation_id, std::size_t slot_id) noexcept;

    LlamaServer *m_server = nullptr;
    std::uint64_t m_generation_id = {};
    std::size_t m_slot_id = {};
};

using unique_llama_session_ptr = std::unique_ptr<LlamaServerSession>;

class LlamaServer final {
public:
    LlamaServer(llama_server_config_s config, std::shared_ptr<spdlog::logger> logger);

    ~LlamaServer();

    LlamaServer(const LlamaServer &) = delete;
    LlamaServer &operator=(const LlamaServer &) = delete;

    LlamaServer(LlamaServer &&) = delete;
    LlamaServer &operator=(LlamaServer &&) = delete;

    void start();

    void stop() noexcept;

    [[nodiscard]] bool is_running() const noexcept;

    [[nodiscard]] bool process_started() const noexcept;

    [[nodiscard]] bool model_generates() const noexcept;

    [[nodiscard]] std::string url() const;

    [[nodiscard]] llama_endpoint_config_s endpoint_config() const;

    [[nodiscard]] model_e current_model() const noexcept;

    [[nodiscard]] bool model_available(model_e model) const;

    [[nodiscard]] std::span<const llama_model_config_s> models() const noexcept;

    [[nodiscard]] unique_llama_session_ptr start_generation();

    void stop_generating() noexcept;

    [[nodiscard]] bool change_model(model_e model);

    void store_model_cache();

    void load_model_cache();

    void erase_model_cache();

    void update_server_state_info();

    [[nodiscard]] llama_server_state_info_s state_info() const;

private:
    llama_server_config_s m_config;

    std::atomic<model_e> m_current_model = model_e::standard;

    std::atomic<bool> m_process_started = {false};
    std::atomic<bool> m_is_running = {false};
    std::atomic<bool> m_is_stopping = {false};

    std::atomic<bool> m_model_generates = {false};
    std::atomic<bool> m_stop_generation_requested = {false};

    std::atomic<std::uint64_t> m_next_generation_id = {1};
    std::atomic<std::uint64_t> m_active_generation_id = {0};

    std::atomic<std::uint64_t> m_warning_count = {0};
    std::atomic<std::uint64_t> m_error_count = {0};

    std::unique_ptr<detail::native_process_s> m_process;
    std::jthread m_output_thread;

    mutable std::mutex m_lifecycle_mutex;
    mutable std::mutex m_state_mutex;

    std::vector<std::string> m_models_used;
    std::string m_started_at;
    std::string m_updated_at;
    std::string m_last_error;

    std::shared_ptr<spdlog::logger> m_logger;


    friend class LlamaServerSession;


    void start_unlocked();

    void stop_unlocked() noexcept;

    void terminate_process_unlocked() noexcept;

    void validate_configuration() const;

    [[nodiscard]] const llama_model_config_s &model_config(model_e model) const;

    [[nodiscard]] std::filesystem::path resolve_path(const std::filesystem::path &path) const;

    [[nodiscard]] std::filesystem::path executable_path() const;

    [[nodiscard]] std::filesystem::path model_path(const llama_model_config_s &model) const;

    [[nodiscard]] std::filesystem::path slot_cache_directory_path() const;

    [[nodiscard]] std::filesystem::path slot_cache_path(const llama_model_config_s &model) const;

    [[nodiscard]] std::filesystem::path state_file_path() const;

    [[nodiscard]] std::vector<std::string> make_server_arguments(const llama_model_config_s &model) const;

    [[nodiscard]] bool health_ready() const;

    void wait_until_ready();

    void store_model_cache_unlocked();

    void load_model_cache_unlocked();

    void erase_model_cache_unlocked();

    void perform_slot_action(std::string_view action, const llama_model_config_s &model);

    void read_server_output();

    void log_server_line(std::string line);

    void load_previous_state_info();

    void append_used_model(const llama_model_config_s &model);

    void set_last_error(std::string message);

    void log_warning(std::string message);

    void log_error(std::string message);

    void attach_file_log_sink();

    void finish_generation(std::uint64_t generation_id) noexcept;

    [[nodiscard]] bool generation_stop_requested(std::uint64_t generation_id) const noexcept;

    void request_generation_stop(std::uint64_t generation_id) noexcept;
};

} // namespace stz::intern::llm