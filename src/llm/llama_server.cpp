#include "llama_server.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <format>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/sinks/basic_file_sink.h>

#include "platform.hpp"
#include "util/file_io.hpp"
#include "util/time.hpp"

#ifdef STZ_INTERN_PLATFORM_WINDOWS

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #include <Windows.h>

#else

    #include <csignal>
    #include <cstring>
    #include <spawn.h>
    #include <sys/types.h>
    #include <sys/wait.h>
    #include <unistd.h>

extern char **environ;

#endif

namespace stz::intern::llm {

namespace {

using json = nlohmann::json;

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path) {
#ifdef STZ_INTERN_PLATFORM_WINDOWS
    const auto value = path.u8string();

    return {
            reinterpret_cast<const char *>(value.data()),
            value.size(),
    };
#else
    return path.string();
#endif
}

[[nodiscard]] bool is_loopback_host(const std::string_view host) noexcept {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

[[nodiscard]] std::string model_display_name(const llama_model_config_s &model) {
    if (model.alias.has_value() && !model.alias->empty()) {
        return *model.alias;
    }

    return std::string{to_string(model.model)};
}

[[nodiscard]] bool valid_cache_filename(const std::string_view filename) {
    if (filename.empty()) {
        return false;
    }

    const auto path = std::filesystem::path{filename};

    return path.filename() == path && path.extension() == ".bin";
}

[[nodiscard]] std::string lowercase_ascii(std::string text) {
    std::ranges::transform(text, text.begin(), [](const unsigned char value) noexcept {
        return static_cast<char>(std::tolower(value));
    });

    return text;
}

void configure_http_timeouts(httplib::Client &client, const std::chrono::milliseconds timeout) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(timeout - seconds);

    client.set_connection_timeout(static_cast<time_t>(seconds.count()), static_cast<time_t>(microseconds.count()));

    client.set_read_timeout(static_cast<time_t>(seconds.count()), static_cast<time_t>(microseconds.count()));

    client.set_write_timeout(static_cast<time_t>(seconds.count()), static_cast<time_t>(microseconds.count()));
}

[[nodiscard]] httplib::Headers make_http_headers(const llama_endpoint_config_s &endpoint) {
    auto headers = httplib::Headers{};

    if (endpoint.api_key.has_value() && !endpoint.api_key->empty()) {
        headers.emplace("Authorization", std::format("Bearer {}", *endpoint.api_key));
    }

    return headers;
}

#ifdef STZ_INTERN_PLATFORM_WINDOWS

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view text) {
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
        throw std::runtime_error{std::format("Failed to convert UTF-8 text to UTF-16: error={}", GetLastError())};
    }

    auto result = std::wstring(static_cast<std::size_t>(required_size), L'\0');

    const auto converted_size = MultiByteToWideChar(CP_UTF8,
                                                    MB_ERR_INVALID_CHARS,
                                                    text.data(),
                                                    static_cast<int>(text.size()),
                                                    result.data(),
                                                    required_size);

    if (converted_size != required_size) {
        throw std::runtime_error{std::format("Failed to convert UTF-8 text to UTF-16: error={}", GetLastError())};
    }

    return result;
}

[[nodiscard]] std::wstring quote_windows_argument(const std::wstring_view argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring{argument};
    }

    auto result = std::wstring{L"\""};
    auto backslashes = 0zu;

    for (const auto ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }

        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }

    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');

    return result;
}

[[nodiscard]] std::wstring make_windows_command_line(const std::filesystem::path &executable,
                                                     const std::span<const std::string> arguments) {
    auto result = quote_windows_argument(executable.wstring());

    for (const auto &argument : arguments) {
        result.push_back(L' ');
        result += quote_windows_argument(utf8_to_wide(argument));
    }

    return result;
}

#endif


template<typename T>
void read_optional_value(const json &object, const std::string_view key, T &destination) {
    const auto it = object.find(key);

    if (it == object.end() || it->is_null()) {
        return;
    }

    try {
        destination = it->get<T>();
    } catch (const std::exception &error) {
        throw std::runtime_error{std::format("Invalid value for '{}': {}", key, error.what())};
    }
}

void read_optional_path(const json &object, const std::string_view key, std::filesystem::path &destination) {
    const auto it = object.find(key);

    if (it == object.end() || it->is_null()) {
        return;
    }

    if (!it->is_string()) {
        throw std::runtime_error{std::format("'{}' must be a string containing a filesystem path", key)};
    }

    destination = std::filesystem::path{it->get<std::string>()};
}

[[nodiscard]] std::filesystem::path read_required_path(const json &object, const std::string_view key) {
    const auto it = object.find(key);

    if (it == object.end() || it->is_null()) {
        throw std::runtime_error{std::format("Required filesystem path '{}' is missing", key)};
    }

    if (!it->is_string()) {
        throw std::runtime_error{std::format("'{}' must be a string containing a filesystem path", key)};
    }

    auto result = std::filesystem::path{it->get<std::string>()};

    if (result.empty()) {
        throw std::runtime_error{std::format("Filesystem path '{}' must not be empty", key)};
    }

    return result;
}

[[nodiscard]] std::uint64_t read_unsigned_integer(const json &value, const std::string_view key) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }

    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();

        if (signed_value < 0) {
            throw std::runtime_error{std::format("'{}' must not be negative: {}", key, signed_value)};
        }

        return static_cast<std::uint64_t>(signed_value);
    }

    throw std::runtime_error{std::format("'{}' must be an integer", key)};
}

void read_optional_size(const json &object, const std::string_view key, std::size_t &destination) {
    const auto it = object.find(key);

    if (it == object.end() || it->is_null()) {
        return;
    }

    const auto value = read_unsigned_integer(*it, key);

    if (value > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error{std::format("Value for '{}' is too large: {}", key, value)};
    }

    destination = static_cast<std::size_t>(value);
}

void read_optional_uint16(const json &object, const std::string_view key, std::uint16_t &destination) {
    const auto it = object.find(key);

    if (it == object.end() || it->is_null()) {
        return;
    }

    const auto value = read_unsigned_integer(*it, key);

    if (value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error{std::format("Invalid uint16 value for '{}': {}", key, value)};
    }

    destination = static_cast<std::uint16_t>(value);
}

