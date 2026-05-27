#include "config.hpp"

#include <format>
#include <limits>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "util/file_io.hpp"

namespace stz::intern {

namespace {

using json = nlohmann::json;

template<typename T>
void read_optional_value(const json &object, const char *key, T &destination) {
    if (const auto it = object.find(key); it != object.end() && !it->is_null()) {
        destination = it->get<T>();
    }
}

void read_optional_path(const json &object, const char *key, std::filesystem::path &destination) {
    if (const auto it = object.find(key); it != object.end() && !it->is_null()) {
        destination = std::filesystem::path{it->get<std::string>()};
    }
}

void read_optional_uint16(const json &object, const char *key, std::uint16_t &destination) {
    if (const auto it = object.find(key); it != object.end() && !it->is_null()) {
        const auto value = it->get<std::int32_t>();

        if (value <= 0 || value > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error{std::format("Invalid uint16 value for '{}': {}", key, value)};
        }

        destination = static_cast<std::uint16_t>(value);
    }
}

void read_optional_model_alias(const json &object, llama_server_config_s &config) {
    const auto model_alias_it = object.find("model_alias");
    const auto alias_it = object.find("alias");

    const auto it = model_alias_it != object.end() ? model_alias_it : alias_it;

    if (it == object.end()) {
        return;
    }

    if (it->is_null()) {
        config.model_alias = std::nullopt;
        return;
    }

    auto alias = it->get<std::string>();

    if (alias.empty()) {
        config.model_alias = std::nullopt;
        return;
    }

    config.model_alias = std::move(alias);
}

} // namespace

llama_server_config_s load_server_config(const std::filesystem::path &filename) try {
    const auto content = util::read_text_file(filename);
    const auto root = json::parse(content);

    if (!root.is_object()) {
        throw std::runtime_error{"Server config root must be JSON object"};
    }

    auto config = llama_server_config_s{};

    read_optional_path(root, "executable_path", config.executable_path);
    read_optional_path(root, "model_path", config.model_path);

    read_optional_value(root, "scheme", config.scheme);
    read_optional_value(root, "host", config.host);
    read_optional_uint16(root, "port", config.port);
    read_optional_value(root, "api_path", config.api_path);

    read_optional_model_alias(root, config);

    read_optional_value(root, "context_size", config.context_size);
    read_optional_value(root, "parallel_slots", config.parallel_slots);

    read_optional_value(root, "max_tokens", config.max_tokens);
    read_optional_value(root, "temperature", config.temperature);
    read_optional_value(root, "top_p", config.top_p);

    read_optional_value(root, "connection_timeout_seconds", config.connection_timeout_seconds);
    read_optional_value(root, "read_timeout_seconds", config.read_timeout_seconds);
    read_optional_value(root, "write_timeout_seconds", config.write_timeout_seconds);

    validate_server_config(config);

    return config;
} catch (const std::exception &error) {
    throw std::runtime_error{std::format("Failed to load server config '{}': {}", filename.string(), error.what())};
}

void validate_server_config(const llama_server_config_s &config) {
    if (config.scheme != "http") {
        throw std::runtime_error{
                std::format("Unsupported llama-server scheme '{}'. Currently only HTTP is enabled", config.scheme)};
    }

    if (config.host.empty()) {
        throw std::runtime_error{"llama-server host is empty"};
    }

    if (config.port == 0) {
        throw std::runtime_error{"llama-server port is zero"};
    }

    if (config.api_path.empty() || config.api_path.front() != '/') {
        throw std::runtime_error{std::format("Invalid llama-server API path '{}'", config.api_path)};
    }

    if (config.max_tokens <= 0) {
        throw std::runtime_error{"max_tokens must be positive"};
    }

    if (config.temperature < 0.0) {
        throw std::runtime_error{"temperature must be non-negative"};
    }

    if (config.top_p <= 0.0 || config.top_p > 1.0) {
        throw std::runtime_error{"top_p must be in range (0; 1]"};
    }

    if (config.connection_timeout_seconds <= 0 || config.read_timeout_seconds <= 0 ||
        config.write_timeout_seconds <= 0) {
        throw std::runtime_error{"HTTP timeouts must be positive"};
    }
}

} // namespace stz::intern