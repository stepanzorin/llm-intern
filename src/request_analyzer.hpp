// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stz::intern {

enum class texting_intent_e {
    unknown,
    master_unavailable,
    client_late,
    client_dissatisfied_after_visit,
    client_complaint,
    client_satisfied,
    client_cancels,
    client_reschedules,
};

[[nodiscard]] constexpr std::string_view to_string(const texting_intent_e intent) noexcept {
    switch (intent) {
        case texting_intent_e::unknown: return "unknown";
        case texting_intent_e::master_unavailable: return "master_unavailable";
        case texting_intent_e::client_late: return "client_late";
        case texting_intent_e::client_dissatisfied_after_visit: return "client_dissatisfied_after_visit";
        case texting_intent_e::client_complaint: return "client_complaint";
        case texting_intent_e::client_satisfied: return "client_satisfied";
        case texting_intent_e::client_cancels: return "client_cancels";
        case texting_intent_e::client_reschedules: return "client_reschedules";
    }

    return "unknown";
}

struct texting_request_analysis_s {
    std::string retrieval_query = {};
    std::vector<std::string> retrieval_queries = {};
    std::vector<std::string> semantic_parts = {};
    std::unordered_map<std::string, std::string> placeholder_values = {};

    texting_intent_e intent = texting_intent_e::unknown;

    bool contains_client_message = false;
    bool has_style_instruction = false;
    bool negative_feedback = false;
};

class TextingRequestAnalyzer final {
public:
    [[nodiscard]] static texting_request_analysis_s analyze(const std::string_view user_text) {
        const auto normalized = normalize(user_text);
        auto semantic_parts = split_semantic_parts(user_text);
        const auto intent = detect_intent(normalized, semantic_parts);
        auto retrieval_queries = make_retrieval_queries(intent, normalized, semantic_parts);
        auto retrieval_query = retrieval_queries.empty() ? normalized : retrieval_queries.front();
        const auto contains_client_message = user_text.contains('\n') ||
                                             normalized.contains("добрый день") ||
                                             normalized.contains("здравствуйте");

        auto result = texting_request_analysis_s{
                .retrieval_query = std::move(retrieval_query),
                .retrieval_queries = std::move(retrieval_queries),
                .semantic_parts = std::move(semantic_parts),
                .placeholder_values = extract_placeholder_values(user_text),
                .intent = intent,
                .contains_client_message = contains_client_message,
                .has_style_instruction = contains_any(normalized,
                                                      {"формаль", "официальн", "сухо", "строже", "строг",
                                                       "дружелюб", "теплее", "мягче", "эмодзи", "смайлик"}),
                .negative_feedback = intent == texting_intent_e::client_dissatisfied_after_visit ||
                                     intent == texting_intent_e::client_complaint,
        };

        return result;
    }

    [[nodiscard]] static std::string instantiate_script(
            const std::string_view script,
            const std::unordered_map<std::string, std::string> &placeholder_values) {
        auto result = std::string{};
        result.reserve(script.size());

        auto position = std::size_t{};

        while (position < script.size()) {
            const auto open = script.find('<', position);

            if (open == std::string_view::npos) {
                result.append(script.substr(position));
                break;
            }

            result.append(script.substr(position, open - position));

            const auto close = script.find('>', open + 1);

            if (close == std::string_view::npos) {
                result.append(script.substr(open));
                break;
            }

            const auto placeholder = script.substr(open + 1, close - open - 1);
            const auto key = normalize_key(placeholder);
            const auto value = placeholder_values.find(key);

            if (value == placeholder_values.end()) {
                result.append(script.substr(open, close - open + 1));
            } else {
                result += value->second;
            }

            position = close + 1;
        }

        return result;
    }

    [[nodiscard]] static bool looks_like_follow_up_edit(const std::string_view user_text) {
        const auto normalized = normalize(user_text);

        if (normalized.size() > 420) {
            return false;
        }

        return starts_with_any(normalized,
                               {"добавь", "убери", "удали", "замени", "укажи", "напиши", "не пиши",
                                "сделай", "перефразируй", "переформулируй", "перефраз", "перефариз",
                                "перепиши", "сократи",
                                "короче", "кратче", "кратко", "подробнее", "формальнее", "строже",
                                "дружелюбнее", "мягче", "больше эмодзи", "побольше смайликов",
                                "не хватает эмодзи", "не хватает смайликов"});
    }

private:
    [[nodiscard]] static bool is_ascii_space(const unsigned char ch) noexcept {
        return ch < 128 && std::isspace(ch) != 0;
    }

