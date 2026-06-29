// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/common.h>
#include <spdlog/logger.h>

#include "assistant_profile.hpp"
#include "chat_history.hpp"
#include "engine.hpp"
#include "web_server_config.hpp"

namespace stz::intern {

class WebServer;

enum class subscription_plan_e {
    free,
    plus,
    premium,
};

enum class application_access_state_e {
    inactive,
    active,
    offline_grace_period,
    expired,
};

[[nodiscard]] std::string_view to_string(subscription_plan_e plan) noexcept;

[[nodiscard]] std::string_view to_string(application_access_state_e state) noexcept;

struct auth_config_s {
    /*
     * Авторизация пока не реализована.
     *
     * При enabled == false приложение работает локально
     * без удалённой проверки.
     */
    bool enabled = false;
    bool is_available = true;
    std::string login = {};
    std::string password = {};

    /*
     * Временная заглушка для разработки.
     *
     * При true приложение считается активированным,
     * а пользователю назначается тариф FREE.
     */
    bool development_bypass = true;

    std::filesystem::path state_file = "backend/auth_state.json";

    std::string validation_url = {};

    std::chrono::minutes validation_interval = std::chrono::minutes{60};
    std::chrono::hours offline_grace_period = std::chrono::hours{24};
};

struct database_config_s {
    /*
     * База данных пока не используется.
     * Поле оставлено для будущей реализации.
     */
    bool enabled = false;

    std::string host = "127.0.0.1";
    std::uint16_t port = 5432;
    std::string database = "intern";
    std::string user = "intern";
    std::string password = {};

    std::filesystem::path filename = "backend/application.db";
};

struct application_identity_config_s {
    std::string company_name = "Название компании";
    std::string point_name = {};
    std::string point_address = {};

    std::string application_version = "0.1.0";
};

struct application_logging_config_s {
    std::filesystem::path filename = "backend/application.log";

    spdlog::level::level_enum level = spdlog::level::info;

    bool write_to_console = true;
    bool truncate_file = false;
};

struct app_config_s {
    /*
     * Конфигурация процесса llama-server.
     */
    llm::llama_server_config_s llama_server = {};

    /*
     * Конфигурация локального HTTP-сервера WebUI.
     */
    web_server_config_s web_server = {};

    /*
     * Конфигурация RAG, истории и роли пользователя.
     */
    engine_config_s engine = {};

    /*
     * Пока используются как заглушки.
     */
    auth_config_s auth = {};
    database_config_s database = {};

    application_identity_config_s identity = {};
    application_logging_config_s logging = {};
};

struct application_state_s {
    bool application_running = false;
    bool activated = false;

    application_access_state_e access_state = application_access_state_e::inactive;

    subscription_plan_e subscription = subscription_plan_e::free;

    bool llama_server_running = false;
    bool model_generates = false;

    std::string company_name = {};
    std::string point_name = {};
    std::string point_address = {};
    std::string application_version = {};
};

struct activation_request_s {
    std::string point_name = {};
    std::string point_address = {};
    std::string activation_key = {};
};

struct report_request_s {
    std::string description = {};
};

struct application_operation_result_s {
    bool success = false;
    std::string message = {};
};

class Application final {
public:
    explicit Application(app_config_s config);

    ~Application();

    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;

    Application(Application &&) = delete;
    Application &operator=(Application &&) = delete;

    /*
     * Preconditions:
     * - run() вызывается не более одного раза.
     */
    [[nodiscard]] int run();

    /*
     * Безопасно прерывает WebServer::wait().
     *
     * Полная остановка компонентов выполняется в run().
     */
    void request_stop() noexcept;

    /*
     * API, которым будет пользоваться WebServer.
     *
     * WebServer не должен получать прямой доступ
     * к LlamaEngine или LlamaServer.
     */
    [[nodiscard]] std::string ask(std::string_view user_text);

    void stop_generating() noexcept;

    [[nodiscard]] application_operation_result_s restart_model_server();

    [[nodiscard]] assistant_profile_e assistant_profile() const noexcept;

    [[nodiscard]] bool change_assistant_profile(assistant_profile_e profile);

    [[nodiscard]] std::vector<chat_history_entry_s> history_snapshot() const;

    void clear_history();

    [[nodiscard]] application_state_s state() const;

    [[nodiscard]] bool can_use_chat() const noexcept;

    [[nodiscard]] std::string_view initial_page() const noexcept;

    /*
     * Заглушки для будущих удалённых сервисов.
     */
    [[nodiscard]] application_operation_result_s activate(const activation_request_s &request);

    [[nodiscard]] application_operation_result_s submit_report(const report_request_s &request);

    void refresh_subscription_state();

private:
    [[nodiscard]] static std::shared_ptr<spdlog::logger> create_logger(const application_logging_config_s &config);

    static void validate_config(const app_config_s &config);

    void initialize_authentication();

    void initialize_database();

    void start_components();

    void stop_components() noexcept;

    void ensure_chat_access() const;

    app_config_s m_config;

    std::shared_ptr<spdlog::logger> m_logger;

    /*
     * Порядок объявления важен:
     * WebServer уничтожается раньше LlamaEngine.
     */
    std::unique_ptr<LlamaEngine> m_engine;
    std::unique_ptr<WebServer> m_web_server;

    /*
     * Защищает историю и остальные изменяемые данные LlamaEngine.
     *
     * stop_generating() намеренно не использует этот mutex,
     * иначе запрос остановки не сможет прервать долгую генерацию.
     */
    mutable std::mutex m_engine_mutex;
    mutable std::mutex m_state_mutex;

    std::atomic_bool m_run_called = false;
    std::atomic_bool m_is_running = false;
    std::atomic_bool m_activated = false;

    std::atomic<application_access_state_e> m_access_state = application_access_state_e::inactive;

    std::atomic<subscription_plan_e> m_subscription = subscription_plan_e::free;
    std::atomic<assistant_profile_e> m_assistant_profile = assistant_profile_e::workflow;
};

} // namespace stz::intern