#include "engine.hpp"

#include "emojis.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

#include "util/file_io.hpp"
#include "util/string_helpers.hpp"
#include "util/time.hpp"

namespace stz::intern {

namespace {

[[nodiscard]] engine_config_s validate_engine_config(engine_config_s config) {
    if (config.history_file.empty()) {
        throw std::runtime_error{"Engine history file path is empty"};
    }

    if (config.texting_history_file.empty()) {
        throw std::runtime_error{"Texting history file path is empty"};
    }

    if (config.assistant_profile_state_file.empty()) {
        throw std::runtime_error{"Assistant profile state file path is empty"};
    }

    if (config.organization_config_file.empty()) {
        throw std::runtime_error{"Organization config file path is empty"};
    }

    if (config.knowledge_directory.empty()) {
        throw std::runtime_error{"Engine knowledge directory path is empty"};
    }

    if (config.texting_style == texting_style_e::any) {
        throw std::runtime_error{"texting_style must be formal, neutral or friendly"};
    }

    if (config.max_knowledge_documents != 0 && config.max_knowledge_chars_per_document == 0) {
        throw std::runtime_error{"max_knowledge_chars_per_document must be positive "
                                 "when knowledge retrieval is enabled"};
    }

    if (config.max_knowledge_documents != 0 && config.max_prompt_knowledge_chars_per_document == 0) {
        throw std::runtime_error{"max_prompt_knowledge_chars_per_document must be positive "
                                 "when knowledge retrieval is enabled"};
    }

    if (config.max_knowledge_documents != 0 && config.max_expansion_knowledge_chars_per_document == 0) {
        throw std::runtime_error{"max_expansion_knowledge_chars_per_document must be positive "
                                 "when knowledge retrieval is enabled"};
    }

    if (config.max_texting_selected_scripts == 0) {
        throw std::runtime_error{"max_texting_selected_scripts must be positive"};
    }

    if (config.max_texting_selector_input_chars == 0) {
        throw std::runtime_error{"max_texting_selector_input_chars must be positive"};
    }

    if (config.max_texting_selector_tokens <= 0) {
        throw std::runtime_error{"max_texting_selector_tokens must be positive"};
    }

    if (config.max_texting_normalizer_small_context_chars == 0 ||
        config.max_texting_normalizer_medium_context_chars == 0 ||
        config.max_texting_normalizer_large_context_chars == 0) {
        throw std::runtime_error{"texting normalizer context sizes must be positive"};
    }

    if (config.max_texting_normalizer_small_context_chars >
            config.max_texting_normalizer_medium_context_chars ||
        config.max_texting_normalizer_medium_context_chars >
            config.max_texting_normalizer_large_context_chars) {
        throw std::runtime_error{"texting normalizer context sizes must be ordered"};
    }

    if (config.max_texting_normalizer_tokens <= 0) {
        throw std::runtime_error{"max_texting_normalizer_tokens must be positive"};
    }

    if (config.max_texting_adaptation_input_chars == 0) {
        throw std::runtime_error{"max_texting_adaptation_input_chars must be positive"};
    }

    if (config.max_texting_adaptation_chars_per_script == 0) {
        throw std::runtime_error{"max_texting_adaptation_chars_per_script must be positive"};
    }

    if (config.max_texting_script_edits == 0) {
        throw std::runtime_error{"max_texting_script_edits must be positive"};
    }

    if (config.max_texting_adaptation_tokens <= 0) {
        throw std::runtime_error{"max_texting_adaptation_tokens must be positive"};
    }

    if (config.max_texting_answer_tokens <= 0) {
        throw std::runtime_error{"max_texting_answer_tokens must be positive"};
    }

    if (config.max_texting_prompt_chars_per_script == 0) {
        throw std::runtime_error{"max_texting_prompt_chars_per_script must be positive"};
    }

    const auto validate_temperature = [](const double value, const std::string_view name) {
        if (value < 0.0 || value > 2.0) {
            throw std::runtime_error{std::format("{} must be in range [0, 2]", name)};
        }
    };

    validate_temperature(config.texting_formal_temperature, "texting_formal_temperature");
    validate_temperature(config.texting_neutral_temperature, "texting_neutral_temperature");
    validate_temperature(config.texting_friendly_temperature, "texting_friendly_temperature");

    if (config.texting_top_p <= 0.0 || config.texting_top_p > 1.0) {
        throw std::runtime_error{"texting_top_p must be in range (0, 1]"};
    }

    if (config.max_context_chars_per_user_message == 0) {
        throw std::runtime_error{"max_context_chars_per_user_message must be positive"};
    }

    if (config.max_context_source_files == 0) {
        throw std::runtime_error{"max_context_source_files must be positive"};
    }

    if (config.max_contextual_retrieval_chars == 0) {
        throw std::runtime_error{"max_contextual_retrieval_chars must be positive"};
    }

    if (config.max_transform_answer_chars == 0) {
        throw std::runtime_error{"max_transform_answer_chars must be positive"};
    }

    return config;
}

[[nodiscard]] std::filesystem::path workflow_knowledge_directory(
        const std::filesystem::path &knowledge_directory) {
    const auto profile_directory = knowledge_directory / "workflow";

    if (std::filesystem::exists(profile_directory)) {
        return profile_directory;
    }

    return knowledge_directory;
}

[[nodiscard]] std::shared_ptr<spdlog::logger> clone_logger(const std::shared_ptr<spdlog::logger> &logger,
                                                           const std::string_view name) {
    assert(logger != nullptr);

    if (logger == nullptr) {
        std::terminate();
    }

    return logger->clone(std::string{name});
}

void require_loaded(const bool loaded) {
    assert(loaded);

    if (!loaded) {
        std::terminate();
    }
}

class request_active_guard_s final {
public:
    explicit request_active_guard_s(std::atomic_bool &active)
        : m_active{&active} {
        auto expected = false;

        if (!m_active->compare_exchange_strong(expected, true)) {
            throw std::runtime_error{"Another assistant request is already active"};
        }
    }

    ~request_active_guard_s() {
        m_active->store(false);
    }

    request_active_guard_s(const request_active_guard_s &) = delete;
    request_active_guard_s &operator=(const request_active_guard_s &) = delete;

private:
    std::atomic_bool *m_active = nullptr;
};

[[nodiscard]] std::string markdown_inline_code(const std::string_view text) {
    auto escaped = std::string{text};

    std::ranges::replace(escaped, '`', '\'');

    return std::format("`{}`", escaped);
}

[[nodiscard]] std::string join_source_filenames(const std::span<const std::string> source_filenames) {
    if (source_filenames.empty()) {
        return "подходящие `Markdown`-файлы не найдены";
    }

    auto result = std::string{};

    for (auto index = std::size_t{}; index < source_filenames.size(); ++index) {
        if (index != 0) {
            result += ", ";
        }

        result += markdown_inline_code(source_filenames[index]);
    }

    return result;
}

[[nodiscard]] bool is_generated_service_line(const std::string_view line) {
    auto normalized = std::string{line};
    util::trim(normalized);

    return normalized.starts_with("⚠️ Бот может допускать ошибки") || normalized.starts_with("Опирался на файлы:") ||
           normalized.starts_with("Источники:") || normalized.starts_with("Источник:") ||
           normalized.starts_with("**Источники:**") || normalized.starts_with("**Источник:**");
}

[[nodiscard]] std::string remove_generated_service_lines(const std::string_view text) {
    auto stream = std::istringstream{std::string{text}};
    auto line = std::string{};
    auto result = std::string{};

    while (std::getline(stream, line)) {
        if (is_generated_service_line(line)) {
            continue;
        }

        if (!result.empty()) {
            result.push_back('\n');
        }

        result += line;
    }

    util::trim(result);

    return result;
}

[[nodiscard]] std::string remove_direct_answer_search_hints(const std::string_view text) {
    auto stream = std::istringstream{std::string{text}};
    auto line = std::string{};
    auto result = std::string{};

    while (std::getline(stream, line)) {
        if (line.contains("сценар") || line.contains("Сценар")) {
            if (const auto hint = line.find(" (например,"); hint != std::string::npos) {
                line.erase(hint);
            }
        }

        if (!result.empty()) {
            result.push_back('\n');
        }

        result += line;
    }

    util::trim(result);

    return result;
}

[[nodiscard]] std::size_t valid_utf8_prefix_size(const std::string_view text,
                                                 const std::size_t maximum_bytes) noexcept {
    auto size = std::min(text.size(), maximum_bytes);

    if (size == text.size()) {
        return size;
    }

    while (size > 0 && (static_cast<unsigned char>(text[size]) & 0xC0) == 0x80) {
        --size;
    }

    return size;
}

[[nodiscard]] std::size_t valid_utf8_suffix_offset(const std::string_view text,
                                                   const std::size_t desired_offset) noexcept {
    auto offset = std::min(desired_offset, text.size());

    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80) {
        ++offset;
    }

