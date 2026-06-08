#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <print>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "engine.hpp"
#include "llm/llama_server.hpp"
#include "platform.hpp"
#include "util/console.hpp"
#include "util/filesystem_helpers.hpp"
#include "util/string_helpers.hpp"
#include "util/time.hpp"


using namespace stz::intern;

namespace {

[[nodiscard]] std::shared_ptr<spdlog::logger> create_logger() {
    auto logger = spdlog::stdout_color_mt("intern");

    logger->set_pattern(std::format("[{}.%e] [%^%l%$] [%n] %v", util::dmy_hms_format));

    logger->set_level(spdlog::level::info);

    return logger;
}

[[nodiscard]] llm::llama_server_config_s make_default_server_config(
        const std::filesystem::path &application_directory) {
    auto config = llm::llama_server_config_s{};

    config.application_directory = application_directory;

#ifdef STZ_INTERN_PLATFORM_WINDOWS
    config.executable = "backend/llama-server.exe";
#else
    config.executable = "backend/llama-server";
#endif

    config.endpoint = llm::llama_endpoint_config_s{
            .host = "127.0.0.1",
            .port = 8080,
            .chat_completions_path = "/v1/chat/completions",
            .api_key = std::nullopt,
    };

    config.models = {
            llm::llama_model_config_s{
                    .model = llm::model_e::standard,
                    .filename = "models/qwen2.5-3b-instruct-q4_k_m.gguf",
                    .alias = "qwen2.5-3b-instruct",
                    .cache_filename = "qwen2.5-3b-chat.bin",
                    .extra_arguments = {},
            },
    };

    config.initial_model = llm::model_e::standard;

    config.threads = 2;
    config.threads_batch = 2;
    config.context_size = 4096;
    config.parallel_slots = 1;
    config.slot_id = 0;

    config.use_jinja = true;
    config.enable_prompt_cache = true;
    config.enable_slots = true;
    config.enable_metrics = true;
    config.disable_builtin_ui = true;
    config.allow_remote_connections = false;

    config.load_cache_on_start = true;
    config.store_cache_on_stop = true;

    config.slot_cache_directory = "cache/llama_slots";

    config.state_file = "state/llama_server_state.json";

    return config;
}

[[nodiscard]] llm::llama_server_config_s load_or_make_default_server_config(
        const std::filesystem::path &filename,
        const std::filesystem::path &application_directory,
        const std::shared_ptr<spdlog::logger> &logger) {
    if (!std::filesystem::exists(filename)) {
        logger->warn("Server config was not found: {}", filename.string());

        logger->warn("Default llama-server configuration will be used");

        auto config = make_default_server_config(application_directory);

        llm::validate_server_config(config);

        return config;
    }

    return llm::load_server_config(filename, application_directory);
}

void print_history(const std::span<const chat_history_entry_s> history) {
    if (history.empty()) {
        std::println("История сообщений пока пустая.");
        return;
    }

    std::println("Загружена история сообщений:");

    for (const auto &entry : history) {
        if (entry.user.has_value()) {
            std::println("\n{} [{}]", entry.user->name, entry.user->created_at);

            std::println("{}", entry.user->content);

            if (entry.status == chat_message_status_e::pending) {
                std::println("[сообщение ожидает ответа]");
            } else if (entry.status == chat_message_status_e::failed) {
                std::println("[ответ не был получен из-за ошибки]");
            } else if (entry.status == chat_message_status_e::cancelled) {
                std::println("[генерация ответа была отменена]");
            }

            continue;
        }

        if (entry.assistant.has_value()) {
            std::println("\n{} [{}]", entry.assistant->name, entry.assistant->created_at);

            std::println("{}", entry.assistant->content);
        }
    }

    std::println("");
}

void print_server_state(const LlamaEngine &engine) {
    const auto state = engine.server_state();

    std::println("llama-server: {}", state.running ? "работает" : "не работает");

    std::println("URL: {}", state.url);

    std::println("Модель: {}", state.current_model_alias);

    std::println("Генерация: {}", state.model_generates ? "выполняется" : "не выполняется");

    std::println("Предупреждения: {}", state.warning_count);

    std::println("Ошибки: {}", state.error_count);
}

void print_startup_help(const LlamaEngine &engine) {
    std::println("Консольный RAG-чат запущен.");
    std::println("llama-server: {}", engine.server_url());

    std::println("Команды: /status — состояние, "
                 "/exit или /quit — выход.");

    std::println("");
}

} // namespace

int main() try {
    util::configure_console();

    auto logger = create_logger();

    const auto application_directory = util::application_directory_path();

    const auto server_config_path = application_directory / "server_config.json";

    auto server_config = load_or_make_default_server_config(server_config_path, application_directory, logger);

    auto config = engine_config_s{
            .server = std::move(server_config),

            .client =
                    llm::llama_client_config_s{
                            .temperature = 0.2,
                            .top_p = 0.9,
                            .max_tokens = 512,
                            .cache_prompt = true,
                            .connection_timeout = std::chrono::seconds{5},
                            .read_timeout = std::chrono::minutes{10},
                            .write_timeout = std::chrono::seconds{30},
                    },

            .history_file = application_directory / "message_history.json",

            .knowledge_directory = application_directory / "knowledge",

            .workplace_role = workplace_role_e::beauty_admin,

            .max_knowledge_documents = 2,
            .max_knowledge_chars_per_document = 3000,
            .min_ranked_knowledge_score = 512,
    };

    auto engine = LlamaEngine{
            std::move(config),
            logger,
    };

    engine.load();

    print_startup_help(engine);
    print_history(engine.history());

    while (true) {
        std::print("> ");
        std::fflush(stdout);

        auto input = std::string{};

        if (!util::read_console_line_utf8(input)) {
            std::println("");
            break;
        }

        util::trim(input);

        if (input == "/exit" || input == "/quit") {
            break;
        }

        if (input == "/status") {
            print_server_state(engine);
            std::println("");
            continue;
        }

        if (input.empty()) {
            continue;
        }

        try {
            const auto answer = engine.ask(input);

            if (answer.status == chat_message_status_e::cancelled) {
                std::println("\n[Генерация ответа отменена]\n");

                continue;
            }

            std::println("\nAI-бот:");
            std::println("{}\n", answer.content);
        } catch (const std::exception &error) {
            logger->error("Failed to process user message: {}", error.what());

            std::println(stderr, "Ошибка: {}", error.what());
        }
    }

    engine.stop_server();

    logger->info("Application finished");

    return EXIT_SUCCESS;
} catch (const std::exception &error) {
    spdlog::critical("Fatal error: {}", error.what());

    std::println(stderr, "Критическая ошибка: {}", error.what());

    return EXIT_FAILURE;
} catch (...) {
    spdlog::critical("Unhandled exception caught: unknown error");

    return EXIT_FAILURE;
}