void read_optional_milliseconds(const json &object,
                                const std::string_view key,
                                std::chrono::milliseconds &destination) {
    const auto it = object.find(key);

    if (it == object.end() || it->is_null()) {
        return;
    }

    const auto value = read_unsigned_integer(*it, key);

    if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error{std::format("Invalid duration for '{}': {}", key, value)};
    }

    destination = std::chrono::milliseconds{static_cast<std::int64_t>(value)};
}

void read_optional_string_vector(const json &object,
                                 const std::string_view key,
                                 std::vector<std::string> &destination) {
    const auto it = object.find(key);

    if (it == object.end() || it->is_null()) {
        return;
    }

    if (!it->is_array()) {
        throw std::runtime_error{std::format("'{}' must be an array of strings", key)};
    }

    auto result = std::vector<std::string>{};
    result.reserve(it->size());

    for (const auto &item : *it) {
        if (!item.is_string()) {
            throw std::runtime_error{std::format("'{}' must contain only strings", key)};
        }

        auto argument = item.get<std::string>();

        if (argument.empty()) {
            throw std::runtime_error{std::format("'{}' must not contain empty arguments", key)};
        }

        result.push_back(std::move(argument));
    }

    destination = std::move(result);
}

void read_optional_string(const json &object, const std::string_view key, std::optional<std::string> &destination) {
    const auto it = object.find(key);

    if (it == object.end()) {
        return;
    }

    if (it->is_null()) {
        destination = std::nullopt;
        return;
    }

    if (!it->is_string()) {
        throw std::runtime_error{std::format("'{}' must be a string or null", key)};
    }

    auto value = it->get<std::string>();

    if (value.empty()) {
        destination = std::nullopt;
        return;
    }

    destination = std::move(value);
}

[[nodiscard]] model_e read_required_model(const json &object, const std::string_view key) {
    const auto it = object.find(key);

    if (it == object.end() || it->is_null()) {
        throw std::runtime_error{std::format("Required model field '{}' is missing", key)};
    }

    if (!it->is_string()) {
        throw std::runtime_error{std::format("'{}' must be a string", key)};
    }

    return model_from_string(it->get<std::string>());
}

void read_optional_model(const json &object, const std::string_view key, model_e &destination) {
    const auto it = object.find(key);

    if (it == object.end() || it->is_null()) {
        return;
    }

    if (!it->is_string()) {
        throw std::runtime_error{std::format("'{}' must be a string", key)};
    }

    destination = model_from_string(it->get<std::string>());
}

[[nodiscard]] llama_endpoint_config_s read_endpoint_config(const json &root, llama_endpoint_config_s endpoint) {
    const auto it = root.find("endpoint");

    if (it == root.end() || it->is_null()) {
        return endpoint;
    }

    if (!it->is_object()) {
        throw std::runtime_error{"'endpoint' must be a JSON object"};
    }

    read_optional_value(*it, "host", endpoint.host);
    read_optional_uint16(*it, "port", endpoint.port);
    read_optional_value(*it, "chat_completions_path", endpoint.chat_completions_path);
    read_optional_string(*it, "api_key", endpoint.api_key);

    return endpoint;
}

[[nodiscard]] llama_model_config_s read_model_config(const json &object) {
    if (!object.is_object()) {
        throw std::runtime_error{"Every element of 'models' must be a JSON object"};
    }

    auto config = llama_model_config_s{};

    config.model = read_required_model(object, "model");
    config.filename = read_required_path(object, "filename");
    config.cache_filename = std::format("{}.bin", to_string(config.model));

    read_optional_string(object, "alias", config.alias);
    read_optional_value(object, "cache_filename", config.cache_filename);
    read_optional_string_vector(object, "extra_arguments", config.extra_arguments);

    return config;
}

void read_models(const json &root, std::vector<llama_model_config_s> &destination) {
    const auto it = root.find("models");

    if (it == root.end() || it->is_null()) {
        return;
    }

    if (!it->is_array()) {
        throw std::runtime_error{"'models' must be a JSON array"};
    }

    auto result = std::vector<llama_model_config_s>{};
    result.reserve(it->size());

    for (const auto &item : *it) {
        result.push_back(read_model_config(item));
    }

    destination = std::move(result);
}

} // namespace

namespace detail {

struct native_process_s {
#ifdef STZ_INTERN_PLATFORM_WINDOWS
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE job = nullptr;
    HANDLE output_read = nullptr;
#else
    pid_t pid = -1;
    int output_fd = -1;
#endif
};

} // namespace detail