    static void append_lowercase_utf8_codepoint(const char32_t codepoint, std::string &result) {
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
        } else if (lowered <= 0x7FF) {
            result.push_back(static_cast<char>(0xC0 | ((lowered >> 6) & 0x1F)));
            result.push_back(static_cast<char>(0x80 | (lowered & 0x3F)));
        } else if (lowered <= 0xFFFF) {
            result.push_back(static_cast<char>(0xE0 | ((lowered >> 12) & 0x0F)));
            result.push_back(static_cast<char>(0x80 | ((lowered >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (lowered & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | ((lowered >> 18) & 0x07)));
            result.push_back(static_cast<char>(0x80 | ((lowered >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((lowered >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (lowered & 0x3F)));
        }
    }

    [[nodiscard]] static std::string lowercase_utf8(const std::string_view text) {
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
                codepoint = static_cast<char32_t>(((first & 0x1F) << 6) |
                                                  (static_cast<unsigned char>(text[index + 1]) & 0x3F));
                length = 2;
            } else if ((first & 0xF0) == 0xE0 && index + 2 < text.size()) {
                codepoint = static_cast<char32_t>(((first & 0x0F) << 12) |
                                                  ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
                                                  (static_cast<unsigned char>(text[index + 2]) & 0x3F));
                length = 3;
            } else if ((first & 0xF8) == 0xF0 && index + 3 < text.size()) {
                codepoint = static_cast<char32_t>(((first & 0x07) << 18) |
                                                  ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
                                                  ((static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
                                                  (static_cast<unsigned char>(text[index + 3]) & 0x3F));
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

    [[nodiscard]] static std::string normalize(const std::string_view text) {
        auto result = lowercase_utf8(text);
        auto compact = std::string{};
        compact.reserve(result.size());
        auto previous_space = false;

        for (const auto ch : result) {
            const auto byte = static_cast<unsigned char>(ch);

            if (is_ascii_space(byte)) {
                if (!compact.empty() && !previous_space) {
                    compact.push_back(' ');
                }

                previous_space = true;
                continue;
            }

            compact.push_back(ch);
            previous_space = false;
        }

        trim(compact);
        return compact;
    }

    [[nodiscard]] static bool is_semantic_separator(const char ch) noexcept {
        switch (ch) {
            case '.':
            case ',':
            case ';':
            case ':':
            case '!':
            case '?':
            case '\n':
            case '\r': return true;
            default: return false;
        }
    }

    [[nodiscard]] static bool is_wrapper_part(const std::string_view part) noexcept {
        return part == "как ответить клиенту" ||
               part == "как ответить" ||
               part == "составь ответ клиенту" ||
               part == "напиши ответ клиенту";
    }

    [[nodiscard]] static std::vector<std::string> split_semantic_parts(const std::string_view text) {
        auto result = std::vector<std::string>{};
        auto part_begin = std::size_t{};

        for (auto index = std::size_t{}; index <= text.size(); ++index) {
            if (index != text.size() && !is_semantic_separator(text[index])) {
                continue;
            }

            auto part = normalize(text.substr(part_begin, index - part_begin));

            if (!part.empty() && !is_wrapper_part(part)) {
                result.push_back(std::move(part));
            }

            part_begin = index + 1;
        }

        return result;
    }

    [[nodiscard]] static std::string normalize_key(const std::string_view text) {
        auto result = normalize(text);

        if (result == "клиент" || result == "клиентка" || result == "имя клиента") {
            return "имя";
        }

        return result;
    }

    static void trim(std::string &text) {
        auto begin = std::size_t{};

        while (begin < text.size() && is_ascii_space(static_cast<unsigned char>(text[begin]))) {
            ++begin;
        }

        auto end = text.size();

        while (end > begin && is_ascii_space(static_cast<unsigned char>(text[end - 1]))) {
            --end;
        }

        text = text.substr(begin, end - begin);
    }

    [[nodiscard]] static bool contains_any(const std::string_view text,
                                           const std::initializer_list<std::string_view> fragments) {
        for (const auto fragment : fragments) {
            if (text.contains(fragment)) {
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] static bool starts_with_any(const std::string_view text,
                                              const std::initializer_list<std::string_view> prefixes) {
        for (const auto prefix : prefixes) {
            if (text.starts_with(prefix)) {
                return true;
            }
        }

        return false;
    }

    [[nodiscard]] static bool is_word_separator(const unsigned char ch) noexcept {
        return ch < 128 && (std::isspace(ch) != 0 || std::ispunct(ch) != 0);
    }

    [[nodiscard]] static bool has_negation_before_stem(const std::string_view text,
                                                       const std::string_view stem,
                                                       const std::size_t max_words) {
        auto search_position = std::size_t{};

        while (search_position < text.size()) {
            const auto stem_position = text.find(stem, search_position);

            if (stem_position == std::string_view::npos) {
                return false;
            }

            auto cursor = stem_position;
            auto inspected_words = std::size_t{};

            while (cursor > 0 && inspected_words < max_words) {
                while (cursor > 0 && is_word_separator(static_cast<unsigned char>(text[cursor - 1]))) {
                    --cursor;
                }

                const auto word_end = cursor;

                while (cursor > 0 && !is_word_separator(static_cast<unsigned char>(text[cursor - 1]))) {
                    --cursor;
                }

                if (word_end == cursor) {
                    break;
                }

                const auto word = text.substr(cursor, word_end - cursor);

                if (word == "не" || word == "ни") {
                    const auto between = text.substr(word_end, stem_position - word_end);

                    if (contains_any(between, {"только", "просто"})) {
                        return false;
                    }

                    return true;
                }

                ++inspected_words;
            }

            search_position = stem_position + stem.size();
        }

        return false;
    }

    [[nodiscard]] static bool has_dissatisfaction_signal(const std::string_view text) {
        return has_negation_before_stem(text, "довол", 8) ||
               contains_any(text,
                            {"недовол", "неудовлетвор", "не понрав", "не устро", "разочар",
                             "некачествен", "плохо сдел", "есть замечани", "не соответствует ожид",
                             "ожидания не оправдал"});
    }

    [[nodiscard]] static bool has_result_context(const std::string_view text) {
        return contains_any(text,
                            {"результат", "после посещен", "после визит", "визит", "услуг", "обслуживан",
                             "процедур", "работ", "заказ", "покупк", "товар", "достав", "маникюр",
                             "педикюр", "покрыт", "дизайн", "ногт", "стриж", "окраш", "бров", "ресниц"});
    }

    [[nodiscard]] static bool refuses_to_return(const std::string_view text) {
        return contains_any(text,
                            {"повторять не план", "больше не приду", "больше не обращусь", "не вернусь",
                             "не буду возвращаться", "обращаться больше не буду"});
    }

    [[nodiscard]] static bool has_explicit_complaint(const std::string_view text) {
        return contains_any(text,
                            {"жалоб", "претензи", "возмущ", "плохой сервис", "ужасное обслуживание"});
    }

    [[nodiscard]] static texting_intent_e detect_intent(
            const std::string_view normalized,
            const std::vector<std::string> &semantic_parts) {
        if (contains_any(normalized, {"мастер заболел", "мастер не вышел", "не вышел на смену"})) {
            return texting_intent_e::master_unavailable;
        }

        if (contains_any(normalized, {"клиент опаздывает", "клиент задерж", "я опаздываю", "задержусь"})) {
            return texting_intent_e::client_late;
        }

        auto dissatisfaction = has_dissatisfaction_signal(normalized);
        auto result_context = has_result_context(normalized);
        auto return_refusal = refuses_to_return(normalized);
        auto explicit_complaint = has_explicit_complaint(normalized);

        for (const auto &part : semantic_parts) {
            dissatisfaction = dissatisfaction || has_dissatisfaction_signal(part);
            result_context = result_context || has_result_context(part);
            return_refusal = return_refusal || refuses_to_return(part);
            explicit_complaint = explicit_complaint || has_explicit_complaint(part);
        }

        if (dissatisfaction && (result_context || return_refusal)) {
            return texting_intent_e::client_dissatisfied_after_visit;
        }

        if (dissatisfaction || explicit_complaint) {
            return texting_intent_e::client_complaint;
        }

        if (contains_any(normalized, {"всё понравилось", "все понравилось", "очень доволь", "спасибо за работу"})) {
            return texting_intent_e::client_satisfied;
        }

        if (contains_any(normalized, {"отменить запись", "отмена записи", "не смогу прийти"})) {
            return texting_intent_e::client_cancels;
        }

        if (contains_any(normalized, {"перенести запись", "перенос записи", "другое время"})) {
            return texting_intent_e::client_reschedules;
        }

        return texting_intent_e::unknown;
    }

    [[nodiscard]] static bool is_retrieval_noise_part(const std::string_view part) noexcept {
        return part == "добрый день" ||
               part == "здравствуйте" ||
               part == "привет" ||
               part == "нет" ||
               part == "да" ||
               part == "спасибо" ||
               part == "благодарю" ||
               part == "пожалуйста";
    }

    static void append_unique_retrieval_query(std::vector<std::string> &queries,
                                              const std::string_view query) {
        if (query.empty() || is_retrieval_noise_part(query)) {
            return;
        }

        if (std::ranges::find(queries, query) == queries.end()) {
            queries.emplace_back(query);
        }
    }

    static void append_intent_retrieval_queries(std::vector<std::string> &queries,
                                                const texting_intent_e intent) {
        const auto append_all = [&](const std::initializer_list<std::string_view> aliases) {
            for (const auto alias : aliases) {
                append_unique_retrieval_query(queries, alias);
            }
        };

        switch (intent) {
            case texting_intent_e::master_unavailable:
                append_all({"мастер заболел", "мастер не вышел"});
                break;
            case texting_intent_e::client_late:
                append_all({"клиент опаздывает", "клиент задерживается", "опоздание клиента"});
                break;
            case texting_intent_e::client_dissatisfied_after_visit:
                append_all({"клиент не остался доволен",
                            "клиентка недовольна после записи",
                            "клиенту не понравился результат",
                            "клиентке не понравилось",
                            "клиент недоволен",
                            "клиент жалуется"});
                break;
            case texting_intent_e::client_complaint:
                append_all({"клиент жалуется", "жалоба", "клиент недоволен"});
                break;
            case texting_intent_e::client_satisfied:
                append_all({"клиент доволен", "клиенту понравилось"});
                break;
            case texting_intent_e::client_cancels:
                append_all({"клиент отменяет запись", "отмена записи"});
                break;
            case texting_intent_e::client_reschedules:
                append_all({"клиент переносит запись", "перенос записи"});
                break;
            case texting_intent_e::unknown:
                break;
        }
    }

    [[nodiscard]] static std::vector<std::string> make_retrieval_queries(
            const texting_intent_e intent,
            const std::string_view normalized,
            const std::vector<std::string> &semantic_parts) {
        auto result = std::vector<std::string>{};
        result.reserve(semantic_parts.size() + 8);

        append_intent_retrieval_queries(result, intent);

        for (const auto &part : semantic_parts) {
            append_unique_retrieval_query(result, part);
        }

        if (result.empty()) {
            append_unique_retrieval_query(result, make_retrieval_query(normalized));
        }

        return result;
    }

    [[nodiscard]] static std::string make_retrieval_query(const std::string_view normalized) {
        auto result = std::string{normalized};

        for (const auto prefix : {std::string_view{"как ответить клиенту"},
                                  std::string_view{"как ответить"},
                                  std::string_view{"составь ответ клиенту"},
                                  std::string_view{"напиши ответ клиенту"}}) {
            if (result.starts_with(prefix)) {
                result.erase(0, prefix.size());
                trim(result);

                while (!result.empty() && (result.front() == ':' || result.front() == '-' || result.front() == '?')) {
                    result.erase(result.begin());
                    trim(result);
                }

                break;
            }
        }

        return result;
    }

    [[nodiscard]] static std::unordered_map<std::string, std::string> extract_placeholder_values(
            const std::string_view text) {
        auto result = std::unordered_map<std::string, std::string>{};
        auto position = std::size_t{};

        while (position <= text.size()) {
            const auto line_end = text.find('\n', position);
            auto line = std::string{text.substr(
                    position,
                    line_end == std::string_view::npos ? text.size() - position : line_end - position)};
            trim(line);

            if (const auto separator = line.find(':'); separator != std::string::npos && separator != 0) {
                auto key = normalize_key(std::string_view{line}.substr(0, separator));
                auto value = line.substr(separator + 1);
                trim(value);

                if (!key.empty() && key.size() <= 64 && !value.empty()) {
                    result.insert_or_assign(std::move(key), std::move(value));
                }
            }

            if (line_end == std::string_view::npos) {
                break;
            }

            position = line_end + 1;
        }

        const auto normalized = normalize(text);
        constexpr auto name_prefix = std::string_view{"клиента зовут "};

        if (!result.contains("имя")) {
            if (const auto name_position = normalized.find(name_prefix); name_position != std::string::npos) {
                auto name = std::string{normalized.substr(name_position + name_prefix.size())};
                const auto end = name.find_first_of(".,!?;:\n");

                if (end != std::string::npos) {
                    name.erase(end);
                }

                trim(name);

                if (!name.empty()) {
                    if (const auto first = static_cast<unsigned char>(name.front()); first < 128) {
                        name.front() = static_cast<char>(std::toupper(first));
                    }

                    result.emplace("имя", std::move(name));
                }
            }
        }

        return result;
    }
};

} // namespace stz::intern
