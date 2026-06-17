#include "engine.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <exception>
#include <format>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "util/string_helpers.hpp"
#include "util/time.hpp"

namespace stz::intern {

namespace {

[[nodiscard]] engine_config_s validate_engine_config(engine_config_s config) {
    if (config.history_file.empty()) {
        throw std::runtime_error{"Engine history file path is empty"};
    }

    if (config.knowledge_directory.empty()) {
        throw std::runtime_error{"Engine knowledge directory path is empty"};
    }

    if (config.max_knowledge_documents != 0 && config.max_knowledge_chars_per_document == 0) {
        throw std::runtime_error{"max_knowledge_chars_per_document must be positive "
                                 "when knowledge retrieval is enabled"};
    }

    return config;
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

[[nodiscard]] bool is_completed_model_visible_entry(const chat_history_entry_s &entry) noexcept {
    if (entry.status != chat_message_status_e::completed) {
        return false;
    }

    return entry.user.has_value() || entry.assistant.has_value();
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


constexpr auto max_topic_anchor_ids = std::size_t{4};

enum class chat_relation_kind_e {
    standalone,
    follow_up,
};

[[nodiscard]] std::string_view relation_kind_name(const chat_relation_kind_e relation) noexcept {
    switch (relation) {
        case chat_relation_kind_e::standalone: return "standalone";
        case chat_relation_kind_e::follow_up: return "follow_up";
    }

    return "standalone";
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
            codepoint = static_cast<char32_t>(((first & 0x07) << 18) | ((second & 0x3F) << 12) |
                                              ((third & 0x3F) << 6) | (fourth & 0x3F));
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

[[nodiscard]] bool contains_any(const std::string_view text,
                                const std::span<const std::string_view> fragments) noexcept {
    return std::ranges::any_of(fragments, [text](const std::string_view fragment) { return text.contains(fragment); });
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

[[nodiscard]] bool has_glossary_knowledge(const std::span<const retrieved_knowledge_s> knowledge) noexcept {
    return std::ranges::any_of(knowledge, [](const retrieved_knowledge_s &item) noexcept {
        return is_glossary_match(item.match);
    });
}

[[nodiscard]] bool looks_like_glossary_question(const std::string_view user_text) {
    const auto normalized_text = normalize_user_query_for_relation(user_text);

    if (normalized_text.empty()) {
        return false;
    }

    constexpr auto definition_prefixes = std::array{
            std::string_view{"что такое "},
            std::string_view{"что это "},
            std::string_view{"что за "},
            std::string_view{"что значит "},
            std::string_view{"что означает "},
            std::string_view{"объясни что такое "},
            std::string_view{"объясни термин "},
            std::string_view{"поясни термин "},
            std::string_view{"дай определение "},
    };

    if (starts_with_any(normalized_text, std::span{definition_prefixes})) {
        return true;
    }

    constexpr auto definition_fragments = std::array{
            std::string_view{" что такое "},
            std::string_view{" что это "},
            std::string_view{" что за "},
            std::string_view{" что значит "},
            std::string_view{" что означает "},
            std::string_view{" определение "},
            std::string_view{" термин "},
    };

    if (contains_any(normalized_text, std::span{definition_fragments})) {
        return true;
    }

    return normalized_text == "что такое" || normalized_text == "определение" || normalized_text == "термин";
}

void append_unique_knowledge(std::vector<retrieved_knowledge_s> &target,
                             const std::span<const retrieved_knowledge_s> source) {
    auto seen = std::unordered_set<std::string>{};
    seen.reserve(target.size() + source.size());

    for (const auto &item : target) {
        seen.insert(item.filename);
    }

    for (const auto &item : source) {
        if (seen.insert(item.filename).second) {
            target.push_back(item);
        }
    }
}

[[nodiscard]] std::vector<retrieved_knowledge_s> merge_contextual_knowledge(
        const std::span<const retrieved_knowledge_s> inherited_knowledge,
        const std::span<const retrieved_knowledge_s> current_knowledge) {
    auto result = std::vector<retrieved_knowledge_s>{};
    result.reserve(inherited_knowledge.size() + current_knowledge.size());

    append_unique_knowledge(result, inherited_knowledge);
    append_unique_knowledge(result, current_knowledge);

    return result;
}

[[nodiscard]] bool looks_like_explicit_follow_up(const std::string_view normalized_text) noexcept {
    constexpr auto follow_up_prefixes = std::array{
            std::string_view{"а "},
            std::string_view{"и "},
            std::string_view{"а если"},
            std::string_view{"а что если"},
            std::string_view{"а как"},
            std::string_view{"а можно"},
            std::string_view{"а надо"},
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
    };

    if (starts_with_any(normalized_text, std::span{follow_up_prefixes})) {
        return true;
    }

    constexpr auto follow_up_fragments = std::array{
            std::string_view{"объясни подробнее"},
            std::string_view{"распиши подробнее"},
            std::string_view{"подробнее"},
            std::string_view{"продолжи"},
            std::string_view{"приведи пример"},
            std::string_view{"конкретные шаги"},
            std::string_view{"какие конкретные шаги"},
            std::string_view{"для этого случая"},
            std::string_view{"в этом случае"},
            std::string_view{"какой таблицей"},
            std::string_view{"какая таблица"},
            std::string_view{"какой файл"},
            std::string_view{"какого формата"},
            std::string_view{"какой формат"},
            std::string_view{"какой шаблон"},
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
            std::string_view{"коротко"},
            std::string_view{"покороче"},
            std::string_view{"короче"},
            std::string_view{"сократи"},
            std::string_view{"сокращенно"},
            std::string_view{"сокращённо"},
            std::string_view{"сделай кратко"},
            std::string_view{"сделай коротко"},
            std::string_view{"сделай короче"},
            std::string_view{"без подробностей"},
            std::string_view{"в двух словах"},
            std::string_view{"одним предложением"},
            std::string_view{"переформулируй"},
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
            std::string_view{"верно ли я понимаю"},
            std::string_view{"я правильно понял"},
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
    };

    constexpr auto exact_follow_up_requests = std::array{
            std::string_view{"кратко"},
            std::string_view{"коротко"},
            std::string_view{"покороче"},
            std::string_view{"короче"},
            std::string_view{"подробнее"},
            std::string_view{"конкретные шаги"},
            std::string_view{"какие конкретные шаги"},
            std::string_view{"какой файл"},
            std::string_view{"какая таблица"},
            std::string_view{"какой формат"},
            std::string_view{"какого формата"},
            std::string_view{"продолжи"},
            std::string_view{"сократи"},
            std::string_view{"переформулируй"},
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
    return std::ranges::any_of(terms, [expected](const std::string &term) noexcept {
        return term == expected;
    });
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
            std::string_view{"перед "},
            std::string_view{"до "},
            std::string_view{"после"},
            std::string_view{"сначала"},
            std::string_view{"потом"},
            std::string_view{"затем"},
            std::string_view{"или после"},
            std::string_view{"или до"},
            std::string_view{"или неважно"},
            std::string_view{"неважно"},
            std::string_view{"обязательно"},
            std::string_view{"должна"},
            std::string_view{"должен"},
            std::string_view{"должны"},
            std::string_view{"надо ли"},
            std::string_view{"нужно ли"},
            std::string_view{"можно ли"},
            std::string_view{"нужно сначала"},
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
        const std::span<const retrieved_knowledge_s> knowledge) {
    if (!has_previous_anchor) {
        return chat_relation_kind_e::standalone;
    }

    const auto normalized_text = normalize_user_query_for_relation(user_text);

    if (normalized_text.empty()) {
        return chat_relation_kind_e::standalone;
    }

    if (looks_like_explicit_follow_up(normalized_text)) {
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

LlamaEngine::LlamaEngine(engine_config_s config,
                         llm::llama_server_config_s server_config,
                         const std::shared_ptr<spdlog::logger> &logger)
    : m_config{validate_engine_config(std::move(config))},
      m_logger{clone_logger(logger, "LlamaEngine")},
      m_history_logger{m_logger->clone("ChatHistory")},
      m_server{
              std::move(server_config),
              m_logger->clone("LlamaServer"),
      },
      m_client{
              m_server,
              m_config.client,
              m_logger->clone("LlamaClient"),
      },
      m_knowledge{
              m_config.knowledge_directory,
              m_logger->clone("KnowledgeStorage"),
      } {}

void LlamaEngine::load() {
    assert(!m_loaded);

    if (m_loaded) {
        std::terminate();
    }

    load_history();
    finalize_interrupted_history_entries();

    m_knowledge.load();
    rebuild_last_topic_anchor();

    if (m_knowledge.empty()) {
        m_logger->warn("Knowledge storage is empty");
    }

    m_loaded = true;

    m_logger->info("LlamaEngine loaded");
    m_logger->info("Workplace role: {}", to_string(m_config.workplace_role));
    m_logger->info("Chat history entries: {}", m_history.size());
    m_logger->info("Model relatives restored: {}", m_last_topic_anchor_ids.size());
}

void LlamaEngine::start() {
    require_loaded(m_loaded);

    assert(!m_server.is_running());

    if (m_server.is_running()) {
        std::terminate();
    }

    m_logger->info("Starting llama-server");

    m_server.start();

    m_logger->info("llama-server started: {}", m_server.url());
}

void LlamaEngine::stop() noexcept {
    m_server.stop_generating();
    m_server.stop();
}

engine_answer_s LlamaEngine::ask(const std::string_view user_text,
                                 const llm::llama_stream_callback_t &stream_callback) {
    require_loaded(m_loaded);

    if (util::is_blank(user_text)) {
        throw std::runtime_error{"User message is empty"};
    }

    const auto user_index = append_pending_user_entry(user_text);

    save_history();

    const auto retrieve_options = knowledge_retrieve_options_s{
            .workplace_role = m_config.workplace_role,

            .include_general = true,
            .include_custom = true,

            .limit = m_config.max_knowledge_documents,

            .max_chars_per_document = m_config.max_knowledge_chars_per_document,

            .min_ranked_score = m_config.min_ranked_knowledge_score,
    };

    const auto glossary_knowledge = looks_like_glossary_question(user_text)
                                    ? m_knowledge.retrieve_glossary(user_text, retrieve_options)
                                    : std::vector<retrieved_knowledge_s>{};

    const auto primary_knowledge = !glossary_knowledge.empty() ? glossary_knowledge
                                                               : m_knowledge.retrieve(user_text, retrieve_options);

    for (const auto &item : primary_knowledge) {
        m_logger->info("Retrieved knowledge: {} "
                       "score={} role={} source={} match={}",
                       item.filename,
                       item.score,
                       to_string(item.role),
                       to_string(item.source),
                       to_string(item.match));
    }

    const auto relation = classify_relation_to_previous_answer(
            user_text,
            !m_last_topic_anchor_ids.empty(),
            primary_knowledge);

    auto knowledge = primary_knowledge;

    if (relation == chat_relation_kind_e::follow_up) {
        const auto inherited_source_filenames = make_source_filenames_from_relatives(m_last_topic_anchor_ids);

        const auto inherited_knowledge = m_knowledge.retrieve_by_filenames(inherited_source_filenames, retrieve_options);

        knowledge = merge_contextual_knowledge(inherited_knowledge, primary_knowledge);

        for (const auto &item : inherited_knowledge) {
            m_logger->info("Inherited contextual knowledge: {} "
                           "score={} role={} source={} match={}",
                           item.filename,
                           item.score,
                           to_string(item.role),
                           to_string(item.source),
                           to_string(item.match));
        }
    }

    /*
     * Восстановленная рабочая логика.
     *
     * Если KnowledgeStorage вернул ровно один файл
     * с прямым совпадением по частым запросам
     * или glossary-заголовку второго уровня,
     * ответ извлекается непосредственно из Markdown.
     *
     * llama-server и LlamaClient в этой ветке
     * вообще не используются.
     */
    if (relation == chat_relation_kind_e::standalone && can_answer_without_llm(knowledge)) {
        auto source_filenames = make_source_filenames(knowledge);

        auto answer_body = make_direct_answer(knowledge);

        auto answer = ensure_sources_block(answer_body, source_filenames);

        const auto user_id = m_history[user_index].id;

        m_history[user_index].answer_kind = chat_answer_kind_e::direct_knowledge;

        m_history[user_index].status = chat_message_status_e::completed;

        m_history[user_index].relatives.clear();

        auto assistant_relatives = std::vector<std::uint64_t>{
                user_id,
        };

        const auto assistant_id = make_next_chat_entry_id(m_history);

        m_history.push_back(chat_history_entry_s{
                .id = assistant_id,

                .answer_kind = chat_answer_kind_e::direct_knowledge,

                .status = chat_message_status_e::completed,

                .user = std::nullopt,

                .assistant =
                        chat_visible_message_s{
                                .content = answer,
                                .created_at = util::make_local_timestamp(),
                                .model_content = answer_body,
                                .name = "AI-бот",
                        },

                .source_files = source_filenames,

                .relatives = assistant_relatives,
        });

        m_last_topic_anchor_ids = std::move(assistant_relatives);

        m_last_topic_anchor_ids.push_back(assistant_id);

        save_history();

        m_logger->info("Answered without LLM by direct knowledge match: "
                       "file={} score={} match={}",
                       knowledge.front().filename,
                       knowledge.front().score,
                       to_string(knowledge.front().match));

        return engine_answer_s{
                .status = chat_message_status_e::completed,

                .content = std::move(answer),
        };
    }

    /*
     * До этой точки прямого совпадения с готовым
     * Markdown-скриптом не найдено.
     *
     * Только теперь требуется llama-server.
     */
    if (!m_server.is_running()) {
        m_history[user_index].status = chat_message_status_e::failed;

        save_history();

        throw std::runtime_error{"Cannot generate answer: "
                                 "llama-server is not ready"};
    }

    m_history[user_index].answer_kind = chat_answer_kind_e::llm;

    if (relation == chat_relation_kind_e::follow_up) {
        m_history[user_index].relatives = m_last_topic_anchor_ids;
    } else {
        m_history[user_index].relatives.clear();
    }

    m_logger->info("LLM request relation: {} previous_anchor_ids={} relatives_used={}",
                   relation_kind_name(relation),
                   m_last_topic_anchor_ids.size(),
                   m_history[user_index].relatives.size());

    save_history();

    auto response = llm::llama_client_response_s{};

    try {
        const auto request_messages = build_request_messages(m_history[user_index], knowledge);

        response = m_client.complete_chat(request_messages, stream_callback);
    } catch (...) {
        m_history[user_index].status = chat_message_status_e::failed;

        save_history();

        throw;
    }

    if (response.status == llm::llama_completion_status_e::cancelled) {
        m_history[user_index].status = chat_message_status_e::cancelled;

        save_history();

        m_logger->info("Answer generation was cancelled");

        return engine_answer_s{
                .status = chat_message_status_e::cancelled,

                .content = std::move(response.content),
        };
    }

    auto source_filenames = make_context_source_filenames(knowledge, m_history[user_index].relatives);

    auto answer_body = remove_generated_service_lines(response.content);

    if (answer_body.empty()) {
        answer_body = "Не удалось получить содержательный "
                      "ответ от модели.";
    }

    auto answer = ensure_sources_block(answer_body, source_filenames);

    const auto user_id = m_history[user_index].id;

    auto assistant_relatives = m_history[user_index].relatives;

    assistant_relatives.push_back(user_id);

    m_history[user_index].status = chat_message_status_e::completed;

    const auto assistant_id = make_next_chat_entry_id(m_history);

    m_history.push_back(chat_history_entry_s{
            .id = assistant_id,

            .answer_kind = chat_answer_kind_e::llm,

            .status = chat_message_status_e::completed,

            .user = std::nullopt,

            .assistant =
                    chat_visible_message_s{
                            .content = answer,
                            .created_at = util::make_local_timestamp(),
                            .model_content = answer_body,
                            .name = "AI-бот",
                    },

            .source_files = source_filenames,

            .relatives = assistant_relatives,
    });

    m_last_topic_anchor_ids = make_topic_anchor_ids(
            std::move(assistant_relatives),
            user_id,
            assistant_id);

    save_history();

    return engine_answer_s{
            .status = chat_message_status_e::completed,

            .content = std::move(answer),
    };
}

void LlamaEngine::stop_generating() noexcept { m_server.stop_generating(); }

bool LlamaEngine::is_running() const noexcept { return m_server.is_running(); }

bool LlamaEngine::model_generates() const noexcept { return m_server.model_generates(); }

bool LlamaEngine::change_model(const llm::model_e model) {
    require_loaded(m_loaded);

    return m_server.change_model(model);
}

void LlamaEngine::store_model_cache() {
    require_loaded(m_loaded);

    m_server.store_model_cache();
}

void LlamaEngine::load_model_cache() {
    require_loaded(m_loaded);

    m_server.load_model_cache();
}

std::string LlamaEngine::server_url() const { return m_server.url(); }

llm::llama_server_state_info_s LlamaEngine::server_state() const { return m_server.state_info(); }

std::span<const chat_history_entry_s> LlamaEngine::history() const noexcept {
    return {
            m_history.data(),
            m_history.size(),
    };
}

void LlamaEngine::clear_history() {
    require_loaded(m_loaded);

    if (m_server.model_generates()) {
        throw std::runtime_error{"Cannot clear chat history while "
                                 "the model is generating an answer"};
    }

    m_history.clear();
    m_last_topic_anchor_ids.clear();

    save_history();

    m_logger->info("Chat history and model context were cleared");
}

void LlamaEngine::load_history() { m_history = load_chat_history(m_config.history_file, m_history_logger); }

void LlamaEngine::save_history() const { save_chat_history(m_config.history_file, m_history); }

void LlamaEngine::finalize_interrupted_history_entries() {
    auto changed = false;
    auto interrupted_count = std::size_t{};

    for (auto &entry : m_history) {
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

    save_history();

    m_history_logger->warn("{} interrupted pending chat entries "
                           "were marked as failed",
                           interrupted_count);
}

void LlamaEngine::rebuild_last_topic_anchor() {
    m_last_topic_anchor_ids.clear();

    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (!is_completed_assistant_entry(*it)) {
            continue;
        }

        auto anchor_ids = it->relatives;
        anchor_ids.push_back(it->id);

        m_last_topic_anchor_ids = trim_topic_anchor_ids(
                std::move(anchor_ids));

        return;
    }
}

const chat_history_entry_s *LlamaEngine::find_history_entry(const std::uint64_t id) const noexcept {
    const auto it = std::ranges::find_if(m_history,
                                         [id](const chat_history_entry_s &entry) noexcept { return entry.id == id; });

    if (it == m_history.end()) {
        return nullptr;
    }

    return &*it;
}

std::size_t LlamaEngine::append_pending_user_entry(const std::string_view user_text) {
    const auto id = make_next_chat_entry_id(m_history);

    m_history.push_back(chat_history_entry_s{
            .id = id,

            .answer_kind = chat_answer_kind_e::unknown,

            .status = chat_message_status_e::pending,

            .user =
                    chat_visible_message_s{
                            .content = std::string{user_text},

                            .created_at = util::make_local_timestamp(),

                            .model_content = std::string{user_text},

                            .name = "Я",
                    },

            .assistant = std::nullopt,
            .source_files = {},
            .relatives = {},
    });

    return m_history.size() - 1;
}

std::string LlamaEngine::build_system_prompt(const std::span<const retrieved_knowledge_s> knowledge) const {
    const auto role_str = to_string(m_config.workplace_role);

    if (knowledge.empty()) {
        return std::format(
            "Роль: AI-помощник стажёра в сфере услуг({}).\n"
            "Язык: Русский (просто, понятно, лаконично).\n"
            "Правило: База знаний пуста. Сформируй ответ ИСКЛЮЧИТЕЛЬНО на основе своих общих знаний.\n"
            "Формат ответа: готовая инструкция для стажёра, а не подсказка для поиска.\n"
            "Запрещено: выдумывать регламенты компании, скидки, возвраты и компенсации.\n"
            "Запрещено: предлагать поисковые запросы, ключевые слова, имена файлов или способы искать информацию в базе знаний.\n"
            "Если вопрос не связан с работой стажёра — откажи: \"Бот помогает только по рабочим вопросам.\".",
            role_str
        );
    }

    if (has_glossary_knowledge(knowledge)) {
        auto prompt = std::format(
            "Роль: AI-помощник стажёра ({}).\n"
            "Язык: Русский (просто, понятно, лаконично).\n"
            "Режим: словарь терминов.\n"
            "Правила:\n"
            "1. Главный источник — контекст в <knowledge_base>.\n"
            "2. Если в контексте есть определение нужного термина, отвечай этим определением, не превращай ответ в пошаговую инструкцию.\n"
            "3. Не выдумывай определения, регламенты, правила, скидки, возвраты и компенсации.\n"
            "4. Не предлагай пользователю поисковые запросы, ключевые слова, имена файлов или способы искать информацию в базе знаний.\n"
            "5. Если прямого определения нет, честно скажи это и дай короткое осторожное объяснение отдельно.\n"
            "6. Если вопрос вне роли или контекста — откажи: \"Бот помогает только по рабочим вопросам.\".\n\n"
            "<knowledge_base>\n",
            role_str
        );

        for (const auto &item : knowledge) {
            prompt += std::format("<glossary name=\"{}\" term=\"{}\">\n{}\n</glossary>\n",
                                  item.filename,
                                  item.title,
                                  item.content);
        }

        prompt += "</knowledge_base>";

        return prompt;
    }

    auto prompt = std::format(
        "Роль: AI-помощник стажёра ({}).\n"
        "Язык: Русский (просто, понятно, лаконично).\n"
        "Правила:\n"
        "1. Отвечай строго по теме работы и обслуживания клиентов.\n"
        "2. Главный источник — контекст в <knowledge_base>. Формат ответа: готовая инструкция для стажёра.\n"
        "3. Категорически запрещено выдумывать: шаги, правила, скидки, возвраты, компенсации.\n"
        "4. Не предлагай пользователю поисковые запросы, ключевые слова, имена файлов или способы искать информацию в базе знаний.\n"
        "5. Если прямого регламента нет, честно скажи это и дай осторожную общую рекомендацию отдельно.\n"
        "6. Если вопрос вне роли или контекста — откажи: \"Бот помогает только по рабочим вопросам.\".\n\n"
        "<knowledge_base>\n",
        role_str
    );

    for (const auto &item : knowledge) {
        prompt += std::format("<doc name=\"{}\">\n{}\n</doc>\n", item.filename, item.content);
    }
    prompt += "</knowledge_base>";

    return prompt;
}

std::vector<chat_message_s> LlamaEngine::build_request_messages(
        const chat_history_entry_s &current_user_entry,
        const std::span<const retrieved_knowledge_s> knowledge) const {
    auto messages = std::vector<chat_message_s>{};

    messages.push_back(chat_message_s{
            .role = chat_role_e::system,
            .name = "system",

            .content = build_system_prompt(knowledge),

            .created_at = util::make_local_timestamp(),

            .source_files = {},
    });

    if (!current_user_entry.relatives.empty()) {
        messages.push_back(chat_message_s{
                .role = chat_role_e::system,
                .name = "follow_up_mode",

                .content =
                        "Текущий пользовательский запрос является уточнением к предыдущему ответу. "
                        "Обязательно используй предыдущие сообщения ниже как основной контекст. "
                        "Если пользователь просит «кратко», «коротко», «сократи», «подробнее», "
                        "«переформулируй», «составь определение», «что проверить», "
                        "«уточни порядок действий», «правильно ли я понимаю» или задаёт похожую короткую команду, "
                        "это не новая тема и не вопрос вне роли. "
                        "В таком случае измени или уточни именно предыдущий ответ: сократи, дополни, "
                        "сформулируй определение, составь чеклист, проверь понимание пользователя или уточни порядок действий. "
                        "Если пользователь спрашивает о порядке действий, например «до или после», отвечай как продолжение предыдущего сценария. "
                        "Если пользователь просит конкретные шаги для одного условия из предыдущей инструкции, не повторяй весь сценарий: "
                        "дай только шаги для этого условия. "
                        "Если вопрос короткий и неполный, восстанавливай смысл из предыдущего ответа и <knowledge_base>. "
                        "Не предлагай поисковые запросы, ключевые слова или имена файлов. "
                        "Не пиши блок источников: приложение добавит его автоматически.",

                .created_at = util::make_local_timestamp(),

                .source_files = {},
        });
    }

    for (const auto relative_id : current_user_entry.relatives) {
        const auto *entry = find_history_entry(relative_id);

        if (entry == nullptr || !is_completed_model_visible_entry(*entry)) {
            continue;
        }

        if (entry->user.has_value() && !util::is_blank(entry->user->model_content)) {
            messages.push_back(chat_message_s{
                    .role = chat_role_e::user,
                    .name = entry->user->name,
                    .content = entry->user->model_content,
                    .created_at = entry->user->created_at,
                    .source_files = entry->source_files,
            });

            continue;
        }

        if (entry->assistant.has_value() && !util::is_blank(entry->assistant->model_content)) {
            messages.push_back(chat_message_s{
                    .role = chat_role_e::assistant,

                    .name = entry->assistant->name,

                    .content = entry->assistant->model_content,

                    .created_at = entry->assistant->created_at,

                    .source_files = entry->source_files,
            });
        }
    }

    if (!current_user_entry.user.has_value()) {
        assert(false);
        std::terminate();
    }

    messages.push_back(chat_message_s{
            .role = chat_role_e::user,

            .name = current_user_entry.user->name,

            .content = current_user_entry.user->model_content,

            .created_at = current_user_entry.user->created_at,

            .source_files = {},
    });

    return messages;
}

std::vector<std::string> LlamaEngine::make_context_source_filenames(
        const std::span<const retrieved_knowledge_s> knowledge,
        const std::span<const std::uint64_t> relatives) const {
    auto result = make_source_filenames_from_relatives(relatives);
    auto retrieved_source_filenames = make_source_filenames(knowledge);

    append_unique_source_files(result, retrieved_source_filenames);

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
    /*
     * Полностью восстановлен прежний рабочий контракт:
     *
     * KnowledgeStorage должен вернуть один-единственный
     * документ с прямым совпадением.
     */
    if (knowledge.size() != 1) {
        return false;
    }

    const auto match = knowledge.front().match;

    return match == knowledge_match_e::exact_frequent_query || match == knowledge_match_e::unordered_frequent_query ||
           match == knowledge_match_e::unordered_fuzzy_frequent_query || is_glossary_match(match);
}

std::string LlamaEngine::make_direct_answer(const std::span<const retrieved_knowledge_s> knowledge) {
    assert(can_answer_without_llm(knowledge));

    if (!can_answer_without_llm(knowledge)) {
        std::terminate();
    }

    if (is_glossary_match(knowledge.front().match)) {
        auto content = knowledge.front().content;
        util::trim(content);
        return content;
    }

    return extract_direct_answer_section(knowledge.front().content);
}

} // namespace stz::intern