namespace {

#ifdef STZ_INTERN_PLATFORM_WINDOWS

void start_native_process(detail::native_process_s &process,
                          const std::filesystem::path &executable,
                          const std::span<const std::string> arguments,
                          const std::filesystem::path &working_directory) {
    auto security_attributes = SECURITY_ATTRIBUTES{
            .nLength = sizeof(SECURITY_ATTRIBUTES),
            .lpSecurityDescriptor = nullptr,
            .bInheritHandle = TRUE,
    };

    auto output_read = HANDLE{};
    auto output_write = HANDLE{};

    if (CreatePipe(&output_read, &output_write, &security_attributes, 0) == FALSE) {
        throw std::runtime_error{std::format("CreatePipe failed: error={}", GetLastError())};
    }

    if (SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        CloseHandle(output_read);
        CloseHandle(output_write);

        throw std::runtime_error{std::format("SetHandleInformation failed: error={}", GetLastError())};
    }

    auto job = CreateJobObjectW(nullptr, nullptr);

    if (job == nullptr) {
        CloseHandle(output_read);
        CloseHandle(output_write);

        throw std::runtime_error{std::format("CreateJobObjectW failed: error={}", GetLastError())};
    }

    auto job_limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{};
    job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (SetInformationJobObject(job,
                                JobObjectExtendedLimitInformation,
                                &job_limits,
                                static_cast<DWORD>(sizeof(job_limits))) == FALSE) {
        const auto error = GetLastError();

        CloseHandle(job);
        CloseHandle(output_read);
        CloseHandle(output_write);

        throw std::runtime_error{std::format("SetInformationJobObject failed: error={}", error)};
    }

    auto startup_info = STARTUPINFOW{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = output_write;
    startup_info.hStdError = output_write;

    auto process_info = PROCESS_INFORMATION{};

    auto command_line = make_windows_command_line(executable, arguments);
    auto command_buffer = std::vector<wchar_t>{
            command_line.begin(),
            command_line.end(),
    };

    command_buffer.push_back(L'\0');

    const auto created = CreateProcessW(executable.wstring().c_str(),
                                        command_buffer.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
                                        nullptr,
                                        working_directory.wstring().c_str(),
                                        &startup_info,
                                        &process_info);

    CloseHandle(output_write);

    if (created == FALSE) {
        const auto error = GetLastError();

        CloseHandle(job);
        CloseHandle(output_read);

        throw std::runtime_error{
                std::format("CreateProcessW failed for '{}': error={}", path_to_utf8(executable), error)};
    }

    if (AssignProcessToJobObject(job, process_info.hProcess) == FALSE) {
        const auto error = GetLastError();

        TerminateProcess(process_info.hProcess, 1);
        WaitForSingleObject(process_info.hProcess, 5000);

        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        CloseHandle(job);
        CloseHandle(output_read);

        throw std::runtime_error{std::format("AssignProcessToJobObject failed: error={}", error)};
    }

    if (ResumeThread(process_info.hThread) == static_cast<DWORD>(-1)) {
        const auto error = GetLastError();

        TerminateProcess(process_info.hProcess, 1);
        WaitForSingleObject(process_info.hProcess, 5000);

        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        CloseHandle(job);
        CloseHandle(output_read);

        throw std::runtime_error{std::format("ResumeThread failed: error={}", error)};
    }

    process.process = process_info.hProcess;
    process.thread = process_info.hThread;
    process.job = job;
    process.output_read = output_read;
}

[[nodiscard]] bool native_process_alive(detail::native_process_s &process) noexcept {
    if (process.process == nullptr) {
        return false;
    }

    auto exit_code = DWORD{};

    if (GetExitCodeProcess(process.process, &exit_code) == FALSE) {
        return false;
    }

    return exit_code == STILL_ACTIVE;
}

void terminate_native_process(detail::native_process_s &process, const std::chrono::milliseconds timeout) noexcept {
    if (process.process == nullptr) {
        return;
    }

    if (!native_process_alive(process)) {
        return;
    }

    TerminateProcess(process.process, 0);

    const auto wait_milliseconds = static_cast<DWORD>(
            std::min<std::int64_t>(timeout.count(), std::numeric_limits<DWORD>::max()));

    WaitForSingleObject(process.process, wait_milliseconds);

    if (process.job != nullptr) {
        CloseHandle(process.job);
        process.job = nullptr;
    }

    WaitForSingleObject(process.process, wait_milliseconds);
}

void close_native_process(detail::native_process_s &process) noexcept {
    if (process.job != nullptr) {
        CloseHandle(process.job);
        process.job = nullptr;
    }

    if (process.output_read != nullptr) {
        CloseHandle(process.output_read);
        process.output_read = nullptr;
    }

    if (process.thread != nullptr) {
        CloseHandle(process.thread);
        process.thread = nullptr;
    }

    if (process.process != nullptr) {
        CloseHandle(process.process);
        process.process = nullptr;
    }
}

[[nodiscard]] std::ptrdiff_t read_native_output(detail::native_process_s &process,
                                                const std::span<char> buffer) noexcept {
    if (process.output_read == nullptr) {
        return 0;
    }

    auto bytes_read = DWORD{};

    const auto success = ReadFile(process.output_read,
                                  buffer.data(),
                                  static_cast<DWORD>(buffer.size()),
                                  &bytes_read,
                                  nullptr);

    if (success == FALSE || bytes_read == 0) {
        return 0;
    }

    return static_cast<std::ptrdiff_t>(bytes_read);
}

#else

void start_native_process(detail::native_process_s &process,
                          const std::filesystem::path &executable,
                          const std::span<const std::string> arguments,
                          const std::filesystem::path &) {
    auto pipe_fds = std::array<int, 2>{};

    if (::pipe(pipe_fds.data()) != 0) {
        throw std::runtime_error{std::format("pipe() failed: {}", std::strerror(errno))};
    }

    auto file_actions = posix_spawn_file_actions_t{};
    auto attributes = posix_spawnattr_t{};

    posix_spawn_file_actions_init(&file_actions);
    posix_spawnattr_init(&attributes);

    posix_spawn_file_actions_adddup2(&file_actions, pipe_fds[1], STDOUT_FILENO);

    posix_spawn_file_actions_adddup2(&file_actions, pipe_fds[1], STDERR_FILENO);

    posix_spawn_file_actions_addclose(&file_actions, pipe_fds[0]);
    posix_spawn_file_actions_addclose(&file_actions, pipe_fds[1]);

    auto flags = static_cast<short>(POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setflags(&attributes, flags);
    posix_spawnattr_setpgroup(&attributes, 0);

    auto argument_storage = std::vector<std::string>{};
    argument_storage.reserve(arguments.size() + 1);

    argument_storage.push_back(path_to_utf8(executable));

    for (const auto &argument : arguments) {
        argument_storage.push_back(argument);
    }

    auto argv = std::vector<char *>{};
    argv.reserve(argument_storage.size() + 1);

    for (auto &argument : argument_storage) {
        argv.push_back(argument.data());
    }

    argv.push_back(nullptr);

    auto pid = pid_t{};

    const auto result = posix_spawn(&pid, executable.c_str(), &file_actions, &attributes, argv.data(), environ);

    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&file_actions);

    ::close(pipe_fds[1]);

    if (result != 0) {
        ::close(pipe_fds[0]);

        throw std::runtime_error{
                std::format("posix_spawn failed for '{}': {}", path_to_utf8(executable), std::strerror(result))};
    }

    process.pid = pid;
    process.output_fd = pipe_fds[0];
}

[[nodiscard]] bool native_process_alive(detail::native_process_s &process) noexcept {
    if (process.pid <= 0) {
        return false;
    }

    auto status = int{};
    const auto result = ::waitpid(process.pid, &status, WNOHANG);

    if (result == 0) {
        return true;
    }

    if (result == process.pid) {
        process.pid = -1;
        return false;
    }

    return errno != ECHILD;
}

void terminate_native_process(detail::native_process_s &process, const std::chrono::milliseconds timeout) noexcept {
    if (process.pid <= 0) {
        return;
    }

    const auto pid = process.pid;

    if (::kill(-pid, SIGTERM) != 0) {
        ::kill(pid, SIGTERM);
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        auto status = int{};
        const auto result = ::waitpid(pid, &status, WNOHANG);

        if (result == pid || (result < 0 && errno == ECHILD)) {
            process.pid = -1;
            return;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    if (::kill(-pid, SIGKILL) != 0) {
        ::kill(pid, SIGKILL);
    }

    auto status = int{};
    ::waitpid(pid, &status, 0);
    process.pid = -1;
}

void close_native_process(detail::native_process_s &process) noexcept {
    if (process.output_fd >= 0) {
        ::close(process.output_fd);
        process.output_fd = -1;
    }
}

[[nodiscard]] std::ptrdiff_t read_native_output(detail::native_process_s &process,
                                                const std::span<char> buffer) noexcept {
    if (process.output_fd < 0) {
        return 0;
    }

    const auto result = ::read(process.output_fd, buffer.data(), buffer.size());

    if (result <= 0) {
        return 0;
    }

    return result;
}

#endif

} // namespace

std::string_view to_string(const model_e model) noexcept {
    switch (model) {
        case model_e::economy: return "economy";
        case model_e::standard: return "standard";
        case model_e::premium: return "premium";
    }

    return "standard";
}

model_e model_from_string(const std::string_view text) {
    if (text == "economy") {
        return model_e::economy;
    }

    if (text == "standard") {
        return model_e::standard;
    }

    if (text == "premium") {
        return model_e::premium;
    }

    throw std::runtime_error{std::format("Unknown model '{}'", text)};
}

LlamaServerSession::LlamaServerSession(LlamaServer &server,
                                       const std::uint64_t generation_id,
                                       const std::size_t slot_id) noexcept
    : m_server{&server},
      m_generation_id{generation_id},
      m_slot_id{slot_id} {}

llama_server_config_s load_server_config(std::filesystem::path application_directory,
                                         const std::filesystem::path &filename) try {
    if (application_directory.empty()) {
        throw std::runtime_error{"Application directory must not be empty"};
    }

    if (filename.empty()) {
        throw std::runtime_error{"Server config filename must not be empty"};
    }

    const auto content = util::read_text_file(application_directory / filename);

    if (content.empty()) {
        throw std::runtime_error{"Server config file is empty"};
    }

    const auto root = json::parse(content);

    if (!root.is_object()) {
        throw std::runtime_error{"Server config root must be a JSON object"};
    }

    auto config = llama_server_config_s{
            .application_directory = std::move(application_directory),
    };

    read_optional_path(root, "executable", config.executable);

    read_optional_path(root, "log_file", config.log_file);

    config.endpoint = read_endpoint_config(root, std::move(config.endpoint));

    read_models(root, config.models);

    read_optional_model(root, "initial_model", config.initial_model);

    read_optional_size(root, "threads", config.threads);

    read_optional_size(root, "threads_batch", config.threads_batch);

    read_optional_size(root, "context_size", config.context_size);

    read_optional_size(root, "parallel_slots", config.parallel_slots);

    read_optional_size(root, "slot_id", config.slot_id);

    read_optional_value(root, "use_jinja", config.use_jinja);

    read_optional_value(root, "enable_prompt_cache", config.enable_prompt_cache);

    read_optional_value(root, "enable_slots", config.enable_slots);

    read_optional_value(root, "enable_metrics", config.enable_metrics);

    read_optional_value(root, "disable_builtin_ui", config.disable_builtin_ui);

    read_optional_value(root, "allow_remote_connections", config.allow_remote_connections);

    read_optional_value(root, "load_cache_on_start", config.load_cache_on_start);

    read_optional_value(root, "store_cache_on_stop", config.store_cache_on_stop);

    read_optional_path(root, "slot_cache_directory", config.slot_cache_directory);

    read_optional_path(root, "state_file", config.state_file);

    read_optional_milliseconds(root, "startup_timeout_ms", config.startup_timeout);

    read_optional_milliseconds(root, "health_poll_interval_ms", config.health_poll_interval);

    read_optional_milliseconds(root, "shutdown_timeout_ms", config.shutdown_timeout);

    read_optional_milliseconds(root, "http_timeout_ms", config.http_timeout);

    read_optional_string_vector(root, "extra_arguments", config.extra_arguments);

    validate_server_config(config);

    // Boost.JSON
    // nlohmanjson

    return config;
} catch (const std::exception &error) {
    throw std::runtime_error{
            std::format("Failed to load llama-server config '{}': {}", filename.string(), error.what())};
}

void validate_server_config(const llama_server_config_s &config) {
    if (config.application_directory.empty()) {
        throw std::runtime_error{"llama-server application directory is empty"};
    }

    if (config.executable.empty()) {
        throw std::runtime_error{"llama-server executable path is empty"};
    }

    if (config.log_file.empty()) {
        throw std::runtime_error{"llama-server log file path is empty"};
    }

    if (config.endpoint.host.empty()) {
        throw std::runtime_error{"llama-server host is empty"};
    }

    if (config.endpoint.port == 0) {
        throw std::runtime_error{"llama-server port is zero"};
    }

    if (config.endpoint.chat_completions_path.empty() || config.endpoint.chat_completions_path.front() != '/') {
        throw std::runtime_error{
                std::format("Invalid llama-server chat completions path '{}'", config.endpoint.chat_completions_path)};
    }

    if (!config.allow_remote_connections && !is_loopback_host(config.endpoint.host)) {
        throw std::runtime_error{std::format("Remote llama-server host '{}' is prohibited. "
                                             "Set 'allow_remote_connections' to true to allow it",
                                             config.endpoint.host)};
    }

    if (config.models.empty()) {
        throw std::runtime_error{"llama-server models list is empty"};
    }

    auto model_names = std::unordered_set<std::string>{};
    auto initial_model_found = false;

    for (const auto &model : config.models) {
        const auto model_name = std::string{to_string(model.model)};

        if (!model_names.insert(model_name).second) {
            throw std::runtime_error{std::format("Duplicate model '{}' in llama-server configuration", model_name)};
        }

        if (model.filename.empty()) {
            throw std::runtime_error{std::format("Model '{}' has an empty filename", model_name)};
        }

        if (config.enable_slots && !valid_cache_filename(model.cache_filename)) {
            throw std::runtime_error{std::format("Model '{}' has invalid cache filename '{}'. "
                                                 "It must be a plain .bin filename without directories",
                                                 model_name,
                                                 model.cache_filename)};
        }

        if (model.model == config.initial_model) {
            initial_model_found = true;
        }
    }

    if (!initial_model_found) {
        throw std::runtime_error{
                std::format("Initial model '{}' is not present in the models list", to_string(config.initial_model))};
    }

    if (config.threads == 0) {
        throw std::runtime_error{"llama-server threads must be greater than zero"};
    }

    if (config.threads_batch == 0) {
        throw std::runtime_error{"llama-server batch threads must be greater than zero"};
    }

    if (config.context_size == 0) {
        throw std::runtime_error{"llama-server context size must be greater than zero"};
    }

    if (config.parallel_slots == 0) {
        throw std::runtime_error{"llama-server parallel slot count must be greater than zero"};
    }

    if (config.slot_id >= config.parallel_slots) {
        throw std::runtime_error{
                std::format("Slot id {} is outside configured slot count {}", config.slot_id, config.parallel_slots)};
    }

    if (config.enable_slots && config.slot_cache_directory.empty()) {
        throw std::runtime_error{"llama-server slot cache directory is empty"};
    }

    if (config.state_file.empty()) {
        throw std::runtime_error{"llama-server state filename is empty"};
    }

    if (config.startup_timeout <= std::chrono::milliseconds::zero()) {
        throw std::runtime_error{"llama-server startup timeout must be positive"};
    }

    if (config.health_poll_interval <= std::chrono::milliseconds::zero()) {
        throw std::runtime_error{"llama-server health poll interval must be positive"};
    }

    if (config.shutdown_timeout <= std::chrono::milliseconds::zero()) {
        throw std::runtime_error{"llama-server shutdown timeout must be positive"};
    }

    if (config.http_timeout <= std::chrono::milliseconds::zero()) {
        throw std::runtime_error{"llama-server HTTP timeout must be positive"};
    }
}

LlamaServerSession::~LlamaServerSession() {
    if (m_server != nullptr) {
        m_server->finish_generation(m_generation_id);
    }
}

bool LlamaServerSession::stop_requested() const noexcept {
    return m_server != nullptr && m_server->generation_stop_requested(m_generation_id);
}

void LlamaServerSession::request_stop() noexcept {
    if (m_server != nullptr) {
        m_server->request_generation_stop(m_generation_id);
    }
}

std::size_t LlamaServerSession::slot_id() const noexcept { return m_slot_id; }

LlamaServer::LlamaServer(llama_server_config_s config, std::shared_ptr<spdlog::logger> logger)
    : m_config{std::move(config)},
      m_current_model{m_config.initial_model},
      m_logger{std::move(logger)} {
    assert(!m_config.application_directory.empty());
    assert(!m_config.executable.empty());
    assert(!m_config.log_file.empty());
    assert(m_logger != nullptr);

    attach_file_log_sink();
    load_previous_state_info();

    m_logger->info("LlamaServer initialized: application_directory='{}', log_file='{}'",
                   path_to_utf8(m_config.application_directory),
                   path_to_utf8(resolve_path(m_config.log_file)));
}

LlamaServer::~LlamaServer() {
    stop();
    if (m_logger != nullptr) {
        m_logger->flush();
    }
}

void LlamaServer::start() {
    auto lock = std::scoped_lock{m_lifecycle_mutex};
    start_unlocked();
}

void LlamaServer::start_unlocked() {
    if (m_process_started.load()) {
        return;
    }

    validate_configuration();

    const auto &selected_model = model_config(m_current_model.load());

    const auto cache_directory = slot_cache_directory_path();
    const auto state_filename = state_file_path();

    std::filesystem::create_directories(cache_directory);

    if (!state_filename.parent_path().empty()) {
        std::filesystem::create_directories(state_filename.parent_path());
    }

    const auto executable = executable_path();
    const auto arguments = make_server_arguments(selected_model);

    m_process = std::make_unique<detail::native_process_s>();

    try {
        m_logger->info("Starting llama-server with model '{}'", model_display_name(selected_model));

        start_native_process(*m_process, executable, arguments, executable.parent_path());

        m_process_started.store(true);
        m_is_stopping.store(false);
        m_is_running.store(false);

        {
            auto state_lock = std::scoped_lock{m_state_mutex};
            m_started_at = util::make_local_timestamp();
            m_last_error.clear();
        }

        m_output_thread = std::jthread{[this](std::stop_token) { read_server_output(); }};

        update_server_state_info();
        wait_until_ready();

        m_is_running.store(true);
        append_used_model(selected_model);

        m_logger->info("llama-server is ready at {}", url());

        update_server_state_info();

        if (m_config.load_cache_on_start) {
            try {
                load_model_cache_unlocked();
            } catch (const std::exception &error) {
                log_warning(std::format("Failed to restore model cache: {}", error.what()));
            }
        }

        update_server_state_info();
    } catch (const std::exception &error) {
        set_last_error(error.what());
        log_error(std::format("Failed to start llama-server: {}", error.what()));

        terminate_process_unlocked();
        update_server_state_info();

        throw;
    }
}

void LlamaServer::stop() noexcept {
    try {
        auto lock = std::scoped_lock{m_lifecycle_mutex};
        stop_unlocked();
    } catch (const std::exception &error) {
        log_error(std::format("Failed to stop llama-server: {}", error.what()));
    } catch (...) {
        log_error("Failed to stop llama-server: unknown error");
    }
}

void LlamaServer::stop_unlocked() noexcept {
    if (!m_process_started.load()) {
        m_is_running.store(false);
        update_server_state_info();

        if (m_logger != nullptr) {
            m_logger->flush();
        }

        return;
    }

    if (m_config.store_cache_on_stop && m_is_running.load() && !m_model_generates.load()) {
        try {
            store_model_cache_unlocked();
        } catch (const std::exception &error) {
            log_warning(std::format("Failed to store model cache before shutdown: {}", error.what()));
        }
    }

    if (m_model_generates.load()) {
        stop_generating();
    }

    terminate_process_unlocked();

    m_logger->info("llama-server stopped");

    update_server_state_info();

    if (m_logger != nullptr) {
        m_logger->flush();
    }
}

void LlamaServer::terminate_process_unlocked() noexcept {
    m_is_stopping.store(true);
    m_is_running.store(false);

    if (m_process != nullptr) {
        terminate_native_process(*m_process, m_config.shutdown_timeout);
    }

    if (m_output_thread.joinable()) {
        m_output_thread.join();
    }

    if (m_process != nullptr) {
        close_native_process(*m_process);
        m_process.reset();
    }

    m_process_started.store(false);
    m_is_running.store(false);
    m_is_stopping.store(false);
}

bool LlamaServer::is_running() const noexcept { return m_is_running.load(); }

bool LlamaServer::process_started() const noexcept { return m_process_started.load(); }

bool LlamaServer::model_generates() const noexcept { return m_model_generates.load(); }

std::string LlamaServer::url() const {
    if (m_config.endpoint.host.find(':') != std::string::npos && !m_config.endpoint.host.starts_with('[')) {
        return std::format("http://[{}]:{}", m_config.endpoint.host, m_config.endpoint.port);
    }

    return std::format("http://{}:{}", m_config.endpoint.host, m_config.endpoint.port);
}

llama_endpoint_config_s LlamaServer::endpoint_config() const { return m_config.endpoint; }

model_e LlamaServer::current_model() const noexcept { return m_current_model.load(); }

bool LlamaServer::model_available(const model_e model) const {
    const auto &config = model_config(model);

    return std::filesystem::is_regular_file(model_path(config));
}

std::span<const llama_model_config_s> LlamaServer::models() const noexcept {
    return {
            m_config.models.data(),
            m_config.models.size(),
    };
}

unique_llama_session_ptr LlamaServer::start_generation() {
    if (!m_is_running.load()) {
        throw std::runtime_error{"Cannot start generation: llama-server is not ready"};
    }

    auto expected = false;

    if (!m_model_generates.compare_exchange_strong(expected, true)) {
        throw std::runtime_error{"Cannot start generation: another generation is active"};
    }

    const auto generation_id = m_next_generation_id.fetch_add(1);

    m_active_generation_id.store(generation_id);
    m_stop_generation_requested.store(false);

    update_server_state_info();

    return unique_llama_session_ptr{new LlamaServerSession{
            *this,
            generation_id,
            m_config.slot_id,
    }};
}

void LlamaServer::stop_generating() noexcept {
    const auto generation_id = m_active_generation_id.load();

    if (generation_id == 0 || !m_model_generates.load()) {
        return;
    }

    request_generation_stop(generation_id);

    m_logger->info("Generation stop requested: generation_id={}", generation_id);

    try {
        update_server_state_info();
    } catch (...) {
    }
}

bool LlamaServer::change_model(const model_e model) {
    auto lock = std::scoped_lock{m_lifecycle_mutex};

    if (model == m_current_model.load()) {
        return false;
    }

    if (m_model_generates.load()) {
        throw std::runtime_error{"Cannot change model while generation is active"};
    }

    if (!model_available(model)) {
        throw std::runtime_error{std::format("Model '{}' is not available", to_string(model))};
    }

    const auto previous_model = m_current_model.load();
    const auto restart_required = m_process_started.load();

    if (restart_required) {
        stop_unlocked();
    }

    m_current_model.store(model);

    try {
        if (restart_required) {
            start_unlocked();
        } else {
            update_server_state_info();
        }
    } catch (...) {
        m_current_model.store(previous_model);

        if (restart_required) {
            try {
                start_unlocked();
            } catch (const std::exception &rollback_error) {
                set_last_error(std::format("Failed to restore previous model: {}", rollback_error.what()));

                log_error(std::format("Failed to restore previous model: {}", rollback_error.what()));
            }
        }

        throw;
    }

    return true;
}

void LlamaServer::store_model_cache() {
    auto lock = std::scoped_lock{m_lifecycle_mutex};
    store_model_cache_unlocked();
}

void LlamaServer::store_model_cache_unlocked() {
    if (!m_is_running.load()) {
        throw std::runtime_error{"Cannot store model cache: llama-server is not ready"};
    }

    if (m_model_generates.load()) {
        throw std::runtime_error{"Cannot store model cache while generation is active"};
    }

    if (!m_config.enable_slots) {
        throw std::runtime_error{"Cannot store model cache: slots endpoint is disabled"};
    }

    const auto &selected_model = model_config(m_current_model.load());

    perform_slot_action("save", selected_model);

    m_logger->info("Stored slot cache: slot={}, file='{}'",
                   m_config.slot_id,
                   path_to_utf8(slot_cache_path(selected_model)));
}

void LlamaServer::load_model_cache() {
    auto lock = std::scoped_lock{m_lifecycle_mutex};
    load_model_cache_unlocked();
}

void LlamaServer::load_model_cache_unlocked() {
    if (!m_is_running.load()) {
        throw std::runtime_error{"Cannot load model cache: llama-server is not ready"};
    }

    if (m_model_generates.load()) {
        throw std::runtime_error{"Cannot load model cache while generation is active"};
    }

    if (!m_config.enable_slots) {
        throw std::runtime_error{"Cannot load model cache: slots endpoint is disabled"};
    }

    const auto &selected_model = model_config(m_current_model.load());
    const auto cache_filename = slot_cache_path(selected_model);

    if (!std::filesystem::exists(cache_filename)) {
        m_logger->info("Slot cache does not exist yet: {}", path_to_utf8(cache_filename));

        return;
    }

    perform_slot_action("restore", selected_model);

    m_logger->info("Restored slot cache: slot={}, file='{}'", m_config.slot_id, path_to_utf8(cache_filename));
}

void LlamaServer::erase_model_cache() {
    auto lock = std::scoped_lock{m_lifecycle_mutex};
    erase_model_cache_unlocked();
}

void LlamaServer::erase_model_cache_unlocked() {
    if (!m_is_running.load()) {
        throw std::runtime_error{"Cannot erase model cache: llama-server is not ready"};
    }

    if (m_model_generates.load()) {
        throw std::runtime_error{"Cannot erase model cache while generation is active"};
    }

    auto client = httplib::Client{m_config.endpoint.host, m_config.endpoint.port};

    configure_http_timeouts(client, m_config.http_timeout);

    const auto endpoint = std::format("/slots/{}?action=erase", m_config.slot_id);

    const auto response = client.Post(endpoint, make_http_headers(m_config.endpoint), "", "application/json");

    if (!response) {
        throw std::runtime_error{std::format("Slot erase request failed: {}", httplib::to_string(response.error()))};
    }

    if (response->status < 200 || response->status >= 300) {
        throw std::runtime_error{
                std::format("Slot erase request failed: HTTP {}, body={}", response->status, response->body)};
    }

    const auto &selected_model = model_config(m_current_model.load());
    const auto cache_filename = slot_cache_path(selected_model);

    std::error_code error;
    std::filesystem::remove(cache_filename, error);

    if (error) {
        log_warning(std::format("Failed to delete slot cache file '{}': {}",
                                path_to_utf8(cache_filename),
                                error.message()));
    }

    m_logger->info("Erased slot cache: slot={}", m_config.slot_id);
}

void LlamaServer::perform_slot_action(const std::string_view action, const llama_model_config_s &model) {
    auto client = httplib::Client{m_config.endpoint.host, m_config.endpoint.port};

    configure_http_timeouts(client, m_config.http_timeout);

    const auto endpoint = std::format("/slots/{}?action={}", m_config.slot_id, action);

    const auto body =
            json{
                    {"filename", model.cache_filename},
            }
                    .dump();

    const auto response = client.Post(endpoint, make_http_headers(m_config.endpoint), body, "application/json");

    if (!response) {
        throw std::runtime_error{
                std::format("Slot '{}' request failed: {}", action, httplib::to_string(response.error()))};
    }

    if (response->status < 200 || response->status >= 300) {
        throw std::runtime_error{
                std::format("Slot '{}' request failed: HTTP {}, body={}", action, response->status, response->body)};
    }
}

void LlamaServer::validate_configuration() const {
    validate_server_config(m_config);

    const auto executable = executable_path();

    if (!std::filesystem::is_regular_file(executable)) {
        throw std::runtime_error{std::format("llama-server executable does not exist: '{}'", path_to_utf8(executable))};
    }

    const auto &selected_model = model_config(m_current_model.load());

    const auto selected_model_path = model_path(selected_model);

    if (!std::filesystem::is_regular_file(selected_model_path)) {
        throw std::runtime_error{std::format("Model file does not exist: '{}'", path_to_utf8(selected_model_path))};
    }
}

const llama_model_config_s &LlamaServer::model_config(const model_e model) const {
    const auto it = std::ranges::find(m_config.models, model, &llama_model_config_s::model);

    if (it == m_config.models.end()) {
        throw std::runtime_error{
                std::format("Model '{}' is not present in llama-server configuration", to_string(model))};
    }

    return *it;
}

std::filesystem::path LlamaServer::resolve_path(const std::filesystem::path &path) const {
    if (path.is_absolute()) {
        return path.lexically_normal();
    }

    return std::filesystem::absolute(m_config.application_directory / path).lexically_normal();
}

std::filesystem::path LlamaServer::executable_path() const { return resolve_path(m_config.executable); }

std::filesystem::path LlamaServer::model_path(const llama_model_config_s &model) const {
    return resolve_path(model.filename);
}

std::filesystem::path LlamaServer::slot_cache_directory_path() const {
    return resolve_path(m_config.slot_cache_directory);
}

std::filesystem::path LlamaServer::slot_cache_path(const llama_model_config_s &model) const {
    return slot_cache_directory_path() / model.cache_filename;
}

std::filesystem::path LlamaServer::state_file_path() const { return resolve_path(m_config.state_file); }

std::vector<std::string> LlamaServer::make_server_arguments(const llama_model_config_s &model) const {
    auto arguments = std::vector<std::string>{};

    arguments.reserve(32 + model.extra_arguments.size() + m_config.extra_arguments.size());

    arguments.emplace_back("--model");
    arguments.push_back(path_to_utf8(model_path(model)));

    arguments.emplace_back("--host");
    arguments.push_back(m_config.endpoint.host);

    arguments.emplace_back("--port");
    arguments.push_back(std::to_string(m_config.endpoint.port));

    arguments.emplace_back("--threads");
    arguments.push_back(std::to_string(m_config.threads));

    arguments.emplace_back("--threads-batch");
    arguments.push_back(std::to_string(m_config.threads_batch));

    arguments.emplace_back("--ctx-size");
    arguments.push_back(std::to_string(m_config.context_size));

    arguments.emplace_back("--parallel");
    arguments.push_back(std::to_string(m_config.parallel_slots));

    if (model.alias.has_value() && !model.alias->empty()) {
        arguments.emplace_back("--alias");
        arguments.push_back(*model.alias);
    }

    if (m_config.use_jinja) {
        arguments.emplace_back("--jinja");
    }

    if (m_config.enable_prompt_cache) {
        arguments.emplace_back("--cache-prompt");
    } else {
        arguments.emplace_back("--no-cache-prompt");
    }

    if (m_config.enable_slots) {
        arguments.emplace_back("--slots");
        arguments.emplace_back("--slot-save-path");
        arguments.push_back(path_to_utf8(slot_cache_directory_path()));
    } else {
        arguments.emplace_back("--no-slots");
    }

    if (m_config.enable_metrics) {
        arguments.emplace_back("--metrics");
    }

    if (m_config.disable_builtin_ui) {
        arguments.emplace_back("--no-ui");
        arguments.emplace_back("--no-webui");
    }

    if (m_config.endpoint.api_key.has_value() && !m_config.endpoint.api_key->empty()) {
        arguments.emplace_back("--api-key");
        arguments.push_back(*m_config.endpoint.api_key);
    }

    arguments.insert(arguments.end(), model.extra_arguments.begin(), model.extra_arguments.end());

    arguments.insert(arguments.end(), m_config.extra_arguments.begin(), m_config.extra_arguments.end());

    return arguments;
}

bool LlamaServer::health_ready() const {
    auto client = httplib::Client{m_config.endpoint.host, m_config.endpoint.port};

    configure_http_timeouts(client, std::chrono::seconds{2});

    const auto response = client.Get("/health");

    if (!response || response->status != 200) {
        return false;
    }

    try {
        const auto body = json::parse(response->body);

        return body.value("status", "") == "ok";
    } catch (...) {
        return false;
    }
}

void LlamaServer::wait_until_ready() {
    const auto deadline = std::chrono::steady_clock::now() + m_config.startup_timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        if (m_process == nullptr || !native_process_alive(*m_process)) {
            throw std::runtime_error{"llama-server process exited before becoming ready"};
        }

        if (health_ready()) {
            return;
        }

        std::this_thread::sleep_for(m_config.health_poll_interval);
    }

    throw std::runtime_error{
            std::format("llama-server did not become ready within {} ms", m_config.startup_timeout.count())};
}

void LlamaServer::read_server_output() {
    auto buffer = std::array<char, 4096>{};
    auto pending = std::string{};

    while (m_process != nullptr) {
        const auto read_size = read_native_output(*m_process, std::span<char>{buffer});

        if (read_size <= 0) {
            break;
        }

        pending.append(buffer.data(), static_cast<std::size_t>(read_size));

        while (true) {
            const auto newline = pending.find('\n');

            if (newline == std::string::npos) {
                break;
            }

            auto line = pending.substr(0, newline);
            pending.erase(0, newline + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            log_server_line(std::move(line));
        }
    }

    if (!pending.empty()) {
        log_server_line(std::move(pending));
    }

    if (!m_is_stopping.load() && m_process_started.exchange(false)) {
        m_is_running.store(false);

        log_warning("llama-server process output stream was closed unexpectedly");

        try {
            update_server_state_info();
        } catch (...) {
        }
    }
}

void LlamaServer::log_server_line(std::string line) {
    if (line.empty()) {
        return;
    }

    const auto normalized = lowercase_ascii(line);

    if (normalized.find("error") != std::string::npos || normalized.find("[err") != std::string::npos) {
        m_error_count.fetch_add(1);
        m_logger->error("[llama-server] {}", line);
        return;
    }

    if (normalized.find("warn") != std::string::npos) {
        m_warning_count.fetch_add(1);
        m_logger->warn("[llama-server] {}", line);
        return;
    }

    m_logger->info("[llama-server] {}", line);
}

void LlamaServer::load_previous_state_info() {
    const auto filename = state_file_path();

    if (!std::filesystem::exists(filename)) {
        return;
    }

    try {
        const auto root = json::parse(util::read_text_file(filename));

        m_warning_count.store(root.value("warning_count", std::uint64_t{}));

        m_error_count.store(root.value("error_count", std::uint64_t{}));

        m_models_used = root.value("models_used", std::vector<std::string>{});

        m_last_error = root.value("last_error", "");
    } catch (const std::exception &error) {
        m_logger->warn("Failed to load previous llama-server state '{}': {}", path_to_utf8(filename), error.what());
    }
}

void LlamaServer::append_used_model(const llama_model_config_s &model) {
    auto lock = std::scoped_lock{m_state_mutex};
    m_models_used.push_back(model_display_name(model));
}

void LlamaServer::set_last_error(std::string message) {
    auto lock = std::scoped_lock{m_state_mutex};
    m_last_error = std::move(message);
}

void LlamaServer::log_warning(std::string message) {
    m_warning_count.fetch_add(1);
    m_logger->warn("{}", message);
}

void LlamaServer::log_error(std::string message) {
    m_error_count.fetch_add(1);
    m_logger->error("{}", message);
}

void LlamaServer::attach_file_log_sink() {
    const auto filename = resolve_path(m_config.log_file);
    const auto parent_directory = filename.parent_path();

    if (!parent_directory.empty()) {
        std::filesystem::create_directories(parent_directory);
    }

    try {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(filename.string(), true);

        file_sink->set_level(spdlog::level::trace);

        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");

        m_logger->sinks().push_back(std::move(file_sink));

        m_logger->flush_on(spdlog::level::info);

        m_logger->info("llama-server file logging enabled: '{}'", path_to_utf8(filename));
    } catch (const std::exception &error) {
        throw std::runtime_error{
                std::format("Failed to create llama-server log file '{}': {}", path_to_utf8(filename), error.what())};
    }
}

void LlamaServer::finish_generation(const std::uint64_t generation_id) noexcept {
    if (m_active_generation_id.load() != generation_id) {
        return;
    }

    m_stop_generation_requested.store(false);
    m_active_generation_id.store(0);
    m_model_generates.store(false);

    try {
        update_server_state_info();
    } catch (...) {
    }
}

bool LlamaServer::generation_stop_requested(const std::uint64_t generation_id) const noexcept {
    return m_active_generation_id.load() == generation_id && m_stop_generation_requested.load();
}

void LlamaServer::request_generation_stop(const std::uint64_t generation_id) noexcept {
    if (m_active_generation_id.load() != generation_id) {
        return;
    }

    m_stop_generation_requested.store(true);
}

llama_server_state_info_s LlamaServer::state_info() const {
    auto lock = std::scoped_lock{m_state_mutex};

    const auto &selected_model = model_config(m_current_model.load());

    return llama_server_state_info_s{
            .process_started = m_process_started.load(),
            .running = m_is_running.load(),
            .model_generates = m_model_generates.load(),
            .current_model = m_current_model.load(),
            .current_model_alias = model_display_name(selected_model),
            .url = url(),
            .models_used = m_models_used,
            .warning_count = m_warning_count.load(),
            .error_count = m_error_count.load(),
            .started_at = m_started_at,
            .updated_at = m_updated_at,
            .last_error = m_last_error,
    };
}

void LlamaServer::update_server_state_info() {
    const auto filename = state_file_path();

    if (!filename.parent_path().empty()) {
        std::filesystem::create_directories(filename.parent_path());
    }

    auto state_lock = std::scoped_lock{m_state_mutex};

    m_updated_at = util::make_local_timestamp();

    const auto &selected_model = model_config(m_current_model.load());

    auto available_models = json::array();

    for (const auto &model : m_config.models) {
        available_models.push_back(json{
                {"id", std::string{to_string(model.model)}},
                {"alias", model_display_name(model)},
                {"available", std::filesystem::is_regular_file(model_path(model))},
        });
    }

    const auto output = json{
            {"version", 1},
            {"process_started", m_process_started.load()},
            {"running", m_is_running.load()},
            {"model_generates", m_model_generates.load()},
            {"stop_generation_requested", m_stop_generation_requested.load()},
            {"url", url()},
            {"current_model", std::string{to_string(m_current_model.load())}},
            {"current_model_alias", model_display_name(selected_model)},
            {"models", std::move(available_models)},
            {"models_used", m_models_used},
            {"warning_count", m_warning_count.load()},
            {"error_count", m_error_count.load()},
            {"started_at", m_started_at},
            {"updated_at", m_updated_at},
            {"last_error", m_last_error},
    };

    util::write_text_file_atomic(filename, output.dump(2, ' ', false));
}

} // namespace stz::intern::llm