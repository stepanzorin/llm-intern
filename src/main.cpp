#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <print>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "config.hpp"
#include "engine.hpp"
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

[[nodiscard]] llama_server_config_s load_or_make_default_server_config(const std::filesystem::path &filename,
                                                                       const std::shared_ptr<spdlog::logger> &logger) {
    if (!std::filesystem::exists(filename)) {
        logger->warn("Server config was not found: {}", filename.string());
        logger->warn("Default llama-server config will be used");

        auto config = llama_server_config_s{};
        validate_server_config(config);

        return config;
    }

    return load_server_config(filename);
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

            continue;
        }
    }

    std::println("");
}

void print_startup_help(const llama_server_config_s &config) {
    std::println("Консольный RAG-чат запущен.");
    std::println("llama-server: http://{}:{}{}", config.host, config.port, config.api_path);
    std::println("Команды: /exit или /quit — выход.");
    std::println("");
}

} // namespace


int main() try {
    util::configure_console();

    auto logger = create_logger();

    try {
        const auto app_dir = util::application_directory_path();

        const auto server_config_path = app_dir / "server_config.json";

        auto server_config = load_or_make_default_server_config(server_config_path, logger);

        auto engine_config = engine_config_s{
                .history_file = app_dir / "message_history.json",
                .knowledge_directory = app_dir / "knowledge",
                .workplace_role = workplace_role_e::beauty_admin,
                .max_knowledge_documents = 2,
                .max_knowledge_chars_per_document = 3000,
        };

        auto engine = LlamaEngine{
                std::move(server_config),
                std::move(engine_config),
                logger,
        };

        engine.load();

        print_startup_help(load_or_make_default_server_config(server_config_path, logger));
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

            if (input.empty()) {
                continue;
            }

            try {
                const auto answer = engine.ask(input);

                std::println("\nAI-бот:");
                std::println("{}\n", answer);
            } catch (const std::exception &error) {
                logger->error("Failed to process user message: {}", error.what());
                std::println(stderr, "Ошибка: {}", error.what());
                std::println(stderr, "Проверь, что llama-server запущен и доступен.");
            }
        }

        logger->info("Application finished");

        return 0;
    } catch (const std::exception &error) {
        logger->critical("Fatal error: {}", error.what());
        std::println(stderr, "Критическая ошибка: {}", error.what());

        return 1;
    }
} catch (...) {
    spdlog::error("Unhandled exception caught: <Unknown error>");
    return EXIT_FAILURE;
}