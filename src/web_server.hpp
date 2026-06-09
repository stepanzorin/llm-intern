// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <httplib.h>
#include <spdlog/logger.h>

#include "web_server_config.hpp"

namespace stz::intern {

class Application;

class WebServer final {
public:
    WebServer(web_server_config_s config, Application &application, std::shared_ptr<spdlog::logger> logger);

    ~WebServer();

    WebServer(const WebServer &) = delete;
    WebServer &operator=(const WebServer &) = delete;

    WebServer(WebServer &&) = delete;
    WebServer &operator=(WebServer &&) = delete;

    /*
     * Preconditions:
     * - start() вызывается не более одного раза.
     * - WebServer ещё не запущен.
     */
    void start();

    /*
     * Метод идемпотентный.
     *
     * Его можно безопасно вызвать несколько раз,
     * в том числе из деструктора.
     */
    void stop() noexcept;

    /*
     * Блокирует вызывающий поток до остановки HTTP-сервера.
     *
     * Preconditions:
     * - start() уже был вызван.
     */
    void wait();

    [[nodiscard]] bool is_running() const noexcept;

    [[nodiscard]] std::string url() const;

private:
    web_server_config_s m_config;

    Application &m_application;

    httplib::Server m_http_server;
    std::jthread m_server_thread;

    mutable std::mutex m_lifecycle_mutex;

    std::mutex m_wait_mutex;
    std::condition_variable m_wait_condition;

    std::atomic_bool m_start_called = false;
    std::atomic_bool m_is_running = false;

    std::shared_ptr<spdlog::logger> m_logger;


    void validate_config() const;

    void configure_http_server();

    void register_page_routes();

    void register_api_routes();

    void register_static_files();

    void register_logging();

    void register_error_handlers();

    void run_http_server() noexcept;

    void open_browser() const noexcept;

    void handle_get_application_state(const httplib::Request &request, httplib::Response &response);

    void handle_get_chat_history(const httplib::Request &request, httplib::Response &response);

    void handle_post_chat_message(const httplib::Request &request, httplib::Response &response);

    void handle_post_stop_generation(const httplib::Request &request, httplib::Response &response);

    void handle_delete_chat_history(const httplib::Request &request, httplib::Response &response);

    void handle_post_activation(const httplib::Request &request, httplib::Response &response);

    void handle_post_report(const httplib::Request &request, httplib::Response &response);
};

} // namespace stz::intern