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

    if (config.max_knowledge_documents != 0 && config.max_prompt_knowledge_chars_per_document == 0) {
        throw std::runtime_error{"max_prompt_knowledge_chars_per_document must be positive "
                                 "when knowledge retrieval is enabled"};
    }

    if (config.max_knowledge_documents != 0 && config.max_expansion_knowledge_chars_per_document == 0) {
        throw std::runtime_error{"max_expansion_knowledge_chars_per_document must be positive "
                                 "when knowledge retrieval is enabled"};
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
            std::string_view{"коротко"},
            std::string_view{"покороче"},
            std::string_view{"короче"},
            std::string_view{"сократи"},
            std::string_view{"одним предложением"},
            std::string_view{"в двух словах"},
    };

    constexpr auto concise_fragments = std::array{
            std::string_view{"сделай кратко"},
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

    constexpr auto rewrite_fragments = std::array{
            std::string_view{"переформулируй"},
            std::string_view{"перефразируй"},
            std::string_view{"перепиши"},
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
            std::string_view{"сделай коротко"},
            std::string_view{"сделай короче"},
            std::string_view{"без подробностей"},
            std::string_view{"в двух словах"},
            std::string_view{"одним предложением"},
            std::string_view{"переформулируй"},
            std::string_view{"перефразируй"},
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
    m_logger->info("Compact model context restored: {}", m_context_state.has_value());
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
    const auto transform_request = !m_last_topic_anchor_ids.empty() &&
                                   transform_kind != previous_answer_transform_kind_e::none;

    const auto retrieve_primary_knowledge = [&](const std::string_view query,
                                                const bool prefer_documents) {
        if (prefer_documents) {
            auto documents = m_knowledge.retrieve(query, retrieve_options);

            if (!documents.empty()) {
                return documents;
            }

            return m_knowledge.retrieve_glossary(query, retrieve_options);
        }

        auto glossary = m_knowledge.retrieve_glossary(query, retrieve_options);
        auto documents = procedural_request || glossary.empty()
                                 ? m_knowledge.retrieve(query, retrieve_options)
                                 : std::vector<retrieved_knowledge_s>{};

        if (procedural_request && !documents.empty()) {
            return documents;
        }

        if (!glossary.empty()) {
            return glossary;
        }

        return documents;
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

    const auto inherited_source_filenames = m_context_state.has_value()
                                                    ? m_context_state->source_files
                                                    : make_source_filenames_from_relatives(m_last_topic_anchor_ids);
    const auto current_retrieval_returns_previous_source = knowledge_contains_any_source_filename(
            primary_knowledge,
            inherited_source_filenames);

    const auto relation = classify_relation_to_previous_answer(user_text,
                                                               !m_last_topic_anchor_ids.empty(),
                                                               transform_kind,
                                                               current_retrieval_returns_previous_source,
                                                               primary_knowledge);

    auto knowledge = primary_knowledge;

    if (relation == chat_relation_kind_e::follow_up) {
        const auto inherited_knowledge = m_knowledge.retrieve_by_filenames(inherited_source_filenames,
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
        knowledge = m_knowledge.retrieve_by_filenames(inherited_source_filenames,
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

        const auto user_id = m_history[user_index].id;

        m_history[user_index].answer_kind = chat_answer_kind_e::direct_knowledge;

        m_history[user_index].status = chat_message_status_e::completed;

        if (relation == chat_relation_kind_e::follow_up) {
            m_history[user_index].relatives = m_last_topic_anchor_ids;
        } else {
            m_history[user_index].relatives.clear();
        }

        auto assistant_relatives = m_history[user_index].relatives;
        assistant_relatives.push_back(user_id);

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
                                .model_content = make_chat_model_content(answer_body, chat_role_e::assistant),
                                .name = "AI-бот",
                        },

                .source_files = source_filenames,

                .relatives = assistant_relatives,

                .context_state = context_state,
        });

        m_context_state = std::move(context_state);
        m_last_topic_anchor_ids = make_topic_anchor_ids(std::move(assistant_relatives), user_id, assistant_id);

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
        m_history[user_index].status = chat_message_status_e::failed;

        save_history();

        throw std::runtime_error{"Cannot generate answer: "
                                 "llama-server is not ready"};
    }

    m_history[user_index].answer_kind = chat_answer_kind_e::llm;

    if (relation != chat_relation_kind_e::standalone) {
        m_history[user_index].relatives = m_last_topic_anchor_ids;
    } else {
        m_history[user_index].relatives.clear();
    }

    m_logger->info("LLM request relation: {} transform={} previous_anchor_ids={} relatives_used={} same_source={}",
                   relation_kind_name(relation),
                   transform_kind_name(transform_kind),
                   m_last_topic_anchor_ids.size(),
                   m_history[user_index].relatives.size(),
                   current_retrieval_returns_previous_source);

    save_history();

    auto response = llm::llama_client_response_s{};

    try {
        const auto request_messages = build_request_messages(
                m_history[user_index],
                knowledge,
                relation == chat_relation_kind_e::transform_previous_answer);

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
                            .model_content = make_chat_model_content(answer_body, chat_role_e::assistant),
                            .name = "AI-бот",
                    },

            .source_files = source_filenames,

            .relatives = assistant_relatives,

            .context_state = context_state,
    });

    m_context_state = std::move(context_state);
    m_last_topic_anchor_ids = make_topic_anchor_ids(std::move(assistant_relatives), user_id, assistant_id);

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
    m_context_state.reset();

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
    m_context_state.reset();

    for (auto index = m_history.size(); index > 0; --index) {
        const auto &entry = m_history[index - 1];

        if (!is_completed_assistant_entry(entry)) {
            continue;
        }

        m_last_topic_anchor_ids = {entry.id};

        if (entry.context_state.has_value()) {
            m_context_state = entry.context_state;

            if (m_context_state->source_files.empty()) {
                m_context_state->source_files = entry.source_files;
            }

            limit_source_files(m_context_state->source_files, m_config.max_context_source_files);

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
            const auto *relative = find_history_entry(relative_id);

            if (relative == nullptr || !relative->user.has_value() ||
                util::is_blank(relative->user->model_content)) {
                continue;
            }

            related_user_messages.push_back(truncate_utf8(compact_whitespace(relative->user->model_content),
                                                           m_config.max_context_chars_per_user_message));
        }

        if (related_user_messages.empty()) {
            for (auto previous = index - 1; previous > 0; --previous) {
                const auto &candidate = m_history[previous - 1];

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

                state.recent_user_messages.assign(related_user_messages.begin() +
                                                          static_cast<std::ptrdiff_t>(first_recent),
                                                  related_user_messages.end());
            }
        }

        m_context_state = std::move(state);

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

                            .model_content = make_chat_model_content(user_text, chat_role_e::user),

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

std::string LlamaEngine::build_knowledge_base_block(
        const std::span<const retrieved_knowledge_s> knowledge,
        const std::size_t max_chars_per_document) const {
    assert(max_chars_per_document > 0);

    auto prompt = std::string{"<knowledge_base>\n"};

    for (const auto &item : knowledge) {
        const auto content = truncate_utf8(item.content, max_chars_per_document);

        if (is_glossary_match(item.match)) {
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
    assert(m_context_state.has_value());

    if (!m_context_state.has_value()) {
        std::terminate();
    }

    const auto &state = *m_context_state;
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

    switch (transform_kind) {
        case previous_answer_transform_kind_e::concise:
            prompt = "Сократи предыдущий ответ заметно. Убери повторы, пояснения и второстепенные варианты. "
                     "Оставь только разные основные действия. Не добавляй новые факты, условия или рекомендации.\n";
            break;

        case previous_answer_transform_kind_e::expand:
            prompt = "Раскрой предыдущий ответ заметно подробнее. Добавляй только конкретные действия и пояснения, "
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
            prompt = "Измени только структуру предыдущего ответа в соответствии с запросом пользователя: список, "
                     "чеклист или определение. Сохрани содержание и не добавляй новые факты или правила.\n";
            break;

        case previous_answer_transform_kind_e::rewrite:
            prompt = "Переформулируй предыдущий ответ в соответствии с запросом пользователя. Сохрани исходный смысл "
                     "и не добавляй новые факты, условия, правила или рекомендации.\n";
            break;

        case previous_answer_transform_kind_e::none:
            assert(false);
            std::terminate();
    }

    prompt += "Не добавляй блок источников: приложение добавит его само.\n";

    if (m_context_state.has_value() && !m_context_state->topic.empty()) {
        prompt += std::format("Тема диалога: {}\n", m_context_state->topic);
    }

    prompt += std::format("<previous_answer>\n{}\n</previous_answer>\n", previous_answer);

    if (transform_kind == previous_answer_transform_kind_e::expand) {
        if (knowledge.empty()) {
            prompt += "<knowledge_base>\n</knowledge_base>\n"
                      "Дополнительных материалов нет. В этом случае раскрой только уже названные действия и прямо "
                      "не добавляй новые правила.\n";
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
    if (!m_context_state.has_value()) {
        return std::string{user_text};
    }

    auto query = std::string{user_text};
    const auto &state = *m_context_state;

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
    for (auto it = m_last_topic_anchor_ids.rbegin(); it != m_last_topic_anchor_ids.rend(); ++it) {
        const auto *entry = find_history_entry(*it);

        if (entry == nullptr || !is_completed_assistant_entry(*entry)) {
            continue;
        }

        return truncate_utf8(remove_generated_service_lines(entry->assistant->model_content),
                             m_config.max_transform_answer_chars);
    }

    for (auto it = m_history.rbegin(); it != m_history.rend(); ++it) {
        if (!is_completed_assistant_entry(*it)) {
            continue;
        }

        return truncate_utf8(remove_generated_service_lines(it->assistant->model_content),
                             m_config.max_transform_answer_chars);
    }

    if (m_context_state.has_value()) {
        return truncate_utf8(m_context_state->last_answer_excerpt, m_config.max_transform_answer_chars);
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
    auto state = follow_up && m_context_state.has_value() ? *m_context_state : chat_context_state_s{};
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

    auto messages = std::vector<chat_message_s>{};

    const auto transform_kind = transform_previous_answer
                                        ? classify_previous_answer_transform_request(
                                                  normalize_user_query_for_relation(
                                                          current_user_entry.user->model_content))
                                        : previous_answer_transform_kind_e::none;

    messages.push_back(chat_message_s{
            .role = chat_role_e::system,
            .name = "system",
            .content = transform_kind == previous_answer_transform_kind_e::expand
                               ? "Ты дополняешь предыдущий ответ AI-помощника стажёра. Используй только предыдущий "
                                 "ответ и предоставленную базу знаний. Дай более подробный, конкретный и полезный "
                                 "ответ по-русски, не выдумывая внутренние правила."
                               : transform_previous_answer
                                         ? "Ты редактируешь предыдущий ответ AI-помощника стажёра. Выполни только "
                                           "указанное пользователем преобразование и не добавляй новое содержание. "
                                           "Отвечай по-русски."
                                         : build_system_prompt(knowledge),
            .created_at = util::make_local_timestamp(),
            .source_files = {},
    });

    if (transform_previous_answer) {
        messages.push_back(chat_message_s{
                .role = chat_role_e::system,
                .name = "previous_answer_transform",
                .content = build_previous_answer_transform_prompt(
                        current_user_entry.user->model_content,
                        knowledge),
                .created_at = util::make_local_timestamp(),
                .source_files = {},
        });
    } else if (!current_user_entry.relatives.empty() && m_context_state.has_value()) {
        messages.push_back(chat_message_s{
                .role = chat_role_e::system,
                .name = "context_state",
                .content = build_context_state_prompt(),
                .created_at = util::make_local_timestamp(),
                .source_files = {},
        });
    }

    if (!transform_previous_answer &&
        looks_like_errors_or_risks_request(current_user_entry.user->model_content)) {
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
            .name = current_user_entry.user->name,
            .content = current_user_entry.user->model_content,
            .created_at = current_user_entry.user->created_at,
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