    return offset;
}

[[nodiscard]] std::string truncate_utf8(const std::string_view text, const std::size_t maximum_bytes) {
    if (text.size() <= maximum_bytes) {
        return std::string{text};
    }

    if (maximum_bytes == 0) {
        return {};
    }

    constexpr auto marker = std::string_view{"..."};

    if (maximum_bytes <= marker.size()) {
        return std::string{text.substr(0, valid_utf8_prefix_size(text, maximum_bytes))};
    }

    const auto prefix_size = valid_utf8_prefix_size(text, maximum_bytes - marker.size());

    return std::format("{}{}", text.substr(0, prefix_size), marker);
}

[[nodiscard]] std::string compact_whitespace(const std::string_view text) {
    auto result = std::string{};
    result.reserve(text.size());

    auto previous_space = false;

    for (const auto ch : text) {
        const auto byte = static_cast<unsigned char>(ch);

        if (byte < 128 && std::isspace(byte) != 0) {
            if (!result.empty() && !previous_space) {
                result.push_back(' ');
            }

            previous_space = true;
            continue;
        }

        result.push_back(ch);
        previous_space = false;
    }

    util::trim(result);

    return result;
}

struct texting_issue_analysis_s {
    bool specified = false;
    std::string summary = {};
    std::string reaction = {};
    std::string photo = {};
};

[[nodiscard]] std::string build_texting_selector_system_prompt() {
    return
            "Ты выполняешь только маршрутизацию клиентского сообщения по сценариям базы знаний. "
            "Не составляй ответ клиенту. Верни строго один JSON-объект без Markdown и пояснений: "
            "{\"queries\":[\"сценарий 1\",\"сценарий 2\"]}.\n"
            "Сформулируй от одного до трёх коротких запросов на русском языке, каждый от трёх до восьми слов. "
            "Пиши их от третьего лица в форме названия ситуации: «Клиент ...». Первый запрос должен отражать "
            "главную деловую ситуацию, остальные — только явно выраженные дополнительные требования. "
            "Не додумывай отмену записи, возврат, скидку, гарантию или компенсацию. Фраза «больше не приду» сама "
            "по себе не означает отмену уже существующей записи. Отличай положительный отзыв от жалобы. При "
            "явной жалобе на качество первым ставь общий сценарий недовольства, а затем конкретные требования. "
            "При положительном отзыве с вопросом или просьбой первым ставь именно практический запрос клиента.\n"
            "Примеры:\n"
            "«Всё понравилось. Когда ближайшее время?» -> "
            "{\"queries\":[\"Клиент спрашивает ближайшее свободное время\"]}\n"
            "«Покрытие отслоилось через три дня» -> "
            "{\"queries\":[\"Клиент пишет, что покрытие отслоилось\"]}\n"
            "«Ужасный результат, хочу возврат или другого мастера» -> "
            "{\"queries\":[\"Клиент не остался доволен\",\"Клиент требует возврат денег\","
            "\"Клиент хочет переделку у другого мастера\"]}\n"
            "«Опаздываю на 15 минут, лучше перенести?» -> "
            "{\"queries\":[\"Клиент опаздывает\",\"Клиент хочет перенести запись\"]}\n"
            "«Сколько стоит снять чужой гель-лак и сколько это займёт?» -> "
            "{\"queries\":[\"Клиент спрашивает стоимость\",\"Клиент пришёл с покрытием из другой студии\","
            "\"Клиент спрашивает длительность процедуры\"]}\n"
            "«Маникюр понравился, запишите на конец месяца к тому же мастеру.» -> "
            "{\"queries\":[\"Клиент спрашивает ближайшее свободное время\","
            "\"Клиент хочет записаться к конкретному мастеру\"]}\n"
            "«Привет. На завтра отмените запись, пожалуйста. Дорого стало, подумаю, возвращаться ли.» -> "
            "{\"queries\":[\"Клиент хочет отменить запись заранее\"]}. "
            "Для этого примера запрещено выбирать перенос или возврат предоплаты: клиент не упоминал предоплату. "
            "Сценарий с предоплатой разрешён только когда в сообщении прямо есть слово «предоплата», «аванс» "
            "или равнозначное явное указание на уже внесённую сумму.";
}

[[nodiscard]] std::string build_texting_selector_retry_system_prompt() {
    return
            "Определи только деловую ситуацию сообщения клиента. Не пиши ответ клиенту. "
            "Верни строго JSON без Markdown: {\"queries\":[\"Клиент ...\"]}. "
            "Используй от одного до трёх коротких названий ситуации. "
            "Если клиент недоволен результатом услуги, первый запрос: \"Клиент не остался доволен\". "
            "Если названы возврат денег, другой мастер, запись, отмена, стоимость, длительность, опоздание "
            "или перенос, добавь только явно указанные ситуации. Ничего не додумывай.";
}

[[nodiscard]] std::string build_texting_organization_query_normalizer_prompt() {
    return
            "Сокращай любой вопрос от клиентов в сфере услуг до ~2-3 ключевых слов,"
            "описывая саму суть контекста.Своди всё к способам или услугам."
            "Примеры:1)\"каким способом я могу записаться на гель-лак?\"→способы записи;"
            "2)\"как я могу оплатить услугу в вашем салоне?\"→способы оплаты;"
            "3)\"Принимает ли у вас подолог?\"→услуга подолога;"
            "4)\"Парикмахерские услуги есть?\"→услуга парикмахера;"
            "5)\"Мне нужна женская стрижка на свадьбу.Подскажите,оказываете ли вы подобные услуги?\"→женская стрижка;"
            "6)\"Хотела бы привести свои бровки в порядок!\"→услуги брови;"
            "7)\"Проблемы с ногтями на ногах, поможете?\"→услуга подолога;"
            "8)\"Сколько примерно пешком от метро до вашего салона?\"→сколько идти от метро до салона."
            "Верни только сокращённую фразу без JSON, Markdown и пояснений.";
}

[[nodiscard]] std::size_t choose_texting_normalizer_input_chars(
        const engine_config_s &config,
        const std::string_view user_text) noexcept {
    const auto text_size = compact_whitespace(user_text).size();
    const auto windows = std::array{
            config.max_texting_normalizer_small_context_chars,
            config.max_texting_normalizer_medium_context_chars,
            config.max_texting_normalizer_large_context_chars,
    };

    for (const auto window : windows) {
        if (text_size <= window) {
            return window;
        }
    }

    return config.max_texting_normalizer_large_context_chars;
}

[[nodiscard]] std::optional<std::string> parse_texting_organization_normalized_query(
        const std::string_view response) {
    if (response.empty()) {
        return std::nullopt;
    }

    try {
        const auto value = nlohmann::json::parse(response, nullptr, true, true);

        if (value.is_string()) {
            auto query = compact_whitespace(value.get<std::string>());

            if (query.empty() || query.size() > 160) {
                return std::nullopt;
            }

            return query;
        }

        if (!value.is_object()) {
            return std::nullopt;
        }

        const auto it = value.find("query");

        if (it == value.end() || !it->is_string()) {
            return std::nullopt;
        }

        auto query = compact_whitespace(it->get<std::string>());

        if (query.empty() || query.size() > 240) {
            return std::nullopt;
        }

        return query;
    } catch (...) {
        auto query = compact_whitespace(response);

        if (query.empty() || query.size() > 160 || query.contains('{') || query.contains('}')) {
            return std::nullopt;
        }

        return query;
    }
}

[[nodiscard]] std::vector<std::string> parse_texting_selector_queries(
        const std::string_view response,
        const std::size_t limit) {
    if (response.empty() || limit == 0) {
        return {};
    }

    const auto parse_values = [&](const nlohmann::json &values) {
        auto result = std::vector<std::string>{};

        if (!values.is_array()) {
            return result;
        }

        auto seen = std::unordered_set<std::string>{};
        result.reserve(std::min(limit, values.size()));

        for (const auto &value : values) {
            if (!value.is_string()) {
                continue;
            }

            auto query = compact_whitespace(value.get<std::string>());

            if (query.empty() || query.size() > 240 || !seen.insert(query).second) {
                continue;
            }

            result.push_back(std::move(query));

            if (result.size() >= limit) {
                break;
            }
        }

        return result;
    };

    const auto object_begin = response.find('{');
    const auto object_end = response.rfind('}');

    if (object_begin != std::string_view::npos && object_end != std::string_view::npos &&
        object_end >= object_begin) {
        try {
            const auto root = nlohmann::json::parse(
                    std::string{response.substr(object_begin, object_end - object_begin + 1)});
            if (const auto queries_it = root.find("queries"); queries_it != root.end()) {
                auto result = parse_values(*queries_it);
                if (!result.empty()) {
                    return result;
                }
            }
        } catch (const nlohmann::json::exception &) {
            // A small model can finish the queries array but truncate the outer JSON object.
        }
    }

    const auto queries_key = response.find("\"queries\"");
    if (queries_key == std::string_view::npos) {
        return {};
    }

    const auto array_begin = response.find('[', queries_key);
    const auto array_end = array_begin == std::string_view::npos
                                   ? std::string_view::npos
                                   : response.find(']', array_begin + 1);

    if (array_begin == std::string_view::npos || array_end == std::string_view::npos) {
        return {};
    }

    try {
        return parse_values(nlohmann::json::parse(
                std::string{response.substr(array_begin, array_end - array_begin + 1)}));
    } catch (const nlohmann::json::exception &) {
        return {};
    }
}

struct texting_script_edit_s {
    std::string target = {};
    std::string replacement = {};
};

struct texting_script_edit_result_s {
    std::string content = {};
    std::size_t applied = 0;
    std::size_t rejected = 0;
};

struct texting_adaptation_result_s {
    std::vector<texting_script_edit_s> edits = {};
    texting_issue_analysis_s issue = {};
};

[[nodiscard]] texting_adaptation_result_s parse_texting_adaptation_result(
        const std::string_view response,
        const std::size_t limit) {
    auto result = texting_adaptation_result_s{};

    if (response.empty() || limit == 0) {
        return result;
    }

    const auto object_begin = response.find('{');
    const auto object_end = response.rfind('}');

    if (object_begin == std::string_view::npos || object_end == std::string_view::npos ||
        object_end < object_begin) {
        return result;
    }

    try {
        const auto root = nlohmann::json::parse(
                std::string{response.substr(object_begin, object_end - object_begin + 1)});

        if (const auto edits_it = root.find("edits");
            edits_it != root.end() && edits_it->is_array()) {
            auto seen_targets = std::unordered_set<std::string>{};
            result.edits.reserve(std::min(limit, edits_it->size()));

            for (const auto &value : *edits_it) {
                if (!value.is_object()) {
                    continue;
                }

                const auto target_it = value.find("target");
                const auto replacement_it = value.find("replacement");

                if (target_it == value.end() || replacement_it == value.end() ||
                    !target_it->is_string() || !replacement_it->is_string()) {
                    continue;
                }

                auto target = target_it->get<std::string>();
                auto replacement = replacement_it->get<std::string>();

                if (compact_whitespace(target).empty() || target.size() > 600 ||
                    replacement.size() > 900 || replacement.size() > target.size() + 320 ||
                    target == replacement || !seen_targets.insert(target).second) {
                    continue;
                }

                result.edits.push_back(texting_script_edit_s{
                        .target = std::move(target),
                        .replacement = std::move(replacement),
                });

                if (result.edits.size() >= limit) {
                    break;
                }
            }
        }

        const auto issue_it = root.find("issue");
        if (issue_it == root.end() || !issue_it->is_object()) {
            return result;
        }

        const auto specified_it = issue_it->find("specified");
        if (specified_it == issue_it->end() || !specified_it->is_boolean() ||
            !specified_it->get<bool>()) {
            return result;
        }

        const auto reason_it = issue_it->find("reason_clause");
        if (reason_it == issue_it->end() || !reason_it->is_string()) {
            return result;
        }

        auto reason = compact_whitespace(reason_it->get<std::string>());
        if (reason.starts_with("что ")) {
            reason.erase(0, std::string_view{"что "}.size());
        }
        while (!reason.empty() &&
               (reason.back() == '.' || reason.back() == '!' || reason.back() == '?')) {
            reason.pop_back();
        }
        util::trim(reason);

        if (reason.empty() || reason.size() > 320) {
            return result;
        }

        result.issue.specified = true;
        result.issue.summary = reason;
        result.issue.reaction = std::format("Нам очень жаль, что {}.", reason);

        const auto photo_it = issue_it->find("photo");
        if (photo_it != issue_it->end() && photo_it->is_string()) {
            auto photo = compact_whitespace(photo_it->get<std::string>());
            if (photo.size() <= 180 && photo.starts_with("фотографию ")) {
                result.issue.photo = std::move(photo);
            }
        }

        return result;
    } catch (const nlohmann::json::exception &) {
        return {};
    }
}

[[nodiscard]] bool texting_edit_targets_category(
        const texting_script_edit_s &edit,
        const std::string_view fragment) noexcept {
    return edit.target.contains(fragment);
}

void enforce_texting_issue_analysis_edits(
        const std::string_view primary_script,
        std::vector<texting_script_edit_s> &edits,
        const std::size_t limit,
        const texting_issue_analysis_s &issue) {
    if (!issue.specified || limit == 0) {
        return;
    }

    constexpr auto generic_reaction =
            std::string_view{"Нам очень жаль, что результат услуги не оправдал Ваших ожиданий."};
    constexpr auto generic_question =
            std::string_view{"Подскажите, пожалуйста, что именно Вас не устроило?"};
    constexpr auto generic_photo = std::string_view{"фотографию результата"};

    std::erase_if(edits, [](const texting_script_edit_s &edit) {
        return texting_edit_targets_category(edit, "Нам очень жаль, что результат услуги") ||
               texting_edit_targets_category(edit, "что именно Вас не устроило") ||
               texting_edit_targets_category(edit, "фотографию результата");
    });

    auto required = std::vector<texting_script_edit_s>{};
    required.reserve(3);

    if (!issue.reaction.empty() && primary_script.contains(generic_reaction)) {
        required.push_back(texting_script_edit_s{
                .target = std::string{generic_reaction},
                .replacement = issue.reaction,
        });
    }

    if (primary_script.contains(generic_question)) {
        required.push_back(texting_script_edit_s{
                .target = std::string{generic_question},
                .replacement = {},
        });
    }

    if (primary_script.contains(generic_photo)) {
        required.push_back(texting_script_edit_s{
                .target = std::string{generic_photo},
                .replacement = issue.photo.empty()
                                       ? "фотографию текущего состояния"
                                       : issue.photo,
        });
    }

    auto merged = std::vector<texting_script_edit_s>{};
    merged.reserve(limit);

    for (auto &edit : required) {
        if (merged.size() >= limit) {
            break;
        }

        merged.push_back(std::move(edit));
    }

    for (auto &edit : edits) {
        if (merged.size() >= limit) {
            break;
        }

        const auto duplicate = std::ranges::any_of(merged, [&](const texting_script_edit_s &existing) {
            return existing.target == edit.target;
        });

        if (!duplicate) {
            merged.push_back(std::move(edit));
        }
    }

    edits = std::move(merged);
}

[[nodiscard]] bool is_horizontal_space(const char value) noexcept {
    return value == ' ' || value == '\t';
}

[[nodiscard]] bool is_sentence_terminator(const char value) noexcept {
    return value == '.' || value == '!' || value == '?';
}

[[nodiscard]] std::pair<std::size_t, std::size_t> expand_empty_texting_edit_range(
        const std::string_view script,
        const std::size_t position,
        const std::size_t length) noexcept {
    auto begin = position;
    auto end = position + length;

    auto target_end = end;
    while (target_end > begin && is_horizontal_space(script[target_end - 1])) {
        --target_end;
    }

    if (target_end > begin && is_sentence_terminator(script[target_end - 1])) {
        auto sentence_begin = begin;

        while (sentence_begin > 0) {
            const auto previous = script[sentence_begin - 1];
            if (previous == '\n' || previous == '\r' || is_sentence_terminator(previous)) {
                break;
            }

            --sentence_begin;
        }

        auto prefix_begin = sentence_begin;
        while (prefix_begin < begin && is_horizontal_space(script[prefix_begin])) {
            ++prefix_begin;
        }

        auto prefix_end = begin;
        while (prefix_end > prefix_begin && is_horizontal_space(script[prefix_end - 1])) {
            --prefix_end;
        }

        const auto prefix = script.substr(prefix_begin, prefix_end - prefix_begin);
        if (!prefix.empty() && prefix.size() <= 96) {
            const auto last = prefix.back();
            if (last == ',' || last == ':' || last == ';') {
                begin = sentence_begin;
            }
        }
    }

    while (end < script.size() && is_horizontal_space(script[end])) {
        ++end;
    }

    if ((begin == 0 || script[begin - 1] == '\n') && end < script.size() && script[end] == '\n') {
        ++end;
    }

    return {begin, end - begin};
}

[[nodiscard]] texting_script_edit_result_s apply_texting_script_edits(
        const std::string_view script,
        const std::span<const texting_script_edit_s> edits) {
    struct resolved_edit_s {
        std::size_t position = 0;
        std::size_t length = 0;
        std::string replacement = {};
    };

    auto resolved = std::vector<resolved_edit_s>{};
    resolved.reserve(edits.size());

    auto rejected = std::size_t{0};

    for (const auto &edit : edits) {
        const auto position = script.find(edit.target);

        if (position == std::string_view::npos ||
            script.find(edit.target, position + edit.target.size()) != std::string_view::npos) {
            ++rejected;
            continue;
        }

        auto resolved_position = position;
        auto resolved_length = edit.target.size();

        if (edit.replacement.empty()) {
            const auto expanded =
                    expand_empty_texting_edit_range(script, resolved_position, resolved_length);
            resolved_position = expanded.first;
            resolved_length = expanded.second;
        }

        const auto end = resolved_position + resolved_length;
        const auto overlaps = std::ranges::any_of(resolved, [&](const resolved_edit_s &other) {
            const auto other_end = other.position + other.length;
            return resolved_position < other_end && other.position < end;
        });

        if (overlaps) {
            ++rejected;
            continue;
        }

        resolved.push_back(resolved_edit_s{
                .position = resolved_position,
                .length = resolved_length,
                .replacement = edit.replacement,
        });
    }

    std::ranges::sort(resolved, std::greater<>{}, &resolved_edit_s::position);

    auto content = std::string{script};

    for (const auto &edit : resolved) {
        content.replace(edit.position, edit.length, edit.replacement);
    }

    return texting_script_edit_result_s{
            .content = std::move(content),
            .applied = resolved.size(),
            .rejected = rejected,
    };
}

[[nodiscard]] double texting_generation_temperature(
        const engine_config_s &config,
        const texting_style_e style) noexcept {
    switch (style) {
        case texting_style_e::formal: return config.texting_formal_temperature;
        case texting_style_e::neutral: return config.texting_neutral_temperature;
        case texting_style_e::friendly: return config.texting_friendly_temperature;
        case texting_style_e::any: break;
    }

    return config.texting_neutral_temperature;
}

[[nodiscard]] std::string make_answer_excerpt(const std::string_view text, const std::size_t maximum_bytes) {
    auto compact = compact_whitespace(remove_generated_service_lines(text));

    if (compact.size() <= maximum_bytes) {
        return compact;
    }

    if (maximum_bytes < 16) {
        return truncate_utf8(compact, maximum_bytes);
    }

    constexpr auto marker = std::string_view{" ... "};
    const auto available = maximum_bytes - marker.size();
    const auto head_budget = available * 2 / 3;
    const auto tail_budget = available - head_budget;

    const auto head_size = valid_utf8_prefix_size(compact, head_budget);
    const auto tail_offset = valid_utf8_suffix_offset(compact, compact.size() - tail_budget);

    return std::format("{}{}{}", compact.substr(0, head_size), marker, compact.substr(tail_offset));
}

[[nodiscard]] bool is_markdown_heading(const std::string_view line) {
    auto normalized = std::string{line};
    util::trim(normalized);

    return normalized.starts_with("#");
}

[[nodiscard]] std::string markdown_heading_text(const std::string_view line) {
    auto text = std::string{line};
    util::trim(text);

    while (!text.empty() && text.front() == '#') {
        text.erase(text.begin());
    }

    util::trim(text);

    return text;
}

[[nodiscard]] bool is_target_answer_heading(const std::string_view line) {
    auto text = markdown_heading_text(line);
    util::trim(text);

    return text == "Пошаговая инструкция что делать";
}

[[nodiscard]] std::string extract_direct_answer_section(const std::string_view markdown) {
    auto inside_section = false;
    auto result = std::string{};
    auto position = std::size_t{};

    while (position <= markdown.size()) {
        const auto line_end = markdown.find('\n', position);

        const auto line = markdown.substr(
                position,
                line_end == std::string_view::npos ? markdown.size() - position : line_end - position);

        if (!inside_section) {
            if (is_markdown_heading(line) && is_target_answer_heading(line)) {
                inside_section = true;
            }
        } else {
            if (is_markdown_heading(line)) {
                break;
            }

            if (!result.empty()) {
                result.push_back('\n');
            }

            result += line;
        }

        if (line_end == std::string_view::npos) {
            break;
        }

        position = line_end + 1;
    }

    util::trim(result);

    if (!result.empty()) {
        return result;
    }

    auto fallback = std::string{markdown};
    util::trim(fallback);

    return fallback;
}

[[nodiscard]] bool is_completed_assistant_entry(const chat_history_entry_s &entry) noexcept {
    return entry.assistant.has_value() && entry.status == chat_message_status_e::completed;
}

void append_unique_source_files(std::vector<std::string> &target, const std::span<const std::string> source) {
    auto seen = std::unordered_set<std::string>{};
    seen.reserve(target.size() + source.size());

    for (const auto &filename : target) {
        seen.insert(filename);
    }

    for (const auto &filename : source) {
        if (seen.insert(filename).second) {
            target.push_back(filename);
        }
    }
}


void limit_source_files(std::vector<std::string> &source_files, const std::size_t limit) {
    assert(limit > 0);

    if (source_files.size() > limit) {
        source_files.resize(limit);
    }
}

constexpr auto max_topic_anchor_ids = std::size_t{1};

enum class chat_relation_kind_e {
    standalone,
    follow_up,
    transform_previous_answer,
};

enum class previous_answer_transform_kind_e {
    none,
    concise,
    expand,
    simplify,
    restructure,
    rewrite,
    formal,
    strict,
    friendly,
    more_emoji,
    fewer_emoji,
};

[[nodiscard]] std::string_view relation_kind_name(const chat_relation_kind_e relation) noexcept {
    switch (relation) {
        case chat_relation_kind_e::standalone: return "standalone";
        case chat_relation_kind_e::follow_up: return "follow_up";
        case chat_relation_kind_e::transform_previous_answer: return "transform_previous_answer";
    }

    return "standalone";
}

[[nodiscard]] std::string_view transform_kind_name(const previous_answer_transform_kind_e transform) noexcept {
    switch (transform) {
        case previous_answer_transform_kind_e::none: return "none";
        case previous_answer_transform_kind_e::concise: return "concise";
        case previous_answer_transform_kind_e::expand: return "expand";
        case previous_answer_transform_kind_e::simplify: return "simplify";
        case previous_answer_transform_kind_e::restructure: return "restructure";
        case previous_answer_transform_kind_e::rewrite: return "rewrite";
        case previous_answer_transform_kind_e::formal: return "formal";
        case previous_answer_transform_kind_e::strict: return "strict";
        case previous_answer_transform_kind_e::friendly: return "friendly";
        case previous_answer_transform_kind_e::more_emoji: return "more_emoji";
        case previous_answer_transform_kind_e::fewer_emoji: return "fewer_emoji";
    }

    return "none";
}

void append_lowercase_utf8_codepoint(const char32_t codepoint, std::string &result) {
    auto lowered = codepoint;

    if (lowered >= U'A' && lowered <= U'Z') {
        lowered += 32;
    } else if (lowered >= U'А' && lowered <= U'Я') {
        lowered += 32;
    } else if (lowered == U'Ё') {
        lowered = U'ё';
    }

    if (lowered <= 0x7F) {
        result.push_back(static_cast<char>(lowered));
        return;
    }

    if (lowered <= 0x7FF) {
        result.push_back(static_cast<char>(0xC0 | ((lowered >> 6) & 0x1F)));
        result.push_back(static_cast<char>(0x80 | (lowered & 0x3F)));
        return;
    }

    if (lowered <= 0xFFFF) {
        result.push_back(static_cast<char>(0xE0 | ((lowered >> 12) & 0x0F)));
        result.push_back(static_cast<char>(0x80 | ((lowered >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (lowered & 0x3F)));
        return;
    }

    result.push_back(static_cast<char>(0xF0 | ((lowered >> 18) & 0x07)));
    result.push_back(static_cast<char>(0x80 | ((lowered >> 12) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | ((lowered >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (lowered & 0x3F)));
}

[[nodiscard]] std::string lowercase_utf8(const std::string_view text) {
    auto result = std::string{};
    result.reserve(text.size());

    auto index = std::size_t{};

    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);

        if (first < 0x80) {
            result.push_back(static_cast<char>(std::tolower(first)));
            ++index;
            continue;
        }

        auto codepoint = char32_t{};
        auto length = std::size_t{};

        if ((first & 0xE0) == 0xC0 && index + 1 < text.size()) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            codepoint = static_cast<char32_t>(((first & 0x1F) << 6) | (second & 0x3F));
            length = 2;
        } else if ((first & 0xF0) == 0xE0 && index + 2 < text.size()) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            codepoint = static_cast<char32_t>(((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F));
            length = 3;
        } else if ((first & 0xF8) == 0xF0 && index + 3 < text.size()) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            const auto fourth = static_cast<unsigned char>(text[index + 3]);
            codepoint = static_cast<char32_t>(((first & 0x07) << 18) | ((second & 0x3F) << 12) | ((third & 0x3F) << 6) |
                                              (fourth & 0x3F));
            length = 4;
        } else {
            result.push_back(static_cast<char>(first));
            ++index;
            continue;
        }

        append_lowercase_utf8_codepoint(codepoint, result);
        index += length;
    }

    return result;
}

[[nodiscard]] std::string normalize_user_query_for_relation(const std::string_view text) {
    auto normalized = lowercase_utf8(text);
    util::trim(normalized);

    auto result = std::string{};
    result.reserve(normalized.size());

    auto previous_space = false;

    for (const auto ch : normalized) {
        const auto byte = static_cast<unsigned char>(ch);

        if (byte < 128 && std::isspace(byte) != 0) {
            if (!previous_space) {
                result.push_back(' ');
                previous_space = true;
            }

            continue;
        }

        result.push_back(ch);
        previous_space = false;
    }

    util::trim(result);

    return result;
}

[[nodiscard]] bool starts_with_any(const std::string_view text,
                                   const std::span<const std::string_view> prefixes) noexcept {
    return std::ranges::any_of(prefixes, [text](const std::string_view prefix) { return text.starts_with(prefix); });
}

[[nodiscard]] bool looks_like_procedural_request(const std::string_view user_text) {
    const auto normalized_text = normalize_user_query_for_relation(user_text);

    constexpr auto procedural_prefixes = std::array{
            std::string_view{"как "},
            std::string_view{"а как "},
            std::string_view{"каким образом "},
            std::string_view{"а каким образом "},
    };

    return starts_with_any(normalized_text, std::span{procedural_prefixes});
}

[[nodiscard]] bool contains_any(const std::string_view text,
                                const std::span<const std::string_view> fragments) noexcept {
    return std::ranges::any_of(fragments, [text](const std::string_view fragment) { return text.contains(fragment); });
}

[[nodiscard]] std::string normalize_texting_phrase_match(const std::string_view text) {
    const auto lowered = lowercase_utf8(text);

    auto result = std::string{};
    result.reserve(lowered.size());

    auto previous_space = true;

    for (const auto ch : lowered) {
        const auto byte = static_cast<unsigned char>(ch);

        if (byte < 128 && std::isalnum(byte) == 0) {
            if (!previous_space) {
                result.push_back(' ');
                previous_space = true;
            }

            continue;
        }

        result.push_back(ch);
        previous_space = false;
    }

    util::trim(result);

    return result;
}

[[nodiscard]] bool contains_texting_phrase(const std::string_view normalized_text,
                                           const std::string_view phrase) {
    if (normalized_text.empty() || phrase.empty()) {
        return false;
    }

    const auto padded_text = std::format(" {} ", normalized_text);
    const auto padded_phrase = std::format(" {} ", phrase);

    return padded_text.contains(padded_phrase);
}

[[nodiscard]] bool has_texting_greeting(const std::string_view text) {
    const auto normalized = normalize_texting_phrase_match(truncate_utf8(text, 320));

    constexpr auto greetings = std::array{
            std::string_view{"доброе утро"},
            std::string_view{"доброе утрое"},
            std::string_view{"добрый день"},
            std::string_view{"добрый вечер"},
            std::string_view{"доброго времени"},
            std::string_view{"здравствуйте"},
            std::string_view{"здравствуй"},
            std::string_view{"здарова"},
            std::string_view{"дарова"},
            std::string_view{"привет"},
            std::string_view{"приветик"},
            std::string_view{"приветули"},
            std::string_view{"хай"},
            std::string_view{"hi"},
            std::string_view{"хэллоу"},
            std::string_view{"хелло"},
            std::string_view{"халло"},
            std::string_view{"ку"},
            std::string_view{"q"},
            std::string_view{"кулити"},
            std::string_view{"кулиалити"},
            std::string_view{"здравия желаю"},
            std::string_view{"физкульт привет"},
            std::string_view{"салам"},
            std::string_view{"салам малейкум"},
            std::string_view{"салем"},
            std::string_view{"ни хао"},
            std::string_view{"хех здарова"},
            std::string_view{"хэх здарова"},
            std::string_view{"здравули"},
            std::string_view{"здравити"},
            std::string_view{"привіт"},
            std::string_view{"привит"},
            std::string_view{"тевирп"},
            std::string_view{"бонжур"},
            std::string_view{"йоу"},
            std::string_view{"вітаю"},
            std::string_view{"витаю"},
    };

    return std::ranges::any_of(greetings, [&](const std::string_view greeting) {
        return contains_texting_phrase(normalized, greeting);
    });
}

[[nodiscard]] bool answer_starts_with_texting_greeting(const std::string_view answer) {
    const auto normalized = normalize_texting_phrase_match(truncate_utf8(answer, 180));

    constexpr auto greetings = std::array{
            std::string_view{"доброе утро"},
            std::string_view{"добрый день"},
            std::string_view{"добрый вечер"},
            std::string_view{"здравствуйте"},
            std::string_view{"здравствуй"},
            std::string_view{"привет"},
    };

    return std::ranges::any_of(greetings, [&](const std::string_view greeting) {
        return contains_texting_phrase(normalized, greeting);
    });
}

[[nodiscard]] std::string texting_greeting_line(const texting_style_e style) {
    if (style == texting_style_e::friendly) {
        return "Здравствуйте, <имя>! 🌷";
    }

    return "Здравствуйте, <имя>!";
}

void ensure_texting_reply_greeting(std::string &answer,
                                   const std::string_view user_text,
                                   const texting_style_e style) {
    if (answer.empty() || !has_texting_greeting(user_text) ||
        answer_starts_with_texting_greeting(answer)) {
        return;
    }

    util::trim(answer);
    answer = std::format("{}\n{}", texting_greeting_line(style), answer);
}

[[nodiscard]] bool looks_like_customer_reply_request(const std::string_view user_text) {
    const auto normalized = normalize_user_query_for_relation(user_text);

    return normalized.contains("как ответить") ||
           normalized.contains("что ответить") ||
           normalized.contains("ответить клиент") ||
           normalized.contains("написать клиент") ||
           normalized.contains("сообщить клиент");
}

void apply_organization_texting_style_tokens(std::string &answer,
                                              const texting_style_e style) {
    constexpr auto prefix = std::string_view{"[[friendly:"};
    constexpr auto suffix = std::string_view{"]]"};

    auto search_from = std::size_t{0};

    while (true) {
        const auto begin = answer.find(prefix, search_from);

        if (begin == std::string::npos) {
            break;
        }

        const auto value_begin = begin + prefix.size();
        const auto end = answer.find(suffix, value_begin);

        if (end == std::string::npos) {
            break;
        }

        const auto replacement = style == texting_style_e::friendly
                                         ? answer.substr(value_begin, end - value_begin)
                                         : std::string{};
        answer.replace(begin, end + suffix.size() - begin, replacement);
        search_from = begin + replacement.size();
    }
}

void append_organization_texting_emoji(std::string &answer,
                                       const std::string_view emoji,
                                       const texting_style_e style) {
    if (style != texting_style_e::friendly || emoji.empty() || answer.empty()) {
        return;
    }

    util::trim(answer);

    if (answer.ends_with(emoji) || answer.contains(std::format("{}\n", emoji)) ||
        answer.contains(std::format("{} ", emoji))) {
        return;
    }

    const auto paragraph_end = answer.find("\n\n");

    if (paragraph_end == std::string::npos) {
        answer += std::format(" {}", emoji);
        return;
    }

    answer.insert(paragraph_end, std::format(" {}", emoji));
}

[[nodiscard]] bool has_texting_prepayment_context(const std::string_view normalized_user_text) {
    constexpr auto fragments = std::array{
            std::string_view{"предоплат"},
            std::string_view{"аванс"},
            std::string_view{"задат"},
    };

    return contains_any(normalized_user_text, std::span{fragments});
}

[[nodiscard]] bool texting_knowledge_is_compatible(
        const retrieved_knowledge_s &knowledge,
        const std::string_view normalized_user_text) {
    const auto normalized_scenario = normalize_user_query_for_relation(
            knowledge.tag_name.empty() ? knowledge.title : knowledge.tag_name);

    if (normalized_scenario.contains("предоплат") &&
        !has_texting_prepayment_context(normalized_user_text)) {
        return false;
    }

    return true;
}

[[nodiscard]] bool looks_like_plain_texting_cancellation(
        const std::string_view normalized_user_text) {
    if (has_texting_prepayment_context(normalized_user_text)) {
        return false;
    }

    constexpr auto negations = std::array{
            std::string_view{"не отменя"},
            std::string_view{"не надо отмен"},
            std::string_view{"не нужно отмен"},
    };

    if (contains_any(normalized_user_text, std::span{negations})) {
        return false;
    }

    constexpr auto fragments = std::array{
            std::string_view{"отмените запись"},
            std::string_view{"отменить запись"},
            std::string_view{"отменяю запись"},
            std::string_view{"запись отмените"},
    };

    return contains_any(normalized_user_text, std::span{fragments});
}

[[nodiscard]] bool has_any_retrieved_knowledge(const std::span<const retrieved_knowledge_s> knowledge) noexcept {
    return std::ranges::any_of(knowledge, [](const retrieved_knowledge_s &item) noexcept {
        return item.match != knowledge_match_e::none;
    });
}

[[nodiscard]] bool is_glossary_match(const knowledge_match_e match) noexcept {
    return match == knowledge_match_e::exact_glossary_heading ||
           match == knowledge_match_e::unordered_glossary_heading ||
           match == knowledge_match_e::unordered_fuzzy_glossary_heading;
}

[[nodiscard]] bool is_direct_knowledge_answer_candidate(
        const std::span<const retrieved_knowledge_s> knowledge) noexcept {
    if (knowledge.size() != 1) {
        return false;
    }

    const auto match = knowledge.front().match;

    return match == knowledge_match_e::exact_frequent_query || match == knowledge_match_e::unordered_frequent_query ||
           match == knowledge_match_e::unordered_fuzzy_frequent_query || is_glossary_match(match);
}

[[nodiscard]] bool is_query_matched_document_tag_candidate(
        const std::span<const retrieved_knowledge_s> knowledge) noexcept {
    if (knowledge.size() != 1) {
        return false;
    }

    const auto &item = knowledge.front();

    return item.tag_matched_query && !item.tag_name.empty() && !item.direct_content.empty() &&
           !is_glossary_match(item.match);
}

[[nodiscard]] bool looks_like_instruction_analysis_request(const std::string_view user_text) {
    const auto normalized_text = normalize_user_query_for_relation(user_text);

    if (normalized_text.empty()) {
        return false;
    }

    /*
     * These requests may still need the same knowledge file as a direct
     * instruction request, but the user is not asking to paste the instruction.
     *
     * Example:
     *   "как удалить запись"                                  -> direct instruction is fine
     *   "какие проблемы могут возникнуть при удалении записи" -> use a matching H2 section;
     *                                                               otherwise give the file to LLM
     *
     * So this predicate blocks only the old whole-instruction direct path.
     * An H2 section explicitly selected by the current query is handled first.
     */
    constexpr auto analysis_prefixes = std::array{
            std::string_view{"какие проблемы"},
            std::string_view{"какая проблема"},
            std::string_view{"какие могут быть проблемы"},
            std::string_view{"какие могут возникнуть проблемы"},
            std::string_view{"какие риски"},
            std::string_view{"какой риск"},
            std::string_view{"какие нюансы"},
            std::string_view{"какие ограничения"},
            std::string_view{"какие исключения"},
            std::string_view{"какие последствия"},
            std::string_view{"что может пойти не так"},
            std::string_view{"что может случиться"},
            std::string_view{"что может произойти"},
            std::string_view{"что будет если"},
            std::string_view{"что будет при"},
            std::string_view{"что произойдет если"},
            std::string_view{"что произойдёт если"},
            std::string_view{"что учитывать"},
            std::string_view{"что нужно учитывать"},
            std::string_view{"что важно учитывать"},
            std::string_view{"что проверить перед"},
            std::string_view{"что проверить при"},
            std::string_view{"на что обратить внимание"},
            std::string_view{"на что нужно обратить внимание"},
            std::string_view{"на что важно обратить внимание"},
            std::string_view{"можно ли"},
            std::string_view{"нужно ли"},
            std::string_view{"надо ли"},
            std::string_view{"обязательно ли"},
            std::string_view{"нельзя ли"},
            std::string_view{"почему"},
            std::string_view{"зачем"},
            std::string_view{"когда можно"},
            std::string_view{"когда нельзя"},
            std::string_view{"в каких случаях"},
            std::string_view{"чем отличается"},
            std::string_view{"в чем разница"},
            std::string_view{"в чём разница"},
    };

    if (starts_with_any(normalized_text, std::span{analysis_prefixes})) {
        return true;
    }

    constexpr auto analysis_fragments = std::array{
            std::string_view{" какие проблемы "},
            std::string_view{" проблемы могут "},
            std::string_view{" может возникнуть "},
            std::string_view{" могут возникнуть "},
            std::string_view{" может быть проблема "},
            std::string_view{" могут быть проблемы "},
            std::string_view{" что может пойти не так "},
            std::string_view{" пойти не так "},
            std::string_view{" какие риски "},
            std::string_view{" риски "},
            std::string_view{" риск "},
            std::string_view{" последствия "},
            std::string_view{" нюансы "},
            std::string_view{" ограничения "},
            std::string_view{" исключения "},
            std::string_view{" подводные камни "},
            std::string_view{" обратить внимание "},
            std::string_view{" что учитывать "},
            std::string_view{" что проверить перед "},
            std::string_view{" что проверить при "},
    };

    return contains_any(std::format(" {} ", normalized_text), std::span{analysis_fragments});
}

[[nodiscard]] bool has_glossary_knowledge(const std::span<const retrieved_knowledge_s> knowledge) noexcept {
    return std::ranges::any_of(knowledge, [](const retrieved_knowledge_s &item) noexcept {
        return is_glossary_match(item.match);
    });
}

[[nodiscard]] bool should_answer_without_llm(const std::string_view user_text,
                                             const std::span<const retrieved_knowledge_s> knowledge) {
    /*
     * An H2 section explicitly selected by the current query is already the
     * complete source answer. This applies both to a fresh request and to a
     * follow-up resolved against the active source file.
     */
    if (is_query_matched_document_tag_candidate(knowledge)) {
        return true;
    }

    if (!is_direct_knowledge_answer_candidate(knowledge)) {
        return false;
    }

    if (has_glossary_knowledge(knowledge)) {
        return !looks_like_procedural_request(user_text);
    }

    return !looks_like_instruction_analysis_request(user_text);
}

[[nodiscard]] bool knowledge_contains_any_source_filename(const std::span<const retrieved_knowledge_s> knowledge,
                                                          const std::span<const std::string> filenames) noexcept {
    if (knowledge.empty() || filenames.empty()) {
        return false;
    }

    return std::ranges::any_of(knowledge, [filenames](const retrieved_knowledge_s &item) noexcept {
        return std::ranges::find(filenames, item.filename) != filenames.end();
    });
}

void append_unique_knowledge(std::vector<retrieved_knowledge_s> &target,
                             const std::span<const retrieved_knowledge_s> source,
                             const std::size_t limit = 0) {
    auto seen = std::unordered_set<std::string>{};
    seen.reserve(target.size() + source.size());

    for (const auto &item : target) {
        seen.insert(item.filename);
    }

    for (const auto &item : source) {
        if (limit != 0 && target.size() >= limit) {
            break;
        }

        if (seen.insert(item.filename).second) {
            target.push_back(item);
        }
    }
}

[[nodiscard]] std::vector<retrieved_knowledge_s> merge_contextual_knowledge(
        const std::span<const retrieved_knowledge_s> first,
        const std::span<const retrieved_knowledge_s> second,
        const std::span<const retrieved_knowledge_s> third,
        const std::size_t limit) {
    auto result = std::vector<retrieved_knowledge_s>{};
    result.reserve(first.size() + second.size() + third.size());

    append_unique_knowledge(result, first, limit);
    append_unique_knowledge(result, second, limit);
    append_unique_knowledge(result, third, limit);

    return result;
}

[[nodiscard]] previous_answer_transform_kind_e classify_previous_answer_transform_request(
        const std::string_view normalized_text) noexcept {
    constexpr auto expand_exact = std::array{
            std::string_view{"подробнее"},
            std::string_view{"подробно"},
            std::string_view{"конкретнее"},
            std::string_view{"конкретней"},
            std::string_view{"распиши"},
            std::string_view{"развернуто"},
            std::string_view{"развёрнуто"},
    };

    constexpr auto expand_fragments = std::array{
            std::string_view{"распиши подробнее"},
            std::string_view{"объясни подробнее"},
            std::string_view{"расскажи подробнее"},
            std::string_view{"сделай подробнее"},
            std::string_view{"составь подробнее"},
            std::string_view{"ответь подробнее"},
            std::string_view{"добавь подробности"},
            std::string_view{"добавь деталей"},
            std::string_view{"более подробно"},
            std::string_view{"больше деталей"},
            std::string_view{"пошагово"},
            std::string_view{"по шагам"},
            std::string_view{"конкретнее"},
            std::string_view{"уточни действия"},
    };

    /*
     * Explicit requests for more detail take precedence over phrases such as
     * "только основные действия". In that combination the user asks for more
     * depth inside the core steps, not for a shorter answer.
     */
    if (std::ranges::contains(expand_exact, normalized_text) ||
        contains_any(normalized_text, std::span{expand_fragments})) {
        return previous_answer_transform_kind_e::expand;
    }

    constexpr auto concise_exact = std::array{
            std::string_view{"кратко"},
            std::string_view{"кратче"},
            std::string_view{"коротко"},
            std::string_view{"покороче"},
            std::string_view{"короче"},
            std::string_view{"сократи"},
            std::string_view{"одним предложением"},
            std::string_view{"в двух словах"},
    };

    constexpr auto concise_fragments = std::array{
            std::string_view{"сделай кратко"},
            std::string_view{"сделай кратче"},
            std::string_view{"сделай коротко"},
            std::string_view{"сделай короче"},
            std::string_view{"составь это кратко"},
            std::string_view{"сократи ответ"},
            std::string_view{"сократи это"},
            std::string_view{"только основные действия"},
            std::string_view{"только основные шаги"},
            std::string_view{"без подробностей"},
            std::string_view{"без деталей"},
            std::string_view{"одним предложением"},
            std::string_view{"в двух словах"},
    };

    if (std::ranges::contains(concise_exact, normalized_text) ||
        contains_any(normalized_text, std::span{concise_fragments})) {
        return previous_answer_transform_kind_e::concise;
    }

    constexpr auto simplify_fragments = std::array{
            std::string_view{"объясни проще"},
            std::string_view{"расскажи проще"},
            std::string_view{"ответь проще"},
            std::string_view{"напиши проще"},
            std::string_view{"простыми словами"},
    };

    if (contains_any(normalized_text, std::span{simplify_fragments})) {
        return previous_answer_transform_kind_e::simplify;
    }

    constexpr auto restructure_fragments = std::array{
            std::string_view{"сделай списком"},
            std::string_view{"составь список"},
            std::string_view{"чеклист"},
            std::string_view{"сделай чеклист"},
            std::string_view{"составь чеклист"},
            std::string_view{"составь определение"},
            std::string_view{"сформулируй определение"},
    };

    if (contains_any(normalized_text, std::span{restructure_fragments})) {
        return previous_answer_transform_kind_e::restructure;
    }

    constexpr auto fewer_emoji_fragments = std::array{
            std::string_view{"меньше эмодзи"},
            std::string_view{"убери эмодзи"},
            std::string_view{"без эмодзи"},
            std::string_view{"меньше смайликов"},
            std::string_view{"поменьше смайликов"},
            std::string_view{"убери смайлики"},
            std::string_view{"без смайликов"},
    };

    if (contains_any(normalized_text, std::span{fewer_emoji_fragments})) {
        return previous_answer_transform_kind_e::fewer_emoji;
    }

    constexpr auto more_emoji_fragments = std::array{
            std::string_view{"больше эмодзи"},
            std::string_view{"побольше эмодзи"},
            std::string_view{"не хватает эмодзи"},
            std::string_view{"добавь эмодзи"},
            std::string_view{"больше смайликов"},
            std::string_view{"побольше смайликов"},
            std::string_view{"не хватает смайликов"},
            std::string_view{"добавь смайлики"},
    };

    if (contains_any(normalized_text, std::span{more_emoji_fragments})) {
        return previous_answer_transform_kind_e::more_emoji;
    }

    constexpr auto friendly_fragments = std::array{
            std::string_view{"дружелюбнее"},
            std::string_view{"более дружелюб"},
            std::string_view{"теплее"},
            std::string_view{"мягче"},
            std::string_view{"по-доброму"},
    };

    if (contains_any(normalized_text, std::span{friendly_fragments})) {
        return previous_answer_transform_kind_e::friendly;
    }

    constexpr auto strict_fragments = std::array{
            std::string_view{"строже"},
            std::string_view{"более строго"},
            std::string_view{"пожестче"},
            std::string_view{"пожёстче"},
    };

    if (contains_any(normalized_text, std::span{strict_fragments})) {
        return previous_answer_transform_kind_e::strict;
    }

    constexpr auto formal_fragments = std::array{
            std::string_view{"формальнее"},
            std::string_view{"более формально"},
            std::string_view{"официальнее"},
            std::string_view{"более официально"},
            std::string_view{"суше"},
    };

    if (contains_any(normalized_text, std::span{formal_fragments})) {
        return previous_answer_transform_kind_e::formal;
    }

    constexpr auto rewrite_fragments = std::array{
            std::string_view{"переформулируй"},
            std::string_view{"перефразируй"},
            std::string_view{"перефраз"},
            std::string_view{"перефариз"},
            std::string_view{"перепиши"},
            std::string_view{"добавь"},
            std::string_view{"убери"},
            std::string_view{"удали"},
            std::string_view{"замени"},
            std::string_view{"не пиши"},
            std::string_view{"сделай"},
            std::string_view{"напиши"},
            std::string_view{"укажи"},
    };

    if (contains_any(normalized_text, std::span{rewrite_fragments})) {
        return previous_answer_transform_kind_e::rewrite;
    }

    return previous_answer_transform_kind_e::none;
}

[[nodiscard]] bool looks_like_errors_or_risks_request(const std::string_view user_text) {
    const auto normalized_text = normalize_user_query_for_relation(user_text);

    constexpr auto fragments = std::array{
            std::string_view{"какие ошибки"},
            std::string_view{"какая ошибка"},
            std::string_view{"ошибки здесь"},
            std::string_view{"ошибки чаще"},
            std::string_view{"часто допускают"},
            std::string_view{"чаще всего допускают"},
            std::string_view{"что может пойти не так"},
            std::string_view{"какие риски"},
            std::string_view{"какой риск"},
            std::string_view{"подводные камни"},
    };

    return contains_any(normalized_text, std::span{fragments});
}

[[nodiscard]] bool looks_like_explicit_follow_up(const std::string_view normalized_text) noexcept {
    constexpr auto follow_up_prefixes = std::array{
            std::string_view{"а если"},
            std::string_view{"а что если"},
            std::string_view{"а как"},
            std::string_view{"а надо"},
            std::string_view{"а можно"},
            std::string_view{"а нужно"},
            std::string_view{"тогда"},
            std::string_view{"тогда "},
            std::string_view{"в таком случае"},
            std::string_view{"в этом случае"},
            std::string_view{"при этом"},
            std::string_view{"после этого"},
            std::string_view{"дальше"},
            std::string_view{"что дальше"},
            std::string_view{"а дальше"},
            std::string_view{"а "},
            std::string_view{"и "},
    };

    if (starts_with_any(normalized_text, std::span{follow_up_prefixes})) {
        return true;
    }

    constexpr auto follow_up_fragments = std::array{
            std::string_view{"объясни подробнее"},
            std::string_view{"распиши подробнее"},
            std::string_view{"подробнее"},
            std::string_view{"распиши"},
            std::string_view{"подробно"},
            std::string_view{"конкретнее"},
            std::string_view{"конкретней"},
            std::string_view{"продолжи"},
            std::string_view{"приведи пример"},
            std::string_view{"конкретные шаги"},
            std::string_view{"ничего не понял"},
            std::string_view{"ничего не поняла"},
            std::string_view{"ни хуя не понял"},
            std::string_view{"ни хуя не поняла"},
            std::string_view{"нихуя не понял"},
            std::string_view{"нихуя не поняла"},
            std::string_view{"какие конкретные шаги"},
            std::string_view{"для этого случая"},
            std::string_view{"в этом случае"},
            std::string_view{"какой таблицей"},
            std::string_view{"какая таблица"},
            std::string_view{"какой файл"},
            std::string_view{"какого формата"},
            std::string_view{"какой формат"},
            std::string_view{"какой шаблон"},
            std::string_view{"может ли"},
            std::string_view{"можно ли"},
            std::string_view{"возможно ли"},
            std::string_view{"что за файл"},
            std::string_view{"что за таблица"},
            std::string_view{"пример фразы"},
            std::string_view{"что значит"},
            std::string_view{"это обязательно"},
            std::string_view{"можно иначе"},
            std::string_view{"почему так"},
            std::string_view{"какой второй вариант"},
            std::string_view{"второй вариант"},
            std::string_view{"кратко"},
            std::string_view{"кратче"},
            std::string_view{"коротко"},
            std::string_view{"покороче"},
            std::string_view{"короче"},
            std::string_view{"сократи"},
            std::string_view{"сокращенно"},
            std::string_view{"сокращённо"},
            std::string_view{"объясни проще"},
            std::string_view{"расскажи проще"},
            std::string_view{"ответь проще"},
            std::string_view{"напиши проще"},
            std::string_view{"сделай кратко"},
            std::string_view{"сделай кратче"},
            std::string_view{"сделай коротко"},
            std::string_view{"сделай короче"},
            std::string_view{"без подробностей"},
            std::string_view{"в двух словах"},
            std::string_view{"одним предложением"},
            std::string_view{"переформулируй"},
            std::string_view{"перефразируй"},
            std::string_view{"перефраз"},
            std::string_view{"перефариз"},
            std::string_view{"перепиши"},
            std::string_view{"простыми словами"},
            std::string_view{"составь определение"},
            std::string_view{"дай определение"},
            std::string_view{"сформулируй определение"},
            std::string_view{"что проверить"},
            std::string_view{"что надо проверить"},
            std::string_view{"что нужно проверить"},
            std::string_view{"что обязательно проверить"},
            std::string_view{"что важно проверить"},
            std::string_view{"что уточнить"},
            std::string_view{"что обязательно уточнить"},
            std::string_view{"какие данные проверить"},
            std::string_view{"какие поля проверить"},
            std::string_view{"уточни порядок действий"},
            std::string_view{"уточни порядок"},
            std::string_view{"уточни шаги"},
            std::string_view{"проверь порядок действий"},
            std::string_view{"правильно ли я понимаю"},
            std::string_view{"правильно ли я понял"},
            std::string_view{"правильно ли я поняла"},
            std::string_view{"верно ли я понимаю"},
            std::string_view{"верно ли я понял"},
            std::string_view{"верно ли я поняла"},
            std::string_view{"я правильно понял"},
            std::string_view{"я правильно поняла"},
            std::string_view{"я правильно понимаю"},
            std::string_view{"то есть надо"},
            std::string_view{"то есть нужно"},
            std::string_view{"то есть правильно"},
            std::string_view{"получается надо"},
            std::string_view{"получается нужно"},
            std::string_view{"проверь правильно ли"},
            std::string_view{"это правильный порядок"},
            std::string_view{"так правильно"},
            std::string_view{"так можно"},
            std::string_view{"чеклист"},
            std::string_view{"сделай чеклист"},
            std::string_view{"составь чеклист"},
            std::string_view{"что сказать клиенту"},
            std::string_view{"как сказать клиенту"},
            std::string_view{"пример ответа клиенту"},
            std::string_view{"пример сообщения клиенту"},
            std::string_view{"скрипт ответа"},
            std::string_view{"что за "},
            std::string_view{"чё за "},
            std::string_view{"че за "},
            std::string_view{"чё такое "},
            std::string_view{"че такое "},
            std::string_view{"чё означает "},
            std::string_view{"че означает "},
    };

    constexpr auto exact_follow_up_requests = std::array{
            std::string_view{"кратко"},
            std::string_view{"кратче"},
            std::string_view{"коротко"},
            std::string_view{"покороче"},
            std::string_view{"короче"},
            std::string_view{"подробнее"},
            std::string_view{"подробно"},
            std::string_view{"конкретнее"},
            std::string_view{"конкретней"},
            std::string_view{"конкретные шаги"},
            std::string_view{"какие конкретные шаги"},
            std::string_view{"какой файл"},
            std::string_view{"какая таблица"},
            std::string_view{"что за"},
            std::string_view{"чё за"},
            std::string_view{"че за"},
            std::string_view{"чё такое"},
            std::string_view{"че такое"},
            std::string_view{"чё означает"},
            std::string_view{"че означает"},
            std::string_view{"какой формат"},
            std::string_view{"какого формата"},
            std::string_view{"продолжи"},
            std::string_view{"сократи"},
            std::string_view{"переформулируй"},
            std::string_view{"перефразируй"},
            std::string_view{"перефраз"},
            std::string_view{"перефариз"},
            std::string_view{"перепиши"},
            std::string_view{"простыми словами"},
            std::string_view{"составь определение"},
            std::string_view{"дай определение"},
            std::string_view{"сформулируй определение"},
            std::string_view{"что проверить"},
            std::string_view{"что обязательно проверить"},
            std::string_view{"что уточнить"},
            std::string_view{"уточни порядок действий"},
            std::string_view{"уточни порядок"},
            std::string_view{"уточни шаги"},
            std::string_view{"чеклист"},
            std::string_view{"сделай чеклист"},
            std::string_view{"составь чеклист"},
    };

    if (std::ranges::contains(exact_follow_up_requests, normalized_text)) {
        return true;
    }

    return contains_any(normalized_text, std::span{follow_up_fragments});
}

[[nodiscard]] bool is_relation_term_separator(const unsigned char byte) noexcept {
    if (byte >= 128) {
        return false;
    }

    return std::isspace(byte) != 0 || std::ispunct(byte) != 0;
}

[[nodiscard]] std::vector<std::string> make_relation_terms(const std::string_view normalized_text) {
    auto terms = std::vector<std::string>{};
    auto current = std::string{};

    for (const auto ch : normalized_text) {
        const auto byte = static_cast<unsigned char>(ch);

        if (is_relation_term_separator(byte)) {
            if (!current.empty()) {
                terms.push_back(std::move(current));
                current.clear();
            }

            continue;
        }

        current.push_back(ch);
    }

    if (!current.empty()) {
        terms.push_back(std::move(current));
    }

    return terms;
}

[[nodiscard]] bool contains_relation_term(const std::span<const std::string> terms,
                                          const std::string_view expected) noexcept {
    return std::ranges::any_of(terms, [expected](const std::string &term) noexcept { return term == expected; });
}

[[nodiscard]] bool contains_any_relation_term(const std::span<const std::string> terms,
                                              const std::span<const std::string_view> expected_terms) noexcept {
    return std::ranges::any_of(expected_terms, [terms](const std::string_view expected) noexcept {
        return contains_relation_term(terms, expected);
    });
}

[[nodiscard]] bool looks_like_short_reference_to_previous_answer(const std::string_view normalized_text) {
    if (normalized_text.size() > 120) {
        return false;
    }

    const auto terms = make_relation_terms(normalized_text);

    if (terms.empty()) {
        return false;
    }

    constexpr auto reference_terms = std::array{
            std::string_view{"это"},
            std::string_view{"этот"},
            std::string_view{"эта"},
            std::string_view{"эти"},
            std::string_view{"так"},
            std::string_view{"тогда"},
            std::string_view{"там"},
            std::string_view{"его"},
            std::string_view{"ее"},
            std::string_view{"её"},
            std::string_view{"он"},
            std::string_view{"она"},
            std::string_view{"они"},
            std::string_view{"пункт"},
            std::string_view{"вариант"},
            std::string_view{"дальше"},
    };

    if (contains_any_relation_term(terms, std::span{reference_terms})) {
        return true;
    }

    constexpr auto reference_phrases = std::array{
            std::string_view{"что с этим"},
            std::string_view{"как с этим"},
            std::string_view{"что по этому"},
            std::string_view{"как по этому"},
    };

    return contains_any(normalized_text, std::span{reference_phrases});
}

[[nodiscard]] bool looks_like_contextual_order_or_confirmation_question(
        const std::string_view normalized_text) noexcept {
    if (normalized_text.size() > 260) {
        return false;
    }

    constexpr auto order_or_confirmation_fragments = std::array{
            std::string_view{"перед "},       std::string_view{"до "},         std::string_view{"после"},
            std::string_view{"сначала"},      std::string_view{"потом"},       std::string_view{"затем"},
            std::string_view{"или после"},    std::string_view{"или до"},      std::string_view{"или неважно"},
            std::string_view{"неважно"},      std::string_view{"обязательно"}, std::string_view{"должна"},
            std::string_view{"должен"},       std::string_view{"должны"},      std::string_view{"надо ли"},
            std::string_view{"нужно ли"},     std::string_view{"можно ли"},    std::string_view{"нужно сначала"},
            std::string_view{"надо сначала"},
    };

    if (!contains_any(normalized_text, std::span{order_or_confirmation_fragments})) {
        return false;
    }

    constexpr auto service_action_fragments = std::array{
            std::string_view{"звон"},
            std::string_view{"позвон"},
            std::string_view{"напис"},
            std::string_view{"сообщ"},
            std::string_view{"сказ"},
            std::string_view{"предупред"},
            std::string_view{"уведом"},
            std::string_view{"уточн"},
            std::string_view{"подтверд"},
            std::string_view{"провери"},
            std::string_view{"перенос"},
            std::string_view{"отмен"},
            std::string_view{"запис"},
            std::string_view{"визит"},
            std::string_view{"клиент"},
    };

    return contains_any(normalized_text, std::span{service_action_fragments});
}

[[nodiscard]] bool looks_like_short_clarifying_question(const std::string_view normalized_text) {
    if (normalized_text.size() > 180) {
        return false;
    }

    if (normalized_text.find('?') == std::string_view::npos) {
        return false;
    }

    constexpr auto clarifying_terms = std::array{
            std::string_view{"какой"},
            std::string_view{"какая"},
            std::string_view{"какое"},
            std::string_view{"какие"},
            std::string_view{"какого"},
            std::string_view{"чем"},
            std::string_view{"куда"},
            std::string_view{"где"},
            std::string_view{"как"},
            std::string_view{"зачем"},
            std::string_view{"почему"},
            std::string_view{"что"},
    };

    const auto terms = make_relation_terms(normalized_text);

    if (terms.empty()) {
        return false;
    }

    return contains_any_relation_term(terms, std::span{clarifying_terms});
}

[[nodiscard]] chat_relation_kind_e classify_relation_to_previous_answer(
        const std::string_view user_text,
        const bool has_previous_anchor,
        const previous_answer_transform_kind_e transform_kind,
        const bool current_retrieval_returns_previous_source,
        const std::span<const retrieved_knowledge_s> knowledge) {
    if (!has_previous_anchor) {
        return chat_relation_kind_e::standalone;
    }

    const auto normalized_text = normalize_user_query_for_relation(user_text);

    if (normalized_text.empty()) {
        return chat_relation_kind_e::standalone;
    }

    if (transform_kind != previous_answer_transform_kind_e::none) {
        return chat_relation_kind_e::transform_previous_answer;
    }

    if (looks_like_explicit_follow_up(normalized_text)) {
        return chat_relation_kind_e::follow_up;
    }

    if (has_any_retrieved_knowledge(knowledge) && current_retrieval_returns_previous_source &&
        !is_direct_knowledge_answer_candidate(knowledge)) {
        return chat_relation_kind_e::follow_up;
    }

    if (looks_like_contextual_order_or_confirmation_question(normalized_text)) {
        return chat_relation_kind_e::follow_up;
    }

    if (looks_like_short_reference_to_previous_answer(normalized_text)) {
        return chat_relation_kind_e::follow_up;
    }

    /*
     * Новый релевантный Markdown-контекст обычно означает самостоятельную
     * тему. Но проверка стоит после явных признаков follow-up: иначе вопросы
     * вида «перед переносом или после?» ошибочно отрываются от предыдущего
     * direct-knowledge ответа только потому, что retrieval нашёл похожий файл.
     */
    if (has_any_retrieved_knowledge(knowledge)) {
        return chat_relation_kind_e::standalone;
    }

    if (looks_like_short_clarifying_question(normalized_text)) {
        return chat_relation_kind_e::follow_up;
    }

    return chat_relation_kind_e::standalone;
}

[[nodiscard]] std::vector<std::uint64_t> trim_topic_anchor_ids(std::vector<std::uint64_t> ids) {
    if (ids.size() <= max_topic_anchor_ids) {
        return ids;
    }

    ids.erase(ids.begin(), ids.end() - static_cast<std::ptrdiff_t>(max_topic_anchor_ids));

    return ids;
}

void append_unique_anchor_id(std::vector<std::uint64_t> &ids, const std::uint64_t id) {
    if (!ids.empty() && ids.back() == id) {
        return;
    }

    ids.push_back(id);
}

[[nodiscard]] std::vector<std::uint64_t> make_topic_anchor_ids(std::vector<std::uint64_t> relatives,
                                                               const std::uint64_t user_id,
                                                               const std::uint64_t assistant_id) {
    append_unique_anchor_id(relatives, user_id);
    append_unique_anchor_id(relatives, assistant_id);

    return trim_topic_anchor_ids(std::move(relatives));
}

} // namespace

assistant_context_s::assistant_context_s(const assistant_profile_e profile_value,
                                         std::filesystem::path history_file_value,
                                         std::filesystem::path knowledge_directory,
                                         const workplace_role_e workplace_role_value,
                                         const std::shared_ptr<spdlog::logger> &logger)
    : profile{profile_value},
      history_file{std::move(history_file_value)},
      workplace_role{workplace_role_value},
      history_logger{clone_logger(logger, std::format("ChatHistory.{}", to_string(profile_value)))},
      knowledge{
              std::move(knowledge_directory),
              profile_value == assistant_profile_e::workflow ? knowledge_domain_e::workflow
                                                             : knowledge_domain_e::texting,
              clone_logger(logger, std::format("KnowledgeStorage.{}", to_string(profile_value))),
      } {}

LlamaEngine::LlamaEngine(engine_config_s config,
                         llm::llama_server_config_s server_config,
                         const std::shared_ptr<spdlog::logger> &logger)
    : m_config{validate_engine_config(std::move(config))},
      m_logger{clone_logger(logger, "LlamaEngine")},
      m_server{
              std::move(server_config),
              m_logger->clone("LlamaServer"),
      },
      m_client{
              m_server,
              m_config.client,
              m_logger->clone("LlamaClient"),
      },
      m_active_context{
              assistant_profile_e::workflow,
              m_config.history_file,
              workflow_knowledge_directory(m_config.knowledge_directory),
              m_config.workplace_role,
              m_logger,
      },
      m_inactive_context{
              assistant_profile_e::texting,
              m_config.texting_history_file,
              m_config.knowledge_directory / "texting",
              workplace_role_e::general,
              m_logger,
      } {}

void LlamaEngine::load() {
    assert(!contexts_loaded());

    if (contexts_loaded()) {
        std::terminate();
    }

    load_organization_config();
    load_context(m_active_context);
    load_context(m_inactive_context);
    load_active_profile();

    m_logger->info("LlamaEngine loaded");
    m_logger->info("Initial assistant profile: {}", to_string(active_profile()));
    m_logger->info("Configured texting style: {}", to_string(m_config.texting_style));

    for (const auto *context : {&m_active_context, &m_inactive_context}) {
        m_logger->info("Assistant context '{}': role={} history_entries={} knowledge_files={} compact_context={}",
                       to_string(context->profile),
                       to_string(context->workplace_role),
                       context->history.size(),
                       context->knowledge.size(),
                       context->context_state.has_value());
    }
}

void LlamaEngine::start() {
    require_loaded(contexts_loaded());

    assert(!m_server.is_running());

    if (m_server.is_running()) {
        std::terminate();
    }

    m_logger->info("Starting llama-server for assistant profile '{}'", to_string(active_profile()));

    m_server.start();

    m_logger->info("llama-server started: {}", m_server.url());
}

void LlamaEngine::stop() noexcept {
    m_server.stop_generating();
    m_server.stop();
}

engine_answer_s LlamaEngine::ask(const std::string_view user_text,
                                 const llm::llama_stream_callback_t &stream_callback) {
    require_loaded(contexts_loaded());

    if (util::is_blank(user_text)) {
        throw std::runtime_error{"User message is empty"};
    }

    auto request_guard = request_active_guard_s{m_request_active};

    reload_organization_config_if_changed();

    if (auto answer = try_answer_from_organization_config(user_text); answer.has_value()) {
        return std::move(*answer);
    }

    if (active_profile() == assistant_profile_e::texting) {
        if (auto answer = try_answer_from_normalized_organization_config(user_text); answer.has_value()) {
            return std::move(*answer);
        }

        return ask_texting(user_text, stream_callback);
    }

    return ask_workflow(user_text, stream_callback);
}

void LlamaEngine::load_organization_config() {
    std::error_code error;

    if (!std::filesystem::exists(m_config.organization_config_file, error)) {
        m_logger->warn("Organization config '{}' was not found; static pre-processing is disabled",
                       m_config.organization_config_file.string());
        m_organization_config = {};
        m_organization_config.enabled = false;
        m_organization_config_write_time.reset();
        return;
    }

    m_organization_config = ::stz::intern::load_organization_config(
            m_config.organization_config_file);
    m_organization_config_write_time = std::filesystem::last_write_time(
            m_config.organization_config_file);

    m_logger->info("Organization config loaded: type={} brand='{}'",
                   to_string(m_organization_config.business_type),
                   m_organization_config.brand_name);
}

void LlamaEngine::reload_organization_config_if_changed() {
    std::error_code error;

    if (!std::filesystem::exists(m_config.organization_config_file, error)) {
        return;
    }

    const auto write_time = std::filesystem::last_write_time(
            m_config.organization_config_file,
            error);

    if (error || (m_organization_config_write_time.has_value() &&
                  *m_organization_config_write_time == write_time)) {
        return;
    }

    try {
        auto updated = ::stz::intern::load_organization_config(
                m_config.organization_config_file);
        m_organization_config = std::move(updated);
        m_organization_config_write_time = write_time;

        m_logger->info("Organization config reloaded: type={} brand='{}'",
                       to_string(m_organization_config.business_type),
                       m_organization_config.brand_name);
    } catch (const std::exception &exception) {
        m_organization_config_write_time = write_time;
        m_logger->error("Failed to reload organization config '{}': {}. "
                        "The previous valid configuration remains active.",
                        m_config.organization_config_file.string(),
                        exception.what());
    }
}

[[nodiscard]] std::string workflow_organization_request_message() {
    return
            "⚠️ Данный запрос **не относится** непосредственно к **инструкциям в контексте рабочего процесса** "
            "и к ним причастному или ситуациям, связанных с ЧС (первая медицинская помощь, пожар, "
            "объявленая **беспилотная** или **ракетная** опасность и пр.).\n\n"
            "Для составления ответов на сообщения пользователей касаемо самой организации, внешних и внутренних "
            "моментов: \"в какие дни вы работаете?\", \"есть ли рядом парковка\", \"какие есть методы оплаты?\" "
            "и многие другие — воспользуйтесь другим режимом бота — `Ответы клиентам`, сменив его рядом "
            "с кнопкой отправки запроса, левее.\n\n"
            "> Примечание: в случае, если Вы не согласны с данным ответом на ваш запрос и считаете его ошибочным, "
            "то просим Вас 🙏 сообщить об этом, нажав сверху-справа на «Сообщить о проблеме», заполнив форму "
            "со всей необходимой информацией и отправив получившуюся заявку. ✉️\n"
            ">\n"
            "> Заранее спасибо, что пытаетесь сделать нас лучше. 😊👍";
}

std::optional<engine_answer_s> LlamaEngine::complete_organization_config_answer(
        const std::string_view user_text,
        organization_config_answer_s config_answer,
        const std::string_view log_reason) {
    const auto user_index = append_pending_user_entry(user_text);
    auto answer_body = std::string{};

    if (active_profile() == assistant_profile_e::workflow) {
        answer_body = workflow_organization_request_message();
    } else if (active_profile() == assistant_profile_e::texting) {
        answer_body = std::move(config_answer.customer_text);
        apply_organization_texting_style_tokens(answer_body, m_config.texting_style);
        ensure_texting_reply_greeting(answer_body, user_text, m_config.texting_style);
        append_organization_texting_emoji(answer_body,
                                          config_answer.emoji,
                                          m_config.texting_style);
    } else if (looks_like_customer_reply_request(user_text)) {
        auto customer_text = std::move(config_answer.customer_text);
        apply_organization_texting_style_tokens(customer_text, m_config.texting_style);
        answer_body = std::format("Клиенту можно ответить:\n\n«{}»",
                                  customer_text);
    } else {
        answer_body = std::move(config_answer.fact_text);
    }

    util::trim(answer_body);

    auto source_filename = m_config.organization_config_file.filename().string();

    if (source_filename.empty()) {
        source_filename = m_config.organization_config_file.string();
    }

    auto source_filenames = std::vector<std::string>{std::move(source_filename)};
    auto visible_answer = answer_body;

    const auto empty_knowledge = std::span<const retrieved_knowledge_s>{};
    auto context_state = make_context_state(false,
                                            false,
                                            user_text,
                                            answer_body,
                                            source_filenames,
                                            empty_knowledge);

    const auto user_id = m_active_context.history[user_index].id;
    m_active_context.history[user_index].answer_kind = chat_answer_kind_e::direct_knowledge;
    m_active_context.history[user_index].status = chat_message_status_e::completed;
    m_active_context.history[user_index].relatives.clear();

    auto assistant_relatives = std::vector<std::uint64_t>{user_id};
    const auto assistant_id = make_next_chat_entry_id(m_active_context.history);

    m_active_context.history.push_back(chat_history_entry_s{
            .id = assistant_id,
            .answer_kind = chat_answer_kind_e::direct_knowledge,
            .status = chat_message_status_e::completed,
            .user = std::nullopt,
            .assistant = chat_visible_message_s{
                    .content = visible_answer,
                    .created_at = util::make_local_timestamp(),
                    .model_content = make_chat_model_content(answer_body,
                                                             chat_role_e::assistant),
                    .name = "AI-бот",
            },
            .source_files = source_filenames,
            .relatives = assistant_relatives,
            .context_state = context_state,
    });

    m_active_context.context_state = std::move(context_state);
    m_active_context.last_topic_anchor_ids = make_topic_anchor_ids(
            std::move(assistant_relatives),
            user_id,
            assistant_id);
    save_history();

    m_logger->info("Answered from organization config: profile={} topic='{}' reason='{}' source='{}'",
                   to_string(active_profile()),
                   config_answer.topic,
                   log_reason,
                   m_config.organization_config_file.string());

    return engine_answer_s{
            .status = chat_message_status_e::completed,
            .content = std::move(visible_answer),
    };
}

std::optional<engine_answer_s> LlamaEngine::try_answer_from_organization_config(
        const std::string_view user_text) {
    auto config_answer = answer_from_organization_config(m_organization_config, user_text);

    if (!config_answer.has_value()) {
        return std::nullopt;
    }

    return complete_organization_config_answer(user_text,
                                               std::move(*config_answer),
                                               "direct");
}

std::optional<std::string> LlamaEngine::normalize_texting_organization_query(
        const std::string_view user_text) {
    if (active_profile() != assistant_profile_e::texting || user_text.empty()) {
        return std::nullopt;
    }

    if (!m_server.is_running()) {
        return std::nullopt;
    }

    const auto input_chars = choose_texting_normalizer_input_chars(m_config, user_text);
    const auto messages = std::array{
            chat_message_s{
                    .role = chat_role_e::system,
                    .name = "texting_organization_query_normalizer",
                    .content = build_texting_organization_query_normalizer_prompt(),
                    .created_at = util::make_local_timestamp(),
                    .source_files = {},
            },
            chat_message_s{
                    .role = chat_role_e::user,
                    .name = "user",
                    .content = truncate_utf8(user_text, input_chars),
                    .created_at = util::make_local_timestamp(),
                    .source_files = {},
            },
    };

    const auto response = m_client.complete_chat(
            messages,
            {},
            llm::llama_completion_options_s{
                    .temperature = 0.0,
                    .top_p = 1.0,
                    .max_tokens = m_config.max_texting_normalizer_tokens,
            });

    if (response.status == llm::llama_completion_status_e::cancelled) {
        return std::nullopt;
    }

    const auto normalized = parse_texting_organization_normalized_query(response.content);

    if (!normalized.has_value()) {
        m_logger->info("Texting organization query normalizer returned no usable query");
        return std::nullopt;
    }

    m_logger->info("Texting organization query normalized: '{}' -> '{}'",
                   compact_whitespace(user_text),
                   *normalized);

    return normalized;
}

std::optional<engine_answer_s> LlamaEngine::try_answer_from_normalized_organization_config(
        const std::string_view user_text) {
    auto normalized_query = std::optional<std::string>{};

    try {
        normalized_query = normalize_texting_organization_query(user_text);
    } catch (const std::exception &error) {
        m_logger->warn("Texting organization query normalization failed: {}", error.what());
        return std::nullopt;
    } catch (...) {
        m_logger->warn("Texting organization query normalization failed with an unknown error");
        return std::nullopt;
    }

    if (!normalized_query.has_value()) {
        return std::nullopt;
    }

    auto config_answer = answer_from_organization_config(m_organization_config, *normalized_query);

    if (!config_answer.has_value()) {
        return std::nullopt;
    }

    return complete_organization_config_answer(user_text,
                                               std::move(*config_answer),
                                               "llm-normalized");
}

engine_answer_s LlamaEngine::ask_workflow(const std::string_view user_text,
                                          const llm::llama_stream_callback_t &stream_callback) {
    const auto user_index = append_pending_user_entry(user_text);

    save_history();

    const auto retrieve_options = knowledge_retrieve_options_s{
            .workplace_role = m_active_context.workplace_role,

            .include_general = true,
            .include_builtin = true,
            .include_custom = true,

            .limit = m_config.max_knowledge_documents,

            .max_chars_per_document = m_config.max_knowledge_chars_per_document,

            .min_ranked_score = m_config.min_ranked_knowledge_score,
    };

    /*
     * Answer routing order:
     * 1. For procedural requests, prefer document lookup over m_documents,
     *    starting from "Частые запросы пользователя".
     * 2. Otherwise, prefer strict glossary heading lookup over m_glossaries.
     * 3. LLM fallback with whatever context retrieval could safely provide.
     */
    const auto procedural_request = looks_like_procedural_request(user_text);
    const auto normalized_user_text = normalize_user_query_for_relation(user_text);
    const auto transform_kind = classify_previous_answer_transform_request(normalized_user_text);
    const auto transform_request = !m_active_context.last_topic_anchor_ids.empty() &&
                                   transform_kind != previous_answer_transform_kind_e::none;

    const auto retrieve_primary_knowledge = [&](const std::string_view query,
                                                const bool prefer_documents) {
        const auto retrieve_from_source = [&](const knowledge_source_e source) {
            auto options = retrieve_options;
            options.include_builtin = source == knowledge_source_e::builtin;
            options.include_custom = source == knowledge_source_e::custom;

            if (prefer_documents) {
                auto documents = m_active_context.knowledge.retrieve(query, options);

                if (!documents.empty()) {
                    return documents;
                }

                return m_active_context.knowledge.retrieve_glossary(query, options);
            }

            auto glossary = m_active_context.knowledge.retrieve_glossary(query, options);
            auto documents = procedural_request || glossary.empty()
                                     ? m_active_context.knowledge.retrieve(query, options)
                                     : std::vector<retrieved_knowledge_s>{};

            if (procedural_request && !documents.empty()) {
                return documents;
            }

            if (!glossary.empty()) {
                return glossary;
            }

            return documents;
        };

        auto custom = retrieve_from_source(knowledge_source_e::custom);

        if (!custom.empty()) {
            return custom;
        }

        return retrieve_from_source(knowledge_source_e::builtin);
    };

    const auto primary_knowledge = transform_request
                                           ? std::vector<retrieved_knowledge_s>{}
                                           : retrieve_primary_knowledge(user_text, false);

    for (const auto &item : primary_knowledge) {
        m_logger->info("Retrieved knowledge: {} "
                       "section='{}' score={} role={} source={} match={}",
                       item.filename,
                       item.tag_name,
                       item.score,
                       to_string(item.role),
                       to_string(item.source),
                       to_string(item.match));
    }

    const auto inherited_source_filenames = m_active_context.context_state.has_value()
                                                    ? m_active_context.context_state->source_files
                                                    : make_source_filenames_from_relatives(m_active_context.last_topic_anchor_ids);
    const auto current_retrieval_returns_previous_source = knowledge_contains_any_source_filename(
            primary_knowledge,
            inherited_source_filenames);

    const auto relation = classify_relation_to_previous_answer(user_text,
                                                               !m_active_context.last_topic_anchor_ids.empty(),
                                                               transform_kind,
                                                               current_retrieval_returns_previous_source,
                                                               primary_knowledge);

    auto knowledge = primary_knowledge;

    if (relation == chat_relation_kind_e::follow_up) {
        const auto inherited_knowledge = m_active_context.knowledge.retrieve_by_filenames(inherited_source_filenames,
                                                                           user_text,
                                                                           retrieve_options);

        const auto contextual_query = build_contextual_retrieval_query(user_text);
        const auto contextual_knowledge = contextual_query == user_text
                                                  ? std::vector<retrieved_knowledge_s>{}
                                                  : retrieve_primary_knowledge(contextual_query, true);

        /*
         * Stable active sources come first. A newly found file can extend the
         * current scenario, but cannot push out the files that established it.
         * The small limit also prevents one weak retrieval result from becoming
         * a permanent inherited source on all following turns.
         */
        knowledge = merge_contextual_knowledge(inherited_knowledge,
                                                contextual_knowledge,
                                                primary_knowledge,
                                                m_config.max_context_source_files);

        for (const auto &item : inherited_knowledge) {
            m_logger->info("Inherited contextual knowledge: {} "
                           "section='{}' score={} role={} source={} match={}",
                           item.filename,
                           item.tag_name,
                           item.score,
                           to_string(item.role),
                           to_string(item.source),
                           to_string(item.match));
        }

        for (const auto &item : contextual_knowledge) {
            m_logger->info("Contextual retrieval knowledge: {} "
                           "section='{}' score={} role={} source={} match={}",
                           item.filename,
                           item.tag_name,
                           item.score,
                           to_string(item.role),
                           to_string(item.source),
                           to_string(item.match));
        }
    } else if (relation == chat_relation_kind_e::transform_previous_answer &&
               transform_kind == previous_answer_transform_kind_e::expand) {
        /*
         * Expansion is not a fresh semantic search. It may use only the files
         * already active in the current scenario, which adds legitimate detail
         * without allowing retrieval drift into a neighbouring instruction.
         */
        knowledge = m_active_context.knowledge.retrieve_by_filenames(inherited_source_filenames,
                                                      user_text,
                                                      retrieve_options);

        for (const auto &item : knowledge) {
            m_logger->info("Inherited knowledge for answer expansion: {} "
                           "section='{}' score={} role={} source={} match={}",
                           item.filename,
                           item.tag_name,
                           item.score,
                           to_string(item.role),
                           to_string(item.source),
                           to_string(item.match));
        }
    }

    /*
     * Direct Markdown answer paths:
     * 1. The previous exact frequent-query or glossary-heading match.
     * 2. One H2 section explicitly selected by the current standalone or
     *    follow-up query inside the already resolved source document.
     *
     * llama-server and LlamaClient are not used in either case.
     */
    const auto direct_answer_relation = relation == chat_relation_kind_e::standalone ||
                                        relation == chat_relation_kind_e::follow_up;

    if (direct_answer_relation && should_answer_without_llm(user_text, knowledge)) {
        auto source_filenames = make_source_filenames(knowledge);

        auto answer_body = make_direct_answer(knowledge);

        auto answer = ensure_sources_block(answer_body, source_filenames);

        auto context_state = make_context_state(relation != chat_relation_kind_e::standalone,
                                                relation == chat_relation_kind_e::follow_up,
                                                user_text,
                                                answer_body,
                                                source_filenames,
                                                knowledge);

        const auto user_id = m_active_context.history[user_index].id;

        m_active_context.history[user_index].answer_kind = chat_answer_kind_e::direct_knowledge;

        m_active_context.history[user_index].status = chat_message_status_e::completed;

        if (relation == chat_relation_kind_e::follow_up) {
            m_active_context.history[user_index].relatives = m_active_context.last_topic_anchor_ids;
        } else {
            m_active_context.history[user_index].relatives.clear();
        }

        auto assistant_relatives = m_active_context.history[user_index].relatives;
        assistant_relatives.push_back(user_id);

        const auto assistant_id = make_next_chat_entry_id(m_active_context.history);

        m_active_context.history.push_back(chat_history_entry_s{
                .id = assistant_id,

                .answer_kind = chat_answer_kind_e::direct_knowledge,

                .status = chat_message_status_e::completed,

                .user = std::nullopt,

                .assistant =
                        chat_visible_message_s{
                                .content = answer,
                                .created_at = util::make_local_timestamp(),
                                .model_content = make_chat_model_content(answer_body, chat_role_e::assistant),
                                .name = "AI-бот",
                        },

                .source_files = source_filenames,

                .relatives = assistant_relatives,

                .context_state = context_state,
        });

        m_active_context.context_state = std::move(context_state);
        m_active_context.last_topic_anchor_ids = make_topic_anchor_ids(std::move(assistant_relatives), user_id, assistant_id);

        save_history();

        m_logger->info("Answered without LLM by direct knowledge match: "
                       "file={} section='{}' score={} match={}",
                       knowledge.front().filename,
                       knowledge.front().tag_name,
                       knowledge.front().score,
                       to_string(knowledge.front().match));

        return engine_answer_s{
                .status = chat_message_status_e::completed,

                .content = std::move(answer),
        };
    }

    if (relation == chat_relation_kind_e::standalone && is_direct_knowledge_answer_candidate(knowledge) &&
        looks_like_instruction_analysis_request(user_text)) {
        m_logger->info("Direct knowledge match will be used as LLM context because the user asks for analysis: "
                       "file={} score={} match={}",
                       knowledge.front().filename,
                       knowledge.front().score,
                       to_string(knowledge.front().match));
    }

    /*
     * До этой точки прямого совпадения с готовым
     * Markdown-скриптом не найдено.
     *
     * Только теперь требуется llama-server.
     */
    if (!m_server.is_running()) {
        m_active_context.history[user_index].status = chat_message_status_e::failed;

        save_history();

        throw std::runtime_error{"Cannot generate answer: "
                                 "llama-server is not ready"};
    }

    m_active_context.history[user_index].answer_kind = chat_answer_kind_e::llm;

    if (relation != chat_relation_kind_e::standalone) {
        m_active_context.history[user_index].relatives = m_active_context.last_topic_anchor_ids;
    } else {
        m_active_context.history[user_index].relatives.clear();
    }

    m_logger->info("LLM request relation: {} transform={} previous_anchor_ids={} relatives_used={} same_source={}",
                   relation_kind_name(relation),
                   transform_kind_name(transform_kind),
                   m_active_context.last_topic_anchor_ids.size(),
                   m_active_context.history[user_index].relatives.size(),
                   current_retrieval_returns_previous_source);

    save_history();

    auto response = llm::llama_client_response_s{};

    try {
        const auto request_messages = build_request_messages(
                m_active_context.history[user_index],
                knowledge,
                relation == chat_relation_kind_e::transform_previous_answer);

        response = m_client.complete_chat(request_messages, stream_callback);
    } catch (...) {
        m_active_context.history[user_index].status = chat_message_status_e::failed;

        save_history();

        throw;
    }

    if (response.status == llm::llama_completion_status_e::cancelled) {
        m_active_context.history[user_index].status = chat_message_status_e::cancelled;

        save_history();

        m_logger->info("Answer generation was cancelled");

        return engine_answer_s{
                .status = chat_message_status_e::cancelled,

                .content = std::move(response.content),
        };
    }

    auto source_filenames = relation == chat_relation_kind_e::transform_previous_answer
                                    ? inherited_source_filenames
                                    : make_context_source_filenames(knowledge);
    limit_source_files(source_filenames, m_config.max_context_source_files);

    auto answer_body = remove_generated_service_lines(response.content);

    if (answer_body.empty()) {
        answer_body = "Не удалось получить содержательный "
                      "ответ от модели.";
    }

    auto answer = ensure_sources_block(answer_body, source_filenames);

    auto context_state = make_context_state(relation != chat_relation_kind_e::standalone,
                                            relation == chat_relation_kind_e::follow_up,
                                            user_text,
                                            answer_body,
                                            source_filenames,
                                            knowledge);

    const auto user_id = m_active_context.history[user_index].id;

    auto assistant_relatives = m_active_context.history[user_index].relatives;

    assistant_relatives.push_back(user_id);

    m_active_context.history[user_index].status = chat_message_status_e::completed;

    const auto assistant_id = make_next_chat_entry_id(m_active_context.history);

    m_active_context.history.push_back(chat_history_entry_s{
            .id = assistant_id,

            .answer_kind = chat_answer_kind_e::llm,

            .status = chat_message_status_e::completed,

            .user = std::nullopt,

            .assistant =
                    chat_visible_message_s{
                            .content = answer,
                            .created_at = util::make_local_timestamp(),
                            .model_content = make_chat_model_content(answer_body, chat_role_e::assistant),
                            .name = "AI-бот",
                    },

            .source_files = source_filenames,

            .relatives = assistant_relatives,

            .context_state = context_state,
    });

    m_active_context.context_state = std::move(context_state);
    m_active_context.last_topic_anchor_ids = make_topic_anchor_ids(std::move(assistant_relatives), user_id, assistant_id);

    save_history();

    return engine_answer_s{
            .status = chat_message_status_e::completed,

            .content = std::move(answer),
    };
}

engine_answer_s LlamaEngine::ask_texting(const std::string_view user_text,
                                         const llm::llama_stream_callback_t &stream_callback) {
    const auto user_index = append_pending_user_entry(user_text);
    save_history();

    const auto normalized_user_text = normalize_user_query_for_relation(user_text);
    const auto transform_kind = classify_previous_answer_transform_request(normalized_user_text);
    const auto transform_previous_answer = !m_active_context.last_topic_anchor_ids.empty() &&
                                           transform_kind != previous_answer_transform_kind_e::none;

    if (!m_server.is_running()) {
        m_active_context.history[user_index].status = chat_message_status_e::failed;
        save_history();
        throw std::runtime_error{"Cannot generate texting answer: llama-server is not ready"};
    }

    m_active_context.history[user_index].answer_kind = chat_answer_kind_e::llm;

    if (transform_previous_answer) {
        m_active_context.history[user_index].relatives = m_active_context.last_topic_anchor_ids;
    } else {
        m_active_context.history[user_index].relatives.clear();
    }

    save_history();

    auto knowledge = std::vector<retrieved_knowledge_s>{};

    if (!transform_previous_answer) {
        const auto retrieve_texting_query = [&](const std::string_view query,
                                                const std::size_t min_ranked_score) {
            const auto retrieve_from_source = [&](const knowledge_source_e source,
                                                  const texting_style_e style) {
                return m_active_context.knowledge.retrieve_glossary(
                        query,
                        knowledge_retrieve_options_s{
                                .workplace_role = workplace_role_e::general,
                                .include_general = true,
                                .include_builtin = source == knowledge_source_e::builtin,
                                .include_custom = source == knowledge_source_e::custom,
                                .texting_style = style,
                                .limit = 1,
                                .max_chars_per_document = m_config.max_knowledge_chars_per_document,
                                .min_ranked_score = min_ranked_score,
                        });
            };

            auto candidate = retrieve_from_source(knowledge_source_e::custom, texting_style_e::any);

            if (candidate.empty()) {
                candidate = retrieve_from_source(knowledge_source_e::builtin, m_config.texting_style);
            }

            std::erase_if(candidate, [&](const retrieved_knowledge_s &item) {
                const auto compatible = texting_knowledge_is_compatible(item, normalized_user_text);

                if (!compatible) {
                    m_logger->warn("Rejected texting scenario '{}' because its explicit prerequisites are absent",
                                   item.tag_name);
                }

                return !compatible;
            });

            return candidate;
        };

        auto direct_candidate = retrieve_texting_query(user_text, m_config.min_ranked_knowledge_score);
        append_unique_knowledge(knowledge, direct_candidate, m_config.max_texting_selected_scripts);

        if (!knowledge.empty()) {
            m_logger->info("Texting script matched directly before LLM selector");
        }

        auto selector_queries = std::vector<std::string>{};

        if (knowledge.empty()) {
            auto selector_response = llm::llama_client_response_s{};
            auto selector_completed = false;

            try {
                const auto selector_messages = std::array{
                        chat_message_s{
                                .role = chat_role_e::system,
                                .name = "texting_scenario_selector",
                                .content = build_texting_selector_system_prompt(),
                                .created_at = util::make_local_timestamp(),
                                .source_files = {},
                        },
                        chat_message_s{
                                .role = chat_role_e::user,
                                .name = "user",
                                .content = truncate_utf8(user_text, m_config.max_texting_selector_input_chars),
                                .created_at = util::make_local_timestamp(),
                                .source_files = {},
                        },
                };

                selector_response = m_client.complete_chat(
                        selector_messages,
                        {},
                        llm::llama_completion_options_s{
                                .temperature = 0.0,
                                .top_p = 1.0,
                                .max_tokens = m_config.max_texting_selector_tokens,
                        });
                selector_completed = true;
            } catch (const std::exception &error) {
                m_logger->warn("Texting scenario selector failed; generating without scripts: {}", error.what());
            } catch (...) {
                m_logger->warn("Texting scenario selector failed with an unknown error; generating without scripts");
            }

            if (selector_completed) {
                m_logger->debug("Texting scenario selector response: {}",
                                compact_whitespace(selector_response.content));
            }

            if (selector_completed && selector_response.status == llm::llama_completion_status_e::cancelled) {
                m_active_context.history[user_index].status = chat_message_status_e::cancelled;
                save_history();

                return engine_answer_s{
                        .status = chat_message_status_e::cancelled,
                        .content = {},
                };
            }

            selector_queries = selector_completed
                                       ? parse_texting_selector_queries(
                                                 selector_response.content,
                                                 std::max<std::size_t>(m_config.max_texting_selected_scripts, 3))
                                       : std::vector<std::string>{};

            if (selector_queries.empty()) {
                m_logger->warn("Texting scenario selector returned no usable queries; retrying with a compact prompt");

                try {
                    const auto retry_messages = std::array{
                            chat_message_s{
                                    .role = chat_role_e::system,
                                    .name = "texting_scenario_selector_retry",
                                    .content = build_texting_selector_retry_system_prompt(),
                                    .created_at = util::make_local_timestamp(),
                                    .source_files = {},
                            },
                            chat_message_s{
                                    .role = chat_role_e::user,
                                    .name = "user",
                                    .content = truncate_utf8(user_text, m_config.max_texting_selector_input_chars),
                                    .created_at = util::make_local_timestamp(),
                                    .source_files = {},
                            },
                    };

                    const auto retry_response = m_client.complete_chat(
                            retry_messages,
                            {},
                            llm::llama_completion_options_s{
                                    .temperature = 0.0,
                                    .top_p = 1.0,
                                    .max_tokens = std::min<std::int32_t>(m_config.max_texting_selector_tokens, 64),
                            });

                    if (retry_response.status == llm::llama_completion_status_e::cancelled) {
                        m_active_context.history[user_index].status = chat_message_status_e::cancelled;
                        save_history();

                        return engine_answer_s{
                                .status = chat_message_status_e::cancelled,
                                .content = {},
                        };
                    }

                    m_logger->debug("Texting compact selector response: {}",
                                    compact_whitespace(retry_response.content));
                    selector_queries = parse_texting_selector_queries(
                            retry_response.content,
                            std::max<std::size_t>(m_config.max_texting_selected_scripts, 3));
                } catch (const std::exception &error) {
                    m_logger->warn("Texting compact selector retry failed: {}", error.what());
                } catch (...) {
                    m_logger->warn("Texting compact selector retry failed with an unknown error");
                }
            }

            if (selector_queries.empty()) {
                m_logger->info("Texting scenario selector returned no usable queries after retry");
            }

            for (const auto &query : selector_queries) {
                m_logger->info("Texting LLM selector query: '{}'", query);

                auto candidate = retrieve_texting_query(query, m_config.min_ranked_knowledge_score);
                append_unique_knowledge(knowledge, candidate, m_config.max_texting_selected_scripts);

                if (knowledge.size() >= m_config.max_texting_selected_scripts) {
                    break;
                }
            }
        }

        if (knowledge.empty()) {
            auto fallback_queries = selector_queries;
            fallback_queries.push_back(std::string{user_text});

            for (const auto &query : fallback_queries) {
                auto candidate = retrieve_texting_query(query, 0);
                append_unique_knowledge(knowledge, candidate, 1);

                if (!knowledge.empty()) {
                    m_logger->info("Texting nearest script fallback selected a low-score candidate");
                    break;
                }
            }
        }

        if (looks_like_plain_texting_cancellation(normalized_user_text)) {
            m_logger->info("Texting routing override: using the plain cancellation scenario");
            auto candidate = retrieve_texting_query("Клиент хочет отменить запись заранее",
                                                    m_config.min_ranked_knowledge_score);

            if (!candidate.empty()) {
                knowledge.clear();
                append_unique_knowledge(knowledge, candidate, m_config.max_texting_selected_scripts);
            }
        }
    }

    for (const auto &item : knowledge) {
        m_logger->info("Texting script selected by LLM: {} section='{}' kind={} style={} score={} source={} match={}",
                       item.filename,
                       item.tag_name,
                       to_string(item.kind),
                       to_string(item.texting_style),
                       item.score,
                       to_string(item.source),
                       to_string(item.match));
    }

    auto selected_scripts = std::vector<retrieved_knowledge_s>{};
    selected_scripts.reserve(knowledge.size());

    for (const auto &item : knowledge) {
        if (item.kind == knowledge_document_kind_e::texting_script) {
            selected_scripts.push_back(item);
        }
    }

    auto scripted_answer = std::optional<std::string>{};
    auto applied_script_edits = std::size_t{0};
    auto rejected_script_edits = std::size_t{0};

    if (!transform_previous_answer && !selected_scripts.empty()) {
        auto edits = std::vector<texting_script_edit_s>{};
        auto adaptation_response = llm::llama_client_response_s{};
        auto adaptation_completed = false;

        try {
            const auto adaptation_messages = std::array{
                    chat_message_s{
                            .role = chat_role_e::system,
                            .name = "texting_script_adapter",
                            .content = build_texting_adaptation_system_prompt(selected_scripts),
                            .created_at = util::make_local_timestamp(),
                            .source_files = {},
                    },
                    chat_message_s{
                            .role = chat_role_e::user,
                            .name = "user",
                            .content = truncate_utf8(user_text, m_config.max_texting_adaptation_input_chars),
                            .created_at = util::make_local_timestamp(),
                            .source_files = {},
                    },
            };

            adaptation_response = m_client.complete_chat(
                    adaptation_messages,
                    {},
                    llm::llama_completion_options_s{
                            .temperature = 0.0,
                            .top_p = 1.0,
                            .max_tokens = m_config.max_texting_adaptation_tokens,
                    });
            adaptation_completed = true;
        } catch (const std::exception &error) {
            m_logger->warn("Texting surgical adaptation failed; using the original script: {}",
                           error.what());
        } catch (...) {
            m_logger->warn("Texting surgical adaptation failed with an unknown error; "
                           "using the original script");
        }

        if (adaptation_completed) {
            m_logger->debug("Texting surgical adaptation response: {}",
                            compact_whitespace(adaptation_response.content));
        }

        if (adaptation_completed && adaptation_response.status == llm::llama_completion_status_e::cancelled) {
            m_active_context.history[user_index].status = chat_message_status_e::cancelled;
            save_history();

            return engine_answer_s{
                    .status = chat_message_status_e::cancelled,
                    .content = {},
            };
        }

        auto adaptation_result = texting_adaptation_result_s{};
        if (adaptation_completed) {
            adaptation_result = parse_texting_adaptation_result(
                    adaptation_response.content,
                    m_config.max_texting_script_edits);
            edits = std::move(adaptation_result.edits);
        }

        m_logger->info("Texting adaptation analysis: concrete_issue={} summary='{}' photo='{}'",
                       adaptation_result.issue.specified,
                       adaptation_result.issue.summary,
                       adaptation_result.issue.photo);

        enforce_texting_issue_analysis_edits(
                selected_scripts.front().content,
                edits,
                m_config.max_texting_script_edits,
                adaptation_result.issue);

        auto edit_result = apply_texting_script_edits(selected_scripts.front().content, edits);
        applied_script_edits = edit_result.applied;
        rejected_script_edits = edit_result.rejected;
        if (!compact_whitespace(edit_result.content).empty()) {
            scripted_answer = std::move(edit_result.content);
        }

        m_logger->info("Texting corporate script assembled on CPU: file={} section='{}' requested_edits={} "
                       "applied_edits={} rejected_edits={}",
                       selected_scripts.front().filename,
                       selected_scripts.front().tag_name,
                       edits.size(),
                       applied_script_edits,
                       rejected_script_edits);
    }

    auto response = llm::llama_client_response_s{};
    auto answer_body = std::string{};

    if (scripted_answer.has_value()) {
        answer_body = std::move(*scripted_answer);
    } else {
        try {
            const auto request_messages = build_request_messages(
                    m_active_context.history[user_index],
                    knowledge,
                    transform_previous_answer);
            response = m_client.complete_chat(
                    request_messages,
                    stream_callback,
                    llm::llama_completion_options_s{
                            .temperature = texting_generation_temperature(m_config, m_config.texting_style),
                            .top_p = m_config.texting_top_p,
                            .max_tokens = m_config.max_texting_answer_tokens,
                    });
        } catch (...) {
            m_active_context.history[user_index].status = chat_message_status_e::failed;
            save_history();
            throw;
        }

        if (response.status == llm::llama_completion_status_e::cancelled) {
            m_active_context.history[user_index].status = chat_message_status_e::cancelled;
            save_history();

            return engine_answer_s{
                    .status = chat_message_status_e::cancelled,
                    .content = std::move(response.content),
            };
        }

        answer_body = remove_generated_service_lines(response.content);
        util::trim(answer_body);
    }

    if (!transform_previous_answer) {
        ensure_texting_reply_greeting(answer_body, user_text, m_config.texting_style);
    }

    auto source_filenames = transform_previous_answer
                                    ? make_source_filenames_from_relatives(m_active_context.last_topic_anchor_ids)
                                    : make_source_filenames(knowledge);
    limit_source_files(source_filenames, m_config.max_context_source_files);

    if (answer_body.empty()) {
        answer_body = "Не удалось получить содержательный ответ от модели.";
    }

    auto context_state = make_context_state(transform_previous_answer,
                                            false,
                                            user_text,
                                            answer_body,
                                            source_filenames,
                                            knowledge);
    const auto user_id = m_active_context.history[user_index].id;
    auto assistant_relatives = m_active_context.history[user_index].relatives;
    assistant_relatives.push_back(user_id);

    m_active_context.history[user_index].status = chat_message_status_e::completed;

    const auto assistant_id = make_next_chat_entry_id(m_active_context.history);

    m_active_context.history.push_back(chat_history_entry_s{
            .id = assistant_id,
            .answer_kind = chat_answer_kind_e::llm,
            .status = chat_message_status_e::completed,
            .user = std::nullopt,
            .assistant = chat_visible_message_s{
                    .content = answer_body,
                    .created_at = util::make_local_timestamp(),
                    .model_content = make_chat_model_content(answer_body, chat_role_e::assistant),
                    .name = "AI-бот",
            },
            .source_files = source_filenames,
            .relatives = assistant_relatives,
            .context_state = context_state,
    });

    m_active_context.context_state = std::move(context_state);
    m_active_context.last_topic_anchor_ids = make_topic_anchor_ids(
            std::move(assistant_relatives),
            user_id,
            assistant_id);
    save_history();

    m_logger->info("Texting answer completed: transform={} selected_knowledge={} scripted={} "
                   "applied_edits={} rejected_edits={}",
                   transform_previous_answer,
                   knowledge.size(),
                   scripted_answer.has_value(),
                   applied_script_edits,
                   rejected_script_edits);

    return engine_answer_s{
            .status = chat_message_status_e::completed,
            .content = std::move(answer_body),
    };
}

void LlamaEngine::stop_generating() noexcept { m_server.stop_generating(); }

bool LlamaEngine::is_running() const noexcept { return m_server.is_running(); }

bool LlamaEngine::model_generates() const noexcept {
    return m_request_active.load() || m_server.model_generates();
}

assistant_profile_e LlamaEngine::active_profile() const noexcept {
    return m_active_context.profile;
}

bool LlamaEngine::change_profile(const assistant_profile_e profile) {
    require_loaded(contexts_loaded());

    if (profile == m_active_context.profile) {
        return false;
    }

    if (m_request_active.load() || m_server.model_generates()) {
        throw std::runtime_error{"Cannot change assistant profile while a request is active"};
    }

    if (m_inactive_context.profile != profile) {
        throw std::runtime_error{std::format("Assistant profile '{}' is not loaded", to_string(profile))};
    }

    const auto previous_profile = m_active_context.profile;
    std::swap(m_active_context, m_inactive_context);
    save_active_profile();

    m_logger->info("Assistant profile switched: {} -> {}",
                   to_string(previous_profile),
                   to_string(m_active_context.profile));

    return true;
}

bool LlamaEngine::change_model(const llm::model_e model) {
    require_loaded(contexts_loaded());

    return m_server.change_model(model);
}

void LlamaEngine::store_model_cache() {
    require_loaded(contexts_loaded());

    m_server.store_model_cache();
}

void LlamaEngine::load_model_cache() {
    require_loaded(contexts_loaded());

    m_server.load_model_cache();
}

std::string LlamaEngine::server_url() const { return m_server.url(); }

llm::llama_server_state_info_s LlamaEngine::server_state() const { return m_server.state_info(); }

std::span<const chat_history_entry_s> LlamaEngine::history() const noexcept {
    return {
            m_active_context.history.data(),
            m_active_context.history.size(),
    };
}

void LlamaEngine::clear_history() {
    require_loaded(contexts_loaded());

    if (m_request_active.load() || m_server.model_generates()) {
        throw std::runtime_error{"Cannot clear chat history while an assistant request is active"};
    }

    m_active_context.history.clear();
    m_active_context.last_topic_anchor_ids.clear();
    m_active_context.context_state.reset();

    save_history();

    m_logger->info("Chat history and model context were cleared for assistant profile '{}'",
                   to_string(active_profile()));
}

void LlamaEngine::load_context(assistant_context_s &context) {
    assert(!context.loaded);

    context.history = load_chat_history(context.history_file, context.history_logger);
    finalize_interrupted_history_entries(context);
    context.knowledge.load();
    rebuild_last_topic_anchor(context);
    context.loaded = true;

    if (context.knowledge.empty()) {
        m_logger->warn("Knowledge storage is empty for assistant profile '{}'", to_string(context.profile));
    }
}

void LlamaEngine::save_history() const {
    save_chat_history(m_active_context.history_file, m_active_context.history);
}

void LlamaEngine::load_active_profile() {
    auto selected_profile = m_config.initial_profile;

    if (std::filesystem::exists(m_config.assistant_profile_state_file)) {
        try {
            const auto root = nlohmann::json::parse(
                    util::read_text_file(m_config.assistant_profile_state_file));
            selected_profile = assistant_profile_from_string(root.value("profile", "workflow"));
        } catch (const std::exception &error) {
            m_logger->warn("Failed to restore assistant profile from '{}': {}. Using '{}'.",
                           m_config.assistant_profile_state_file.string(),
                           error.what(),
                           to_string(selected_profile));
        }
    }

    if (selected_profile != m_active_context.profile) {
        if (selected_profile != m_inactive_context.profile) {
            throw std::runtime_error{std::format("Assistant profile '{}' was not loaded",
                                                 to_string(selected_profile))};
        }

        std::swap(m_active_context, m_inactive_context);
    }

    save_active_profile();
}

void LlamaEngine::save_active_profile() const {
    const auto output = nlohmann::json{
            {"version", 1},
            {"profile", std::string{to_string(m_active_context.profile)}},
    };

    util::write_text_file_atomic(
            m_config.assistant_profile_state_file,
            output.dump(2, ' ', false));
}

void LlamaEngine::finalize_interrupted_history_entries(assistant_context_s &context) {
    auto changed = false;
    auto interrupted_count = std::size_t{};

    for (auto &entry : context.history) {
        if (entry.status != chat_message_status_e::pending) {
            continue;
        }

        entry.status = chat_message_status_e::failed;
        changed = true;
        ++interrupted_count;
    }

    if (!changed) {
        return;
    }

    save_chat_history(context.history_file, context.history);

    context.history_logger->warn("{} interrupted pending chat entries were marked as failed",
                                 interrupted_count);
}

void LlamaEngine::rebuild_last_topic_anchor(assistant_context_s &context) {
    context.last_topic_anchor_ids.clear();
    context.context_state.reset();

    const auto find_entry = [&](const std::uint64_t id) -> const chat_history_entry_s * {
        const auto it = std::ranges::find(context.history, id, &chat_history_entry_s::id);
        return it == context.history.end() ? nullptr : &*it;
    };

    for (auto index = context.history.size(); index > 0; --index) {
        const auto &entry = context.history[index - 1];

        if (!is_completed_assistant_entry(entry)) {
            continue;
        }

        context.last_topic_anchor_ids = {entry.id};

        if (entry.context_state.has_value()) {
            context.context_state = entry.context_state;

            if (context.context_state->source_files.empty()) {
                context.context_state->source_files = entry.source_files;
            }

            limit_source_files(context.context_state->source_files, m_config.max_context_source_files);
            return;
        }

        auto state = chat_context_state_s{};
        state.source_files = entry.source_files;
        limit_source_files(state.source_files, m_config.max_context_source_files);

        if (entry.assistant.has_value()) {
            state.last_answer_excerpt = make_answer_excerpt(entry.assistant->model_content,
                                                             m_config.max_context_answer_excerpt_chars);
        }

        auto related_user_messages = std::vector<std::string>{};

        for (const auto relative_id : entry.relatives) {
            const auto *relative = find_entry(relative_id);

            if (relative == nullptr || !relative->user.has_value() ||
                util::is_blank(relative->user->model_content)) {
                continue;
            }

            related_user_messages.push_back(truncate_utf8(compact_whitespace(relative->user->model_content),
                                                           m_config.max_context_chars_per_user_message));
        }

        if (related_user_messages.empty()) {
            for (auto previous = index - 1; previous > 0; --previous) {
                const auto &candidate = context.history[previous - 1];

                if (candidate.status != chat_message_status_e::completed || !candidate.user.has_value() ||
                    util::is_blank(candidate.user->model_content)) {
                    continue;
                }

                related_user_messages.push_back(truncate_utf8(compact_whitespace(candidate.user->model_content),
                                                               m_config.max_context_chars_per_user_message));
                break;
            }
        }

        if (!related_user_messages.empty()) {
            state.topic = related_user_messages.front();

            if (related_user_messages.size() > 1 && m_config.max_context_user_messages != 0) {
                const auto first_recent = related_user_messages.size() > m_config.max_context_user_messages
                                                  ? related_user_messages.size() - m_config.max_context_user_messages
                                                  : std::size_t{1};

                state.recent_user_messages.assign(
                        related_user_messages.begin() + static_cast<std::ptrdiff_t>(first_recent),
                        related_user_messages.end());
            }
        }

        context.context_state = std::move(state);
        return;
    }
}

bool LlamaEngine::contexts_loaded() const noexcept {
    return m_active_context.loaded && m_inactive_context.loaded;
}

assistant_context_s &LlamaEngine::active_context() noexcept { return m_active_context; }

const assistant_context_s &LlamaEngine::active_context() const noexcept { return m_active_context; }

const chat_history_entry_s *LlamaEngine::find_history_entry(const std::uint64_t id) const noexcept {
    const auto it = std::ranges::find_if(m_active_context.history,
                                         [id](const chat_history_entry_s &entry) noexcept { return entry.id == id; });

    if (it == m_active_context.history.end()) {
        return nullptr;
    }

    return &*it;
}

std::size_t LlamaEngine::append_pending_user_entry(const std::string_view user_text) {
    const auto id = make_next_chat_entry_id(m_active_context.history);

    m_active_context.history.push_back(chat_history_entry_s{
            .id = id,

            .answer_kind = chat_answer_kind_e::unknown,

            .status = chat_message_status_e::pending,

            .user =
                    chat_visible_message_s{
                            .content = std::string{user_text},

                            .created_at = util::make_local_timestamp(),

                            .model_content = make_chat_model_content(user_text, chat_role_e::user),

                            .name = "Я",
                    },

            .assistant = std::nullopt,
            .source_files = {},
            .relatives = {},
    });

    return m_active_context.history.size() - 1;
}

std::string LlamaEngine::build_system_prompt(const std::span<const retrieved_knowledge_s> knowledge) const {
    const auto role_str = to_string(m_active_context.workplace_role);
    const auto glossary_mode = has_glossary_knowledge(knowledge);

    auto prompt = std::format(
            "Ты — AI-помощник стажёра ({}). Отвечай по-русски, просто, кратко и по рабочей теме.\n"
            "Не выдумывай внутренние правила, шаги, скидки, возвраты или компенсации. "
            "Не повторяй одну рекомендацию разными словами и не добавляй лишний вариант только ради длины ответа. "
            "Не предлагай поисковые запросы, названия файлов или способы поиска. "
            "Не добавляй блок источников: приложение добавит его само.\n"
            "Если точного регламента нет, прямо скажи об этом и отдельно дай осторожную общую рекомендацию. "
            "На вопрос вне рабочих задач ответь: \"Бот помогает только по рабочим вопросам.\".\n",
            role_str);

    if (knowledge.empty()) {
        prompt += "Для этого запроса подходящий фрагмент базы знаний не найден. Используй только общие знания и не "
                  "выдавай их за внутренний регламент.";
        return prompt;
    }

    if (glossary_mode) {
        prompt += "Режим словаря: если контекст содержит определение термина, дай именно определение, а не "
                  "пошаговую инструкцию.\n";
    } else {
        prompt += "Главный источник ответа — <knowledge_base>. Сформируй готовый ответ стажёру по текущему вопросу.\n";
    }

    prompt += build_knowledge_base_block(knowledge, m_config.max_prompt_knowledge_chars_per_document);

    return prompt;
}

std::string LlamaEngine::build_texting_adaptation_system_prompt(
        const std::span<const retrieved_knowledge_s> knowledge) const {
    assert(!knowledge.empty());
    assert(knowledge.front().kind == knowledge_document_kind_e::texting_script);

    auto prompt = std::string{R"PROMPT(Ты выполняешь короткий анализ и только точечное редактирование готового корпоративного скрипта. Не переписывай скрипт целиком и не составляй новый ответ.

Верни строго один JSON-объект без Markdown и пояснений:
{"issue":{"specified":false,"reason_clause":"","photo":""},"edits":[]}

Правила:
1. primary_script — утверждённый ответ организации. Всё, чего нет в edits, должно остаться побайтно неизменным.
2. issue.specified=true только если клиент прямо назвал конкретную причину или наблюдаемый дефект: например, отвалились ногти, покрытие отслоилось, появился скол, не нравится форма или появилась боль. Общее «не довольна» или «не понравилось» — это specified=false.
3. При specified=true запиши в reason_clause короткую грамматически готовую часть после слов «Нам очень жаль, что»: например, «уже на следующий день после маникюра отвалились три ногтя». В photo укажи конкретный объект, начиная со слова «фотографию»: например, «фотографию повреждённых ногтей».
4. При specified=false reason_clause и photo должны быть пустыми.
5. edits содержит только минимальные точечные изменения. target должен быть дословным непрерывным фрагментом primary_script.
6. Если клиент выразил общее недовольство конкретной услугой, не удаляй вопрос. Только уточни услугу: «что именно Вас не устроило?» -> «что именно Вас не устроило в маникюре?».
7. Если issue.specified=true, не добавляй в edits общую реакцию, вопрос и объект фотографии: приложение само надёжно заменит общую реакцию, удалит повторный вопрос и уточнит фотографию по полям issue.
8. Сохраняй явно указанные дату, период, услугу и мастера. Например, «на конец месяца» можно адаптировать точечной заменой соответствующего фрагмента.
9. Не выдумывай возврат, компенсацию, гарантию, скидку, цену, свободное время, причину проблемы или выполненное действие.
10. Не добавляй приветствие: приложение добавит его отдельно, если это нужно.

Пример общего недовольства:
Клиент: «Я не осталась на 100% довольна результатом маникюра»
Ответ: {"issue":{"specified":false,"reason_clause":"","photo":""},"edits":[{"target":"что именно Вас не устроило?","replacement":"что именно Вас не устроило в маникюре?"}]}

Пример конкретного дефекта:
Клиент: «Вчера сделала маникюр, сегодня уже три ногтя отвалились»
Ответ: {"issue":{"specified":true,"reason_clause":"уже на следующий день после маникюра отвалились три ногтя","photo":"фотографию повреждённых ногтей"},"edits":[]}

Пример ограничения по времени:
Клиент: «Запишите меня на конец месяца»
Ответ: {"issue":{"specified":false,"reason_clause":"","photo":""},"edits":[{"target":"Ближайшее доступное время — <дата и время>","replacement":"Ближайшее доступное время в конце месяца — <дата и время>"}]}
)PROMPT"};

    const auto &primary = knowledge.front();
    const auto primary_scenario = primary.tag_name.empty() ? primary.title : primary.tag_name;
    prompt += std::format("<primary_script name=\"{}\" scenario=\"{}\">\n{}\n</primary_script>\n",
                          primary.filename,
                          primary_scenario,
                          truncate_utf8(primary.content,
                                        m_config.max_texting_adaptation_chars_per_script));

    for (const auto &supporting : knowledge | std::views::drop(1)) {
        const auto scenario = supporting.tag_name.empty() ? supporting.title : supporting.tag_name;
        prompt += std::format("<supporting_script name=\"{}\" scenario=\"{}\">\n{}\n</supporting_script>\n",
                              supporting.filename,
                              scenario,
                              truncate_utf8(supporting.content,
                                            m_config.max_texting_adaptation_chars_per_script));
    }

    return prompt;
}

std::string LlamaEngine::build_texting_system_prompt(
        const std::span<const retrieved_knowledge_s> knowledge) const {
    auto prompt = std::string{
            "Составь готовое сообщение клиенту от лица организации. Верни только текст сообщения на русском "
            "языке: без анализа, заголовка, пояснений, JSON, названий файлов и источников. Этот режим используется "
            "только когда подходящий готовый корпоративный скрипт не найден. Обычно используй 3–7 содержательных "
            "предложений в 1–3 коротких абзацах.\n"
            "Учти факты и эмоцию клиента, ответь на явный вопрос или требование и обозначь следующий шаг. Не проси "
            "повторить уже указанную информацию. Не спорь и не выдумывай цены, свободное время, скидки, возвраты, "
            "компенсации, гарантии, сроки, причины, правила или уже выполненные действия.\n"};

    switch (m_config.texting_style) {
        case texting_style_e::formal:
            prompt +=
                    "Стиль: формальный и сдержанный, уважительное «Вы», точные деловые формулировки, без эмодзи, "
                    "сленга и лишних восклицаний.\n";
            break;
        case texting_style_e::neutral:
            prompt +=
                    "Стиль: нейтральный, спокойный и доброжелательный. Допустим максимум один уместный эмодзи.\n";
            break;
        case texting_style_e::friendly:
            prompt +=
                    "Стиль: тёплый, дружелюбный и заботливый, но профессиональный. Иногда используй 1–3 "
                    "уместных эмодзи; в жалобах выбирай спокойные эмодзи либо не используй их.\n";
            break;
        case texting_style_e::any: std::unreachable();
    }

    if (!knowledge.empty()) {
        prompt += "Используй предоставленные материалы как деловые ограничения и источник фактов.\n";
        prompt += build_knowledge_base_block(knowledge, m_config.max_texting_prompt_chars_per_script);
    }

    return prompt;
}

std::string LlamaEngine::build_knowledge_base_block(
        const std::span<const retrieved_knowledge_s> knowledge,
        const std::size_t max_chars_per_document) const {
    assert(max_chars_per_document > 0);

    auto prompt = std::string{"<knowledge_base>\n"};

    for (const auto &item : knowledge) {
        const auto content = truncate_utf8(item.content, max_chars_per_document);

        const auto scenario = item.tag_name.empty() ? item.title : item.tag_name;

        if (item.kind == knowledge_document_kind_e::texting_script) {
            prompt += std::format("<script name=\"{}\" scenario=\"{}\">\n{}\n</script>\n",
                                  item.filename,
                                  scenario,
                                  content);
        } else if (item.kind == knowledge_document_kind_e::texting_structure) {
            prompt += std::format("<response_structure name=\"{}\" scenario=\"{}\">\n{}\n</response_structure>\n",
                                  item.filename,
                                  scenario,
                                  content);
        } else if (is_glossary_match(item.match)) {
            prompt += std::format("<glossary name=\"{}\" term=\"{}\">\n{}\n</glossary>\n",
                                  item.filename,
                                  item.title,
                                  content);
        } else if (!item.tag_name.empty()) {
            prompt += std::format("<doc name=\"{}\" section=\"{}\">\n{}\n</doc>\n",
                                  item.filename,
                                  item.tag_name,
                                  content);
        } else {
            prompt += std::format("<doc name=\"{}\">\n{}\n</doc>\n", item.filename, content);
        }
    }

    prompt += "</knowledge_base>";

    return prompt;
}

std::string LlamaEngine::build_context_state_prompt() const {
    assert(m_active_context.context_state.has_value());

    if (!m_active_context.context_state.has_value()) {
        std::terminate();
    }

    const auto &state = *m_active_context.context_state;
    auto prompt = std::string{
            "Текущий запрос продолжает предыдущую тему. Используй компактное состояние ниже только для связи "
            "реплик; рабочие правила бери из <knowledge_base>, если он есть.\n<context_state>\n"};

    if (!state.topic.empty()) {
        prompt += std::format("Тема: {}\n", state.topic);
    }

    if (!state.recent_user_messages.empty()) {
        prompt += "Последние смысловые уточнения пользователя:\n";

        for (const auto &message : state.recent_user_messages) {
            prompt += std::format("- {}\n", message);
        }
    }

    if (!state.last_answer_excerpt.empty()) {
        prompt += std::format("Предыдущий ответ, сокращённо: {}\n", state.last_answer_excerpt);
    }

    prompt += "</context_state>\nОтветь именно на текущее уточнение. Не повторяй весь предыдущий ответ без необходимости.";

    return prompt;
}

std::string LlamaEngine::build_previous_answer_transform_prompt(
        const std::string_view user_text,
        const std::span<const retrieved_knowledge_s> knowledge) const {
    const auto previous_answer = previous_answer_for_transform();

    assert(!previous_answer.empty());

    if (previous_answer.empty()) {
        std::terminate();
    }

    const auto normalized_user_text = normalize_user_query_for_relation(user_text);
    const auto transform_kind = classify_previous_answer_transform_request(normalized_user_text);

    assert(transform_kind != previous_answer_transform_kind_e::none);

    auto prompt = std::string{};
    const auto texting = active_profile() == assistant_profile_e::texting;

    switch (transform_kind) {
        case previous_answer_transform_kind_e::concise:
            prompt = texting
                             ? "Сделай сообщение заметно короче. Сохрани уважительный тон, основную мысль и "
                               "предложенный следующий шаг. Не добавляй новые факты или обещания.\n"
                             : "Сократи предыдущий ответ заметно. Убери повторы, пояснения и второстепенные варианты. "
                               "Оставь только разные основные действия. Не добавляй новые факты, условия или рекомендации.\n";
            break;

        case previous_answer_transform_kind_e::expand:
            prompt = texting
                             ? "Сделай сообщение подробнее, но сохрани его как готовый ответ клиенту. Раскрывай только "
                               "мысли из предыдущего ответа и <knowledge_base>; не добавляй новые причины, обещания, "
                               "компенсации или правила.\n"
                             : "Раскрой предыдущий ответ заметно подробнее. Добавляй только конкретные действия и пояснения, "
                               "которые прямо следуют из <knowledge_base>. Не выдумывай внутренние правила. Если пользователь "
                               "одновременно просит «только основные действия», не сокращай ответ до двух фраз: сохрани только "
                               "основные шаги, но сделай каждый шаг конкретным — что сообщить, что согласовать, что изменить и "
                               "что подтвердить. Для рабочего сценария обычно дай 3–6 последовательных пунктов. Не повторяй одну "
                               "мысль разными словами.\n";
            break;

        case previous_answer_transform_kind_e::simplify:
            prompt = "Перепиши предыдущий ответ более простыми словами. Сохрани все полезные действия и исходный "
                     "смысл, но не добавляй новые факты, условия или рекомендации.\n";
            break;

        case previous_answer_transform_kind_e::restructure:
            prompt = "Измени только структуру предыдущего ответа в соответствии с запросом пользователя. Сохрани "
                     "содержание и не добавляй новые факты или правила.\n";
            break;

        case previous_answer_transform_kind_e::rewrite:
            prompt = "Переформулируй предыдущий ответ в соответствии с запросом пользователя. Сохрани исходный смысл "
                     "и не добавляй новые факты, условия, правила или рекомендации.\n";
            break;

        case previous_answer_transform_kind_e::formal:
            prompt = "Сделай предыдущий ответ более формальным и официальным. Убери разговорные обороты и лишние "
                     "эмодзи, но сохрани вежливость, смысл и все факты.\n";
            break;

        case previous_answer_transform_kind_e::strict:
            prompt = "Сделай предыдущий ответ строже и увереннее, но не грубо и не обвиняюще. Не добавляй угрозы, "
                     "санкции, правила или обещания, которых не было в исходном ответе.\n";
            break;

        case previous_answer_transform_kind_e::friendly:
            prompt = "Сделай предыдущий ответ теплее и дружелюбнее. Сохрани профессиональность, факты и смысл; не "
                     "добавляй лишних обещаний.\n";
            break;

        case previous_answer_transform_kind_e::more_emoji:
            prompt = "Добавь уместные эмодзи в предыдущий ответ. Не ставь эмодзи после каждого предложения и не "
                     "меняй факты, смысл или предложенный следующий шаг.\n";
            break;

        case previous_answer_transform_kind_e::fewer_emoji:
            prompt = "Убери большинство или все эмодзи из предыдущего ответа в соответствии с запросом пользователя. "
                     "Не меняй факты, смысл и тон сообщения.\n";
            break;

        case previous_answer_transform_kind_e::none:
            assert(false);
            std::terminate();
    }

    prompt += texting
                      ? "Верни только готовый текст сообщения клиенту, без заголовка и пояснений.\n"
                      : "Не добавляй блок источников: приложение добавит его само.\n";

    if (m_active_context.context_state.has_value() && !m_active_context.context_state->topic.empty()) {
        prompt += std::format("Тема диалога: {}\n", m_active_context.context_state->topic);
    }

    prompt += std::format("<previous_answer>\n{}\n</previous_answer>\n", previous_answer);

    if (transform_kind == previous_answer_transform_kind_e::expand) {
        if (knowledge.empty()) {
            prompt += "<knowledge_base>\n</knowledge_base>\n"
                      "Дополнительных материалов нет. В этом случае раскрой только уже названные действия и не "
                      "добавляй новые правила.\n";
        } else {
            prompt += build_knowledge_base_block(
                    knowledge,
                    m_config.max_expansion_knowledge_chars_per_document);
            prompt.push_back('\n');
        }
    }

    prompt += std::format("Точное задание пользователя: {}", user_text);

    return prompt;
}

std::string LlamaEngine::build_contextual_retrieval_query(const std::string_view user_text) const {
    if (!m_active_context.context_state.has_value()) {
        return std::string{user_text};
    }

    auto query = std::string{user_text};
    const auto &state = *m_active_context.context_state;

    if (!state.topic.empty()) {
        query += '\n';
        query += state.topic;
    }

    /*
     * recent_user_messages contains only semantic refinements. Formatting
     * commands such as "сделай кратко" are deliberately not stored here and
     * therefore cannot pollute the next retrieval query.
     */
    for (const auto &message : state.recent_user_messages) {
        query += '\n';
        query += message;
    }

    return truncate_utf8(query, m_config.max_contextual_retrieval_chars);
}

std::string LlamaEngine::previous_answer_for_transform() const {
    for (auto it = m_active_context.last_topic_anchor_ids.rbegin(); it != m_active_context.last_topic_anchor_ids.rend(); ++it) {
        const auto *entry = find_history_entry(*it);

        if (entry == nullptr || !is_completed_assistant_entry(*entry)) {
            continue;
        }

        const auto &answer = active_profile() == assistant_profile_e::texting
                                     ? entry->assistant->content
                                     : entry->assistant->model_content;

        return truncate_utf8(remove_generated_service_lines(answer), m_config.max_transform_answer_chars);
    }

    for (auto it = m_active_context.history.rbegin(); it != m_active_context.history.rend(); ++it) {
        if (!is_completed_assistant_entry(*it)) {
            continue;
        }

        const auto &answer = active_profile() == assistant_profile_e::texting
                                     ? it->assistant->content
                                     : it->assistant->model_content;

        return truncate_utf8(remove_generated_service_lines(answer), m_config.max_transform_answer_chars);
    }

    if (m_active_context.context_state.has_value()) {
        return truncate_utf8(m_active_context.context_state->last_answer_excerpt, m_config.max_transform_answer_chars);
    }

    return {};
}

chat_context_state_s LlamaEngine::make_context_state(
        const bool follow_up,
        const bool remember_user_message,
        const std::string_view user_text,
        const std::string_view answer_body,
        const std::span<const std::string> source_filenames,
        const std::span<const retrieved_knowledge_s> knowledge) const {
    auto state = follow_up && m_active_context.context_state.has_value() ? *m_active_context.context_state : chat_context_state_s{};
    const auto compact_user_text = truncate_utf8(make_chat_model_content(user_text, chat_role_e::user),
                                                 m_config.max_context_chars_per_user_message);

    const auto *split_knowledge =
            knowledge.size() == 1 && !knowledge.front().document_query.empty() &&
                            !knowledge.front().section_query.empty()
                    ? &knowledge.front()
                    : nullptr;

    const auto compact_document_query = split_knowledge == nullptr
                                                ? std::string{}
                                                : truncate_utf8(split_knowledge->document_query,
                                                                m_config.max_context_chars_per_user_message);
    const auto compact_section_query = split_knowledge == nullptr
                                               ? std::string{}
                                               : truncate_utf8(split_knowledge->section_query,
                                                               m_config.max_context_chars_per_user_message);

    if (!follow_up || state.topic.empty()) {
        state.topic = compact_document_query.empty() ? compact_user_text : compact_document_query;
        state.recent_user_messages.clear();

        if (!compact_section_query.empty() && m_config.max_context_user_messages != 0) {
            state.recent_user_messages.push_back(compact_section_query);
        }

        state.source_files.clear();
    } else if (remember_user_message && m_config.max_context_user_messages != 0) {
        const auto &message = compact_section_query.empty() ? compact_user_text : compact_section_query;

        if (!message.empty()) {
            state.recent_user_messages.push_back(message);
        }

        if (state.recent_user_messages.size() > m_config.max_context_user_messages) {
            state.recent_user_messages.erase(
                    state.recent_user_messages.begin(),
                    state.recent_user_messages.end() -
                            static_cast<std::ptrdiff_t>(m_config.max_context_user_messages));
        }
    }

    state.last_answer_excerpt = make_answer_excerpt(
            make_chat_model_content(answer_body, chat_role_e::assistant),
            m_config.max_context_answer_excerpt_chars);

    if (!source_filenames.empty()) {
        state.source_files.clear();
        append_unique_source_files(state.source_files, source_filenames);
        limit_source_files(state.source_files, m_config.max_context_source_files);
    } else if (!follow_up) {
        state.source_files.clear();
    }

    return state;
}

std::vector<chat_message_s> LlamaEngine::build_request_messages(
        const chat_history_entry_s &current_user_entry,
        const std::span<const retrieved_knowledge_s> knowledge,
        const bool transform_previous_answer) const {
    if (!current_user_entry.user.has_value()) {
        assert(false);
        std::terminate();
    }

    const auto texting = active_profile() == assistant_profile_e::texting;
    const auto &user_message = *current_user_entry.user;
    const auto &user_content = texting ? user_message.content : user_message.model_content;

    auto messages = std::vector<chat_message_s>{};

    const auto transform_kind = transform_previous_answer
                                        ? classify_previous_answer_transform_request(
                                                  normalize_user_query_for_relation(user_content))
                                        : previous_answer_transform_kind_e::none;

    auto system_prompt = std::string{};

    if (texting) {
        system_prompt = transform_previous_answer
                                ? "Ты редактируешь предыдущий готовый ответ клиенту. Выполни только указанное "
                                  "пользователем преобразование. Не добавляй новые факты, обещания, скидки, "
                                  "компенсации или внутренние правила. Верни только готовый текст сообщения клиенту."
                                : build_texting_system_prompt(knowledge);
    } else if (transform_kind == previous_answer_transform_kind_e::expand) {
        system_prompt = "Ты дополняешь предыдущий ответ AI-помощника стажёра. Используй только предыдущий "
                        "ответ и предоставленную базу знаний. Дай более подробный, конкретный и полезный "
                        "ответ по-русски, не выдумывая внутренние правила.";
    } else if (transform_previous_answer) {
        system_prompt = "Ты редактируешь предыдущий ответ AI-помощника стажёра. Выполни только "
                        "указанное пользователем преобразование и не добавляй новое содержание. "
                        "Отвечай по-русски.";
    } else {
        system_prompt = build_system_prompt(knowledge);
    }

    messages.push_back(chat_message_s{
            .role = chat_role_e::system,
            .name = "system",
            .content = std::move(system_prompt),
            .created_at = util::make_local_timestamp(),
            .source_files = {},
    });

    if (transform_previous_answer) {
        messages.push_back(chat_message_s{
                .role = chat_role_e::system,
                .name = "previous_answer_transform",
                .content = build_previous_answer_transform_prompt(user_content, knowledge),
                .created_at = util::make_local_timestamp(),
                .source_files = {},
        });
    } else if (!texting && !current_user_entry.relatives.empty() && m_active_context.context_state.has_value()) {
        messages.push_back(chat_message_s{
                .role = chat_role_e::system,
                .name = "context_state",
                .content = build_context_state_prompt(),
                .created_at = util::make_local_timestamp(),
                .source_files = {},
        });
    }

    if (!texting && !transform_previous_answer && looks_like_errors_or_risks_request(user_message.model_content)) {
        messages.push_back(chat_message_s{
                .role = chat_role_e::system,
                .name = "errors_and_risks_mode",
                .content = "Пользователь спрашивает об ошибках или рисках. Дай до трёх конкретных пунктов в формате "
                           "«Ошибка — как правильно». Называй наблюдаемое неправильное действие, а не абстрактную "
                           "фразу вроде «неправильное согласование» или «неправильное подтверждение». Пиши простыми "
                           "грамматически естественными фразами. Не выдумывай внутренние правила. Если в контексте "
                           "нет точного перечня ошибок, прямо скажи об этом и дай осторожные общие проверки.",
                .created_at = util::make_local_timestamp(),
                .source_files = {},
        });
    }

    messages.push_back(chat_message_s{
            .role = chat_role_e::user,
            .name = user_message.name,
            .content = user_content,
            .created_at = user_message.created_at,
            .source_files = {},
    });

    return messages;
}

std::vector<std::string> LlamaEngine::make_context_source_filenames(
        const std::span<const retrieved_knowledge_s> knowledge) const {
    auto result = make_source_filenames(knowledge);
    limit_source_files(result, m_config.max_context_source_files);
    return result;
}

std::vector<std::string> LlamaEngine::make_source_filenames_from_relatives(
        const std::span<const std::uint64_t> relatives) const {
    auto result = std::vector<std::string>{};

    for (const auto relative_id : relatives) {
        const auto *entry = find_history_entry(relative_id);

        if (entry == nullptr) {
            continue;
        }

        append_unique_source_files(result, entry->source_files);
    }

    return result;
}

std::vector<std::string> LlamaEngine::make_source_filenames(const std::span<const retrieved_knowledge_s> knowledge) {
    auto result = std::vector<std::string>{};

    auto seen = std::unordered_set<std::string>{};

    result.reserve(knowledge.size());

    for (const auto &item : knowledge) {
        if (seen.insert(item.filename).second) {
            result.push_back(item.filename);
        }
    }

    return result;
}

std::string LlamaEngine::ensure_sources_block(std::string answer, const std::span<const std::string> source_filenames) {
    auto normalized = remove_generated_service_lines(answer);

    if (normalized.empty()) {
        normalized = "Не удалось получить содержательный ответ от модели.";
    }

    auto result = std::string{};

    result += normalized;

    result += "\n\n---\n\n";

    result += std::format("**Источники:** {}", join_source_filenames(source_filenames));

    return result;
}

bool LlamaEngine::can_answer_without_llm(const std::span<const retrieved_knowledge_s> knowledge) noexcept {
    return is_direct_knowledge_answer_candidate(knowledge) ||
           is_query_matched_document_tag_candidate(knowledge);
}

std::string LlamaEngine::make_direct_answer(const std::span<const retrieved_knowledge_s> knowledge) {
    assert(can_answer_without_llm(knowledge));

    if (!can_answer_without_llm(knowledge)) {
        std::terminate();
    }

    if (is_query_matched_document_tag_candidate(knowledge)) {
        auto content = knowledge.front().direct_content;
        util::trim(content);
        return remove_direct_answer_search_hints(content);
    }

    if (is_glossary_match(knowledge.front().match)) {
        auto content = knowledge.front().content;
        util::trim(content);
        return content;
    }

    return remove_direct_answer_search_hints(extract_direct_answer_section(knowledge.front().content));
}

} // namespace stz::intern
