#include "application.hpp"

#include <cstdio>
#include <print>
#include <string>

#include "util/console.hpp"
#include "util/string_helpers.hpp"

#ifndef CONSOLE_INPUT
    #define CONSOLE_INPUT 0
#endif

#include <cassert>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <utility>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "util/string_helpers.hpp"
#include "web_server.hpp"

namespace stz::intern {

namespace {

[[nodiscard]] bool access_state_allows_chat(const application_access_state_e state) noexcept {
    return state == application_access_state_e::active || state == application_access_state_e::offline_grace_period;
}

} // namespace

std::string_view to_string(const subscription_plan_e plan) noexcept {
    switch (plan) {
        case subscription_plan_e::free: return "FREE";
        case subscription_plan_e::plus: return "PLUS";
        case subscription_plan_e::premium: return "PREMIUM";
    }

    return "FREE";
}

std::string_view to_string(const application_access_state_e state) noexcept {
    switch (state) {
        case application_access_state_e::inactive: return "inactive";
        case application_access_state_e::active: return "active";
        case application_access_state_e::offline_grace_period: return "offline_grace_period";
        case application_access_state_e::expired: return "expired";
    }

    return "inactive";
}

Application::Application(app_config_s config) : m_config{std::move(config)}, m_logger{create_logger(m_config.logging)} {
    validate_config(m_config);

    m_engine = std::make_unique<LlamaEngine>(m_config.engine, m_config.llama_server, m_logger->clone("LlamaEngine"));
    m_web_server = std::make_unique<WebServer>(m_config.web_server, *this, m_logger->clone("WebServer"));
}

Application::~Application() { stop_components(); }

int Application::run() {
    const auto already_called = m_run_called.exchange(true);

    assert(!already_called);

    if (already_called) {
        std::terminate();
    }

    m_logger->info("Application is starting");

    try {
        initialize_database();
        initialize_authentication();

        start_components();

#if CONSOLE_INPUT
        m_logger->info("Console input mode is enabled");

        std::println("");
        std::println("Консольный режим запущен.");
        std::println("Команды: /exit или /quit — завершить приложение.");
        std::println("");

        while (m_is_running.load()) {
            std::print("> ");
            std::fflush(stdout);

            auto input = std::string{};

            if (!util::read_console_line_utf8(input)) {
                std::println("");
                break;
            }

            util::trim(input);

            if (input.empty()) {
                continue;
            }

            if (input == "/exit" || input == "/quit") {
                m_logger->info("Application stop requested from console");
                break;
            }

            try {
                auto answer = ask(input);

                std::println("");
                std::println("AI-бот:");
                std::println("{}", answer);
                std::println("");
            } catch (const std::exception &error) {
                m_logger->error("Failed to process console message: {}", error.what());

                std::println(stderr, "Ошибка: {}", error.what());

                std::println("");
            }
        }

        request_stop();
#else
        m_web_server->wait();
#endif

        stop_components();

        m_logger->info("Application finished");

        return EXIT_SUCCESS;
    } catch (...) {
        stop_components();
        throw;
    }
}

void Application::request_stop() noexcept {
    if (m_web_server == nullptr) {
        return;
    }

    try {
        m_logger->info("Application stop requested");
        m_web_server->stop();
    } catch (const std::exception &error) {
        m_logger->error("Failed to request WebServer stop: {}", error.what());
    } catch (...) {
        m_logger->error("Failed to request WebServer stop: unknown error");
    }
}

std::string Application::ask(const std::string_view user_text) {
    ensure_chat_access();

    if (util::is_blank(user_text)) {
        throw std::runtime_error{"User message is empty"};
    }

    const auto lock = std::unique_lock{m_engine_mutex, std::try_to_lock};

    if (!lock.owns_lock()) {
        throw std::runtime_error{"Another answer is already being generated"};
    }

    auto answer = m_engine->ask(user_text);

    if (answer.status == chat_message_status_e::cancelled) {
        m_logger->info("Answer generation was cancelled");

        if (answer.content.empty()) {
            return "Генерация ответа остановлена.";
        }
    }

    if (answer.status != chat_message_status_e::completed && answer.status != chat_message_status_e::cancelled) {
        throw std::runtime_error{std::format("Unexpected engine answer status: {}", to_string(answer.status))};
    }

    return std::move(answer.content);
}

void Application::stop_generating() noexcept {
    try {
        m_engine->stop_generating();
    } catch (const std::exception &error) {
        m_logger->error("Failed to stop generation: {}", error.what());
    } catch (...) {
        m_logger->error("Failed to stop generation: unknown error");
    }
}

application_operation_result_s Application::restart_model_server() {
    if (!can_use_chat()) {
        return application_operation_result_s{.success = false, .message = "Доступ к чату недоступен"};
    }

    const auto lock = std::unique_lock{m_engine_mutex, std::try_to_lock};

    if (!lock.owns_lock()) {
        return application_operation_result_s{.success = false,
                                              .message = "Нельзя перезапустить модель, пока формируется ответ"};
    }

    if (m_engine->model_generates()) {
        return application_operation_result_s{.success = false,
                                              .message = "Нельзя перезапустить модель во время генерации ответа"};
    }

    m_logger->info("Restarting llama-server by user request");

    try {
        m_engine->stop();
        m_engine->start();

        return application_operation_result_s{.success = true, .message = "Модель успешно перезапущена"};
    } catch (const std::exception &error) {
        m_logger->error("Failed to restart llama-server: {}", error.what());

        return application_operation_result_s{.success = false, .message = "Не удалось перезапустить модель"};
    } catch (...) {
        m_logger->error("Failed to restart llama-server: unknown error");

        return application_operation_result_s{.success = false, .message = "Не удалось перезапустить модель"};
    }
}

std::vector<chat_history_entry_s> Application::history_snapshot() const {
    auto lock = std::scoped_lock{m_engine_mutex};

    const auto history = m_engine->history();

    return {history.begin(), history.end()};
}

void Application::clear_history() {
    auto lock = std::scoped_lock{m_engine_mutex};

    m_engine->clear_history();

    m_logger->info("Chat history was cleared");
}

application_state_s Application::state() const {
    auto result = application_state_s{.application_running = m_is_running.load(),
                                      .activated = m_activated.load(),
                                      .access_state = m_access_state.load(),
                                      .subscription = m_subscription.load(),
                                      .llama_server_running = m_engine->is_running(),
                                      .model_generates = m_engine->model_generates(),
                                      .company_name = {},
                                      .point_name = {},
                                      .point_address = {},
                                      .application_version = {}};

    {
        auto lock = std::scoped_lock{m_state_mutex};

        result.company_name = m_config.identity.company_name;
        result.point_name = m_config.identity.point_name;
        result.point_address = m_config.identity.point_address;
        result.application_version = m_config.identity.application_version;
    }

    return result;
}

bool Application::can_use_chat() const noexcept {
    return m_activated.load() && access_state_allows_chat(m_access_state.load());
}

std::string_view Application::initial_page() const noexcept {
    if (m_access_state.load() == application_access_state_e::expired) {
        return "/session_expired.html";
    }

    if (!m_activated.load()) {
        return "/index.html";
    }

    return "/chat.html";
}

application_operation_result_s Application::activate(const activation_request_s &request) {
    if (util::is_blank(request.point_name)) {
        return application_operation_result_s{.success = false, .message = "Не указано название точки"};
    }

    if (util::is_blank(request.point_address)) {
        return application_operation_result_s{.success = false, .message = "Не указан адрес точки"};
    }

    if (util::is_blank(request.activation_key)) {
        return application_operation_result_s{.success = false, .message = "Не указан ключ активации"};
    }

    /*
     * Временная реализация для локальной разработки.
     */
    if (!m_config.auth.development_bypass) {
        m_logger->warn("Activation requested, but remote authentication is not implemented");

        return application_operation_result_s{.success = false, .message = "Удалённая активация пока не реализована"};
    }

    {
        auto lock = std::scoped_lock{m_state_mutex};

        m_config.identity.point_name = request.point_name;
        m_config.identity.point_address = request.point_address;
    }

    m_activated.store(true);
    m_access_state.store(application_access_state_e::active);
    m_subscription.store(subscription_plan_e::free);

    m_logger->info("Development activation completed for point '{}'", request.point_name);

    return application_operation_result_s{
            .success = true,
            .message = "Приложение активировано в режиме разработки",
    };
}

application_operation_result_s Application::submit_report(const report_request_s &request) {
    if (util::is_blank(request.description)) {
        return application_operation_result_s{
                .success = false,
                .message = "Описание проблемы пустое",
        };
    }

    m_logger->warn("Report submission is not implemented. Description size: {}", request.description.size());

    return application_operation_result_s{
            .success = false,
            .message = "Отправка отчётов пока не реализована",
    };
}

void Application::refresh_subscription_state() {
    if (m_config.auth.development_bypass) {
        m_activated.store(true);
        m_access_state.store(application_access_state_e::active);
        m_subscription.store(subscription_plan_e::free);

        m_logger->debug("Subscription state refreshed through development bypass");
        return;
    }

    m_logger->warn("Remote subscription validation is not implemented");
}

std::shared_ptr<spdlog::logger> Application::create_logger(const application_logging_config_s &config) {
    auto sinks = std::vector<spdlog::sink_ptr>{};

    if (config.write_to_console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    if (!config.filename.empty()) {
        const auto parent_directory = config.filename.parent_path();

        if (!parent_directory.empty()) {
            std::filesystem::create_directories(parent_directory);
        }

        sinks.push_back(
                std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.filename.string(), config.truncate_file));
    }

    if (sinks.empty()) {
        throw std::runtime_error{"Application logger must have at least one sink"};
    }

    auto logger = std::make_shared<spdlog::logger>("Application", sinks.begin(), sinks.end());

    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    logger->set_level(config.level);
    logger->flush_on(spdlog::level::warn);

    return logger;
}

void Application::validate_config(const app_config_s &config) {
    if (config.auth.validation_interval.count() <= 0) {
        throw std::runtime_error{"Authentication validation interval must be positive"};
    }

    if (config.auth.offline_grace_period.count() <= 0) {
        throw std::runtime_error{"Authentication offline grace period must be positive"};
    }

    if (config.database.enabled && config.database.filename.empty()) {
        throw std::runtime_error{"Database filename is empty"};
    }

    if (config.identity.application_version.empty()) {
        throw std::runtime_error{"Application version is empty"};
    }
}

void Application::initialize_authentication() {
    if (!m_config.auth.enabled || m_config.auth.development_bypass) {
        m_activated.store(true);
        m_access_state.store(application_access_state_e::active);
        m_subscription.store(subscription_plan_e::free);

        m_logger->warn("Remote authentication is disabled. "
                       "Development access with FREE subscription is enabled");

        return;
    }

    /*
     * Без реализованной удалённой проверки нельзя считать
     * настоящий клиентский сеанс активным.
     */
    m_activated.store(false);
    m_access_state.store(application_access_state_e::inactive);
    m_subscription.store(subscription_plan_e::free);

    m_logger->warn("Authentication is enabled, but its implementation is not available yet");
}

void Application::initialize_database() {
    if (!m_config.database.enabled) {
        m_logger->info("Application database is disabled");
        return;
    }

    /*
     * Заглушка.
     *
     * Позже здесь будут:
     * - открытие SQLite;
     * - создание схемы;
     * - миграции;
     * - таблицы авторизации, настроек и отчётов.
     */
    m_logger->warn("Database is enabled, but database initialization is not implemented");
}

void Application::start_components() {
    assert(m_engine != nullptr);
    assert(m_web_server != nullptr);

    m_logger->info("Loading LlamaEngine");

    m_engine->load();

    m_logger->info("Starting LlamaEngine and llama-server");

    m_engine->start();

    m_is_running.store(true);

    m_logger->info("Starting WebServer at http://{}:{}", m_config.web_server.host, m_config.web_server.port);

    m_web_server->start();

    m_logger->info("Application started. Initial page: {}", initial_page());
}

void Application::stop_components() noexcept {
    if (m_web_server != nullptr) {
        try {
            m_web_server->stop();
        } catch (const std::exception &error) {
            m_logger->error("Failed to stop WebServer: {}", error.what());
        } catch (...) {
            m_logger->error("Failed to stop WebServer: unknown error");
        }
    }

    if (m_engine != nullptr) {
        try {
            m_engine->stop_generating();
        } catch (const std::exception &error) {
            m_logger->error("Failed to stop current generation: {}", error.what());
        } catch (...) {
            m_logger->error("Failed to stop current generation: unknown error");
        }

        try {
            m_engine->stop();
        } catch (const std::exception &error) {
            m_logger->error("Failed to stop LlamaEngine: {}", error.what());
        } catch (...) {
            m_logger->error("Failed to stop LlamaEngine: unknown error");
        }
    }

    m_is_running.store(false);
}

void Application::ensure_chat_access() const {
    if (!m_activated.load()) {
        throw std::runtime_error{"Application is not activated"};
    }

    const auto access_state = m_access_state.load();

    if (!access_state_allows_chat(access_state)) {
        throw std::runtime_error{
                std::format("Chat access is unavailable. Application access state: {}", to_string(access_state))};
    }

    if (!m_engine->is_running()) {
        throw std::runtime_error{"llama-server is not running"};
    }
}

} // namespace stz::intern