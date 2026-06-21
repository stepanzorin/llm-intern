#include <cstdlib>
#include <exception>
#include <filesystem>
#include <print>
#include <utility>

#include <spdlog/spdlog.h>

#include "application.hpp"
#include "llm/llama_server.hpp"
#include "util/console.hpp"
#include "util/filesystem_helpers.hpp"

using namespace stz::intern;

namespace {

[[nodiscard]] app_config_s make_application_config() {
    const auto application_directory = util::application_directory_path();

    auto config = app_config_s{};

    config.llama_server = llm::load_server_config(application_directory, "server_config.json");

    config.web_server = web_server_config_s{
            .host = "127.0.0.1",
            .port = 3000,
            .web_directory = application_directory / "web",
            .worker_threads = 4,
            .open_browser_on_start = true,
    };

    config.engine = engine_config_s{
            .history_file = application_directory / "message_history.json",
            .knowledge_directory = application_directory / "knowledge",
            .workplace_role = workplace_role_e::barista,
            .max_knowledge_documents = 2,
            .max_knowledge_chars_per_document = 3000,
            .min_ranked_knowledge_score = 512,
    };

    config.auth = auth_config_s{
            .enabled = false,
            .development_bypass = true,
            .state_file = application_directory / "backend/auth_state.json",
            .validation_url = {},
            .validation_interval = std::chrono::minutes{60},
            .offline_grace_period = std::chrono::hours{24},
    };

    config.database = database_config_s{
            .enabled = false,
            .filename = application_directory / "backend/application.db",
    };

    config.identity = application_identity_config_s{
            .company_name = "Название компании",
            .point_name = {},
            .point_address = {},
            .application_version = "0.1.0",
    };

    config.logging = application_logging_config_s{
            .filename = application_directory / "backend/application.log",
            .level = spdlog::level::info,
            .write_to_console = true,
            .truncate_file = false,
    };

    return config;
}

} // namespace

int main() try {
    util::configure_console();

    const auto application_directory = util::application_directory_path();

    auto application = Application{
            make_application_config(),
    };

    return application.run();
} catch (const std::exception &error) {
    spdlog::critical("Fatal application error: {}", error.what());
    std::println(stderr, "Критическая ошибка: {}", error.what());

    return EXIT_FAILURE;
} catch (...) {
    spdlog::critical("Unhandled exception caught: unknown error");

    return EXIT_FAILURE;
}