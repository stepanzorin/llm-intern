#include "web_server.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "application.hpp"
#include "platform.hpp"

#ifdef STZ_INTERN_PLATFORM_WINDOWS

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif

    #include <shellapi.h>
    #include <Windows.h>

    #if defined(_MSC_VER)
        #pragma comment(lib, "Shell32.lib")
    #endif

#endif

namespace stz::intern {

namespace {

using json = nlohmann::json;

constexpr auto json_content_type = "application/json; charset=utf-8";

constexpr auto plain_text_content_type = "text/plain; charset=utf-8";


#ifdef STZ_INTERN_PLATFORM_WINDOWS

[[nodiscard]] std::wstring utf8_to_wstring(const std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const auto required_size = MultiByteToWideChar(CP_UTF8,
                                                   MB_ERR_INVALID_CHARS,
                                                   text.data(),
                                                   static_cast<int>(text.size()),
                                                   nullptr,
                                                   0);

    if (required_size <= 0) {
        throw std::runtime_error{
                std::format("Failed to convert UTF-8 string to UTF-16. Windows error: {}", GetLastError())};
    }

    auto result = std::wstring(static_cast<std::size_t>(required_size), L'\0');

    const auto converted_size = MultiByteToWideChar(CP_UTF8,
                                                    MB_ERR_INVALID_CHARS,
                                                    text.data(),
                                                    static_cast<int>(text.size()),
                                                    result.data(),
                                                    required_size);

    if (converted_size != required_size) {
        throw std::runtime_error{
                std::format("Failed to convert complete UTF-8 string to UTF-16. Windows error: {}", GetLastError())};
    }

    return result;
}

#endif

[[nodiscard]] std::string_view history_status_name(const chat_message_status_e status) noexcept {
    switch (status) {
        case chat_message_status_e::pending: return "pending";

        case chat_message_status_e::completed: return "completed";

        case chat_message_status_e::failed: return "failed";

        case chat_message_status_e::cancelled: return "cancelled";
    }

    return "failed";
}

void set_json_response(httplib::Response &response, const json &body, const int status = 200) {
    response.status = status;

    response.set_header("Cache-Control", "no-store");

    response.set_content(body.dump(), json_content_type);
}

void set_empty_json_response(httplib::Response &response, const int status = 204) {
    response.status = status;

    response.set_header("Cache-Control", "no-store");

    if (status != 204) {
        response.set_content("{}", json_content_type);
    }
}

void set_error_response(httplib::Response &response,
                        const int status,
                        const std::string_view code,
                        const std::string_view message) {
    set_json_response(response,
                      json{
                              {
                                      "error",
                                      {
                                              {"code", code},
                                              {"message", message},
                                      },
                              },
                      },
                      status);
}

[[nodiscard]] json parse_json_object(const httplib::Request &request) {
    if (request.body.empty()) {
        throw std::runtime_error{"Request body is empty"};
    }

    auto root = json::parse(request.body);

    if (!root.is_object()) {
        throw std::runtime_error{"Request JSON root must be an object"};
    }

    return root;
}

[[nodiscard]] std::string read_required_string(const json &object, const std::string_view key) {
    const auto it = object.find(key);

    if (it == object.end() || !it->is_string()) {
        throw std::runtime_error{std::format("Required string field '{}' is missing", key)};
    }

    return it->get<std::string>();
}

template<typename Message>
void append_history_message_json(json &messages,
                                 const chat_history_entry_s &entry,
                                 const std::string_view role,
                                 const Message &message) {
    messages.push_back(json{
            {"id", entry.id},
            {"role", role},
            {"content", message.content},
            {"created_at", message.created_at},
            {
                    "status",
                    history_status_name(entry.status),
            },
            {"source_files", entry.source_files},
    });
}

[[nodiscard]] json make_history_json(const std::span<const chat_history_entry_s> history) {
    auto messages = json::array();

    for (const auto &entry : history) {
        if (entry.user.has_value()) {
            append_history_message_json(messages, entry, "user", *entry.user);
        }

        if (entry.assistant.has_value()) {
            append_history_message_json(messages, entry, "assistant", *entry.assistant);
        }
    }

    return json{
            {"messages", std::move(messages)},
    };
}

[[nodiscard]] bool is_api_path(const std::string_view path) noexcept {
    return path == "/api" || path.starts_with("/api/");
}

#ifdef STZ_INTERN_PLATFORM_WINDOWS

[[nodiscard]] std::wstring ascii_to_wstring(const std::string_view text) {
    /*
     * URL WebServer состоит только из ASCII-символов:
     * http, localhost/127.0.0.1, порт и путь.
     */
    return {
            text.begin(),
            text.end(),
    };
}

#endif

} // namespace

WebServer::WebServer(web_server_config_s config, Application &application, std::shared_ptr<spdlog::logger> logger)
    : m_config{std::move(config)},
      m_application{application},
      m_logger{std::move(logger)} {
    assert(m_logger != nullptr);

    validate_config();
    configure_http_server();
}

WebServer::~WebServer() { stop(); }

void WebServer::start() {
    auto lock = std::scoped_lock{m_lifecycle_mutex};

    const auto start_was_already_called = m_start_called.exchange(true);

    assert(!start_was_already_called);
    assert(!m_server_thread.joinable());

    if (start_was_already_called || m_server_thread.joinable()) {
        std::terminate();
    }

    m_logger->info("Binding WebServer to {}:{}", m_config.host, m_config.port);

    const auto bound = m_http_server.bind_to_port(m_config.host, static_cast<int>(m_config.port));

    if (!bound) {
        m_start_called.store(false);
        throw std::runtime_error{std::format("Failed to bind WebServer to {}:{}", m_config.host, m_config.port)};
    }

    m_is_running.store(true);

    try {
        m_server_thread = std::jthread{
                [this] { run_http_server(); },
        };
    } catch (...) {
        m_is_running.store(false);
        m_start_called.store(false);

        m_http_server.stop();
        m_wait_condition.notify_all();

        throw;
    }

    m_http_server.wait_until_ready();

    if (!m_is_running.load()) {
        throw std::runtime_error{"WebServer stopped during startup"};
    }

    m_logger->info("WebServer started: {}", url());

    m_logger->info("Web root directory: '{}'", m_config.web_directory.string());

    if (m_config.open_browser_on_start) {
        open_browser();
    }
}

void WebServer::stop() noexcept {
    auto thread = std::jthread{};

    {
        auto lock = std::scoped_lock{
                m_lifecycle_mutex,
        };

        if (!m_start_called.load()) {
            return;
        }

        if (!m_server_thread.joinable()) {
            m_is_running.store(false);
            m_wait_condition.notify_all();
            return;
        }

        m_logger->info("Stopping WebServer");

        m_http_server.stop();

        /*
         * Переносим поток во временный объект,
         * чтобы не держать lifecycle mutex во время join().
         */
        thread = std::move(m_server_thread);
    }

    if (thread.joinable()) {
        thread.join();
    }

    m_is_running.store(false);
    m_wait_condition.notify_all();

    m_logger->info("WebServer stopped");
}

void WebServer::wait() {
    const auto start_was_called = m_start_called.load();

    assert(start_was_called);

    if (!start_was_called) {
        std::terminate();
    }

    auto lock = std::unique_lock{
            m_wait_mutex,
    };

    m_wait_condition.wait(lock, [this] { return !m_is_running.load(); });
}

bool WebServer::is_running() const noexcept { return m_is_running.load(); }

std::string WebServer::url() const { return std::format("http://{}:{}", m_config.host, m_config.port); }

void WebServer::validate_config() const {
    if (m_config.host != "127.0.0.1" && m_config.host != "localhost") {
        throw std::runtime_error{std::format("WebServer host must be local, got '{}'", m_config.host)};
    }

    if (m_config.port == 0) {
        throw std::runtime_error{"WebServer port must not be zero"};
    }

    /*
     * Один поток может быть занят генерацией ответа.
     * Второй нужен как минимум для /api/chat/stop.
     */
    if (m_config.worker_threads < 2) {
        throw std::runtime_error{"WebServer worker_threads must be at least 2"};
    }

    if (m_config.web_directory.empty()) {
        throw std::runtime_error{"WebServer web_directory is empty"};
    }

    if (!std::filesystem::exists(m_config.web_directory)) {
        throw std::runtime_error{std::format("Web directory does not exist: '{}'", m_config.web_directory.string())};
    }

    if (!std::filesystem::is_directory(m_config.web_directory)) {
        throw std::runtime_error{std::format("Web path is not a directory: '{}'", m_config.web_directory.string())};
    }
}

void WebServer::configure_http_server() {
    /*
     * Все настройки и маршруты регистрируются до start().
     */
    m_http_server.new_task_queue = [worker_threads = m_config.worker_threads] {
        return new httplib::ThreadPool{
                worker_threads,
        };
    };

    register_logging();
    register_error_handlers();
    register_page_routes();
    register_api_routes();
    register_static_files();
}

void WebServer::register_page_routes() {
    m_http_server.Get("/", [this](const httplib::Request &, httplib::Response &response) {
        response.set_redirect(std::string{
                m_application.initial_page(),
        });
    });

    m_http_server.Get("/index", [](const httplib::Request &, httplib::Response &response) {
        response.set_redirect("/index.html");
    });

    m_http_server.Get("/chat", [](const httplib::Request &, httplib::Response &response) {
        response.set_redirect("/chat.html");
    });

    m_http_server.Get("/settings", [](const httplib::Request &, httplib::Response &response) {
        response.set_redirect("/settings.html");
    });

    m_http_server.Get("/report", [](const httplib::Request &, httplib::Response &response) {
        response.set_redirect("/report.html");
    });

    m_http_server.Get("/session-expired", [](const httplib::Request &, httplib::Response &response) {
        response.set_redirect("/session_expired.html");
    });
}

void WebServer::register_api_routes() {
    m_http_server.Get("/api/health", [this](const httplib::Request &, httplib::Response &response) {
        set_json_response(response,
                          json{
                                  {"ok", true},
                                  {
                                          "web_server_running",
                                          is_running(),
                                  },
                          });
    });

    m_http_server.Get("/api/application/state", [this](const httplib::Request &request, httplib::Response &response) {
        handle_get_application_state(request, response);
    });

    /*
     * Дополнительный алиас, чтобы старый JS-код
     * мог обращаться к /api/server/state.
     */
    m_http_server.Get("/api/server/state", [this](const httplib::Request &request, httplib::Response &response) {
        handle_get_application_state(request, response);
    });

    m_http_server.Get("/api/chat/history", [this](const httplib::Request &request, httplib::Response &response) {
        handle_get_chat_history(request, response);
    });

    m_http_server.Post("/api/chat/messages", [this](const httplib::Request &request, httplib::Response &response) {
        handle_post_chat_message(request, response);
    });

    m_http_server.Post("/api/chat/stop", [this](const httplib::Request &request, httplib::Response &response) {
        handle_post_stop_generation(request, response);
    });

    m_http_server.Post("/api/server/restart", [this](const httplib::Request &request, httplib::Response &response) {
        handle_post_restart_server(request, response);
    });

    m_http_server.Delete("/api/chat/history", [this](const httplib::Request &request, httplib::Response &response) {
        handle_delete_chat_history(request, response);
    });

    m_http_server.Post("/api/auth/activate", [this](const httplib::Request &request, httplib::Response &response) {
        handle_post_activation(request, response);
    });

    m_http_server.Post("/api/report", [this](const httplib::Request &request, httplib::Response &response) {
        handle_post_report(request, response);
    });
}

void WebServer::register_static_files() {
    const auto mounted = m_http_server.set_mount_point("/", m_config.web_directory.string());

    if (!mounted) {
        throw std::runtime_error{std::format("Failed to mount web directory '{}'", m_config.web_directory.string())};
    }
}

void WebServer::register_logging() {
    const auto logger = m_logger;

    m_http_server.set_logger([logger](const httplib::Request &request, const httplib::Response &response) {
        logger->debug("{} {} -> HTTP {}", request.method, request.path, response.status);
    });
}

void WebServer::register_error_handlers() {
    const auto logger = m_logger;

    m_http_server.set_exception_handler([logger](const httplib::Request &request,
                                                 httplib::Response &response,
                                                 const std::exception_ptr exception) {
        auto message = std::string{"Unknown exception"};

        try {
            if (exception != nullptr) {
                std::rethrow_exception(exception);
            }
        } catch (const std::exception &error) {
            message = error.what();
        } catch (...) {
            message = "Unknown exception";
        }

        logger->error("Unhandled exception while processing '{} {}': {}", request.method, request.path, message);

        if (is_api_path(request.path)) {
            set_error_response(response, 500, "internal_error", "Внутренняя ошибка сервера");

            return;
        }

        response.status = 500;

        response.set_content("Внутренняя ошибка сервера", plain_text_content_type);
    });

    m_http_server.set_error_handler([](const httplib::Request &request, httplib::Response &response) {
        if (is_api_path(request.path)) {
            const auto status = response.status == 0 ? 404 : response.status;

            set_error_response(response,
                               status,
                               "http_error",
                               status == 404 ? "API-метод не найден" : "Ошибка HTTP-запроса");

            return;
        }

        if (response.status == 404) {
            response.set_content("Страница не найдена", plain_text_content_type);
        }
    });
}

void WebServer::run_http_server() noexcept {
    try {
        const auto result = m_http_server.listen_after_bind();

        if (!result) {
            m_logger->error("WebServer listen loop finished with an error");
        }
    } catch (const std::exception &error) {
        m_logger->error("WebServer listen loop failed: {}", error.what());
    } catch (...) {
        m_logger->error("WebServer listen loop failed: unknown error");
    }

    m_is_running.store(false);
    m_wait_condition.notify_all();
}

void WebServer::open_browser() const noexcept {
    /*
     * Открываем корневой маршрут.
     *
     * WebServer сам перенаправит пользователя на:
     * - /index.html;
     * - /chat.html;
     * - /session_expired.html.
     */
    const auto target_url = url() + "/";

    try {
#ifdef STZ_INTERN_PLATFORM_WINDOWS

        const auto wide_url = utf8_to_wstring(target_url);

        const auto result = ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

        const auto result_code = reinterpret_cast<std::intptr_t>(result);

        if (result_code <= 32) {
            m_logger->warn("Failed to open browser at '{}'. ShellExecuteW result: {}", target_url, result_code);

            return;
        }

#elif defined(__APPLE__)

        const auto command = std::format("open \"{}\" >/dev/null 2>&1 &", target_url);

        const auto result = std::system(command.c_str());

        if (result != 0) {
            m_logger->warn("Failed to open browser at '{}'. Command result: {}", target_url, result);

            return;
        }

#else

        const auto command = std::format("xdg-open \"{}\" >/dev/null 2>&1 &", target_url);

        const auto result = std::system(command.c_str());

        if (result != 0) {
            m_logger->warn("Failed to open browser at '{}'. Command result: {}", target_url, result);

            return;
        }

#endif

        m_logger->info("Opened local WebUI in browser: {}", target_url);
    } catch (const std::exception &error) {
        m_logger->warn("Failed to open local WebUI in browser: {}", error.what());
    } catch (...) {
        m_logger->warn("Failed to open local WebUI in browser: unknown error");
    }
}

void WebServer::handle_get_application_state(const httplib::Request &, httplib::Response &response) {
    const auto state = m_application.state();

    set_json_response(response,
                      json{
                              {
                                      "application_running",
                                      state.application_running,
                              },
                              {
                                      "activated",
                                      state.activated,
                              },
                              {
                                      "access_state",
                                      to_string(state.access_state),
                              },
                              {
                                      "subscription",
                                      to_string(state.subscription),
                              },
                              {
                                      "llama_server_running",
                                      state.llama_server_running,
                              },
                              {
                                      "model_generates",
                                      state.model_generates,
                              },
                              {
                                      "company_name",
                                      state.company_name,
                              },
                              {
                                      "point_name",
                                      state.point_name,
                              },
                              {
                                      "point_address",
                                      state.point_address,
                              },
                              {
                                      "application_version",
                                      state.application_version,
                              },
                              {
                                      "initial_page",
                                      m_application.initial_page(),
                              },
                      });
}

void WebServer::handle_get_chat_history(const httplib::Request &, httplib::Response &response) {
    try {
        const auto history = m_application.history_snapshot();

        set_json_response(response, make_history_json(history));
    } catch (const std::exception &error) {
        m_logger->error("Failed to load chat history: {}", error.what());

        set_error_response(response, 500, "history_load_failed", "Не удалось загрузить историю сообщений");
    }
}

void WebServer::handle_post_chat_message(const httplib::Request &request, httplib::Response &response) {
    try {
        if (!m_application.can_use_chat()) {
            set_json_response(response,
                              json{
                                      {
                                              "error",
                                              {
                                                      {
                                                              "code",
                                                              "chat_access_denied",
                                                      },
                                                      {
                                                              "message",
                                                              "Доступ к чату недоступен",
                                                      },
                                              },
                                      },
                                      {
                                              "redirect",
                                              m_application.initial_page(),
                                      },
                              },
                              403);

            return;
        }

        const auto state = m_application.state();

        if (!state.llama_server_running) {
            set_error_response(response, 503, "llama_server_unavailable", "Модель ещё не запущена");

            return;
        }

        if (state.model_generates) {
            set_error_response(response, 409, "generation_in_progress", "Модель уже формирует ответ");

            return;
        }

        const auto root = parse_json_object(request);

        const auto message = read_required_string(root, "message");

        if (message.empty()) {
            set_error_response(response, 400, "empty_message", "Сообщение пустое");

            return;
        }

        const auto answer = m_application.ask(message);

        set_json_response(response,
                          json{
                                  {"answer", answer},
                          });
    } catch (const json::exception &error) {
        m_logger->warn("Invalid chat request JSON: {}", error.what());

        set_error_response(response, 400, "invalid_json", "Некорректный JSON запроса");
    } catch (const std::runtime_error &error) {
        m_logger->warn("Chat request failed: {}", error.what());

        set_error_response(response, 400, "invalid_request", error.what());
    } catch (const std::exception &error) {
        m_logger->error("Failed to process chat message: {}", error.what());

        set_error_response(response, 500, "generation_failed", "Не удалось получить ответ от бота");
    }
}

void WebServer::handle_post_stop_generation(const httplib::Request &, httplib::Response &response) {
    const auto was_generating = m_application.state().model_generates;

    m_application.stop_generating();

    set_json_response(response,
                      json{
                              {"accepted", was_generating},
                      },
                      was_generating ? 202 : 200);
}

void WebServer::handle_post_restart_server(const httplib::Request &, httplib::Response &response) {
    try {
        const auto state = m_application.state();

        if (state.model_generates) {
            set_error_response(response,
                               409,
                               "generation_in_progress",
                               "Нельзя перезапустить модель во время генерации ответа");

            return;
        }

        const auto result = m_application.restart_model_server();

        set_json_response(response,
                          json{
                                  {"success", result.success},
                                  {"message", result.message},
                          },
                          result.success ? 200 : 409);
    } catch (const std::exception &error) {
        m_logger->error("Failed to restart llama-server: {}", error.what());

        set_error_response(response, 500, "restart_failed", "Не удалось перезапустить модель");
    }
}

void WebServer::handle_delete_chat_history(const httplib::Request &, httplib::Response &response) {
    try {
        if (m_application.state().model_generates) {
            set_error_response(response,
                               409,
                               "generation_in_progress",
                               "Нельзя очистить историю во время генерации ответа");

            return;
        }

        m_application.clear_history();

        set_empty_json_response(response, 204);
    } catch (const std::exception &error) {
        m_logger->error("Failed to clear chat history: {}", error.what());

        set_error_response(response, 500, "history_clear_failed", "Не удалось очистить историю сообщений");
    }
}

void WebServer::handle_post_activation(const httplib::Request &request, httplib::Response &response) {
    try {
        const auto root = parse_json_object(request);

        auto activation_request = activation_request_s{
                .point_name = read_required_string(root, "point_name"),

                .point_address = read_required_string(root, "point_address"),

                .activation_key = read_required_string(root, "activation_key"),
        };

        const auto result = m_application.activate(activation_request);

        set_json_response(response,
                          json{
                                  {"success", result.success},
                                  {"message", result.message},
                                  {
                                          "redirect",
                                          result.success ? m_application.initial_page() : "",
                                  },
                          },
                          result.success ? 200 : 400);
    } catch (const json::exception &error) {
        m_logger->warn("Invalid activation request JSON: {}", error.what());

        set_error_response(response, 400, "invalid_json", "Некорректный JSON запроса");
    } catch (const std::exception &error) {
        m_logger->warn("Activation request failed: {}", error.what());

        set_error_response(response, 400, "activation_failed", error.what());
    }
}

void WebServer::handle_post_report(const httplib::Request &request, httplib::Response &response) {
    try {
        const auto root = parse_json_object(request);

        const auto result = m_application.submit_report(report_request_s{
                .description = read_required_string(root, "description"),
        });

        set_json_response(response,
                          json{
                                  {"success", result.success},
                                  {"message", result.message},
                          },
                          result.success ? 200 : 501);
    } catch (const json::exception &error) {
        m_logger->warn("Invalid report request JSON: {}", error.what());

        set_error_response(response, 400, "invalid_json", "Некорректный JSON запроса");
    } catch (const std::exception &error) {
        m_logger->warn("Report request failed: {}", error.what());

        set_error_response(response, 400, "report_failed", error.what());
    }
}

} // namespace stz::intern