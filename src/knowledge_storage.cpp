#include "knowledge_storage.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#include "util/file_io.hpp"
#include "util/string_helpers.hpp"

namespace stz::intern {

namespace {

constexpr auto exact_frequent_query_score = std::size_t{1024};
constexpr auto unordered_frequent_query_score = std::size_t{960};
constexpr auto unordered_fuzzy_frequent_query_score = std::size_t{900};

[[nodiscard]] bool is_markdown_file(const std::filesystem::path &filename) {
    auto extension = filename.extension().string();

    std::ranges::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c) noexcept {
        return static_cast<char>(std::tolower(c));
    });

    return extension == ".md" || extension == ".markdown";
}

void append_utf8(const char32_t codepoint, std::string &result) {
    if (codepoint <= 0x7F) {
        result.push_back(static_cast<char>(codepoint));
        return;
    }

    if (codepoint <= 0x7FF) {
        result.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }

    if (codepoint <= 0xFFFF) {
        result.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }

    result.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    result.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
}

[[nodiscard]] char32_t lowercase_codepoint(const char32_t codepoint) noexcept {
    if (codepoint >= U'A' && codepoint <= U'Z') {
        return codepoint + 32;
    }

    if (codepoint >= U'А' && codepoint <= U'Я') {
        return codepoint + 32;
    }

    if (codepoint == U'Ё') {
        return U'ё';
    }

    return codepoint;
}

[[nodiscard]] std::string normalize_for_search(const std::string_view text) {
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

        append_utf8(lowercase_codepoint(codepoint), result);
        index += length;
    }

    return result;
}

[[nodiscard]] std::vector<char32_t> utf8_to_codepoints(const std::string_view text) {
    auto result = std::vector<char32_t>{};

    auto index = std::size_t{};

    while (index < text.size()) {
        const auto first = static_cast<unsigned char>(text[index]);

        if (first < 0x80) {
            result.push_back(static_cast<char32_t>(first));
            ++index;
            continue;
        }

        if ((first & 0xE0) == 0xC0 && index + 1 < text.size()) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            result.push_back(static_cast<char32_t>(((first & 0x1F) << 6) | (second & 0x3F)));
            index += 2;
            continue;
        }

        if ((first & 0xF0) == 0xE0 && index + 2 < text.size()) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            result.push_back(static_cast<char32_t>(((first & 0x0F) << 12) | ((second & 0x3F) << 6) | (third & 0x3F)));
            index += 3;
            continue;
        }

        if ((first & 0xF8) == 0xF0 && index + 3 < text.size()) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            const auto third = static_cast<unsigned char>(text[index + 2]);
            const auto fourth = static_cast<unsigned char>(text[index + 3]);
            result.push_back(static_cast<char32_t>(((first & 0x07) << 18) | ((second & 0x3F) << 12) |
                                                   ((third & 0x3F) << 6) | (fourth & 0x3F)));
            index += 4;
            continue;
        }

        result.push_back(static_cast<char32_t>(first));
        ++index;
    }

    return result;
}

[[nodiscard]] std::size_t utf8_codepoints_count(const std::string_view text) noexcept {
    auto count = std::size_t{};

    for (auto index = std::size_t{}; index < text.size();) {
        const auto byte = static_cast<unsigned char>(text[index]);

        if (byte < 0x80) {
            ++index;
        } else if ((byte & 0xE0) == 0xC0 && index + 1 < text.size()) {
            index += 2;
        } else if ((byte & 0xF0) == 0xE0 && index + 2 < text.size()) {
            index += 3;
        } else if ((byte & 0xF8) == 0xF0 && index + 3 < text.size()) {
            index += 4;
        } else {
            ++index;
        }

        ++count;
    }

    return count;
}

[[nodiscard]] std::size_t max_typo_distance_for_word(const std::size_t codepoints_count) noexcept {
    if (codepoints_count < 5) {
        return 0;
    }

    if (codepoints_count <= 7) {
        return 1;
    }

    return 2;
}

[[nodiscard]] std::size_t damerau_levenshtein_distance(const std::span<const char32_t> lhs,
                                                       const std::span<const char32_t> rhs,
                                                       const std::size_t max_distance) {
    if (lhs.size() > rhs.size() + max_distance || rhs.size() > lhs.size() + max_distance) {
        return max_distance + 1;
    }

    auto previous_previous = std::vector<std::size_t>(rhs.size() + 1);
    auto previous = std::vector<std::size_t>(rhs.size() + 1);
    auto current = std::vector<std::size_t>(rhs.size() + 1);

    for (auto j = std::size_t{}; j <= rhs.size(); ++j) {
        previous[j] = j;
    }

    for (auto i = std::size_t{1}; i <= lhs.size(); ++i) {
        current[0] = i;
        auto row_min = current[0];

        for (auto j = std::size_t{1}; j <= rhs.size(); ++j) {
            const auto cost = lhs[i - 1] == rhs[j - 1] ? std::size_t{} : std::size_t{1};

            auto value = std::min({
                    previous[j] + 1,
                    current[j - 1] + 1,
                    previous[j - 1] + cost,
            });

            if (i > 1 && j > 1 && lhs[i - 1] == rhs[j - 2] && lhs[i - 2] == rhs[j - 1]) {
                value = std::min(value, previous_previous[j - 2] + 1);
            }

            current[j] = value;
            row_min = std::min(row_min, value);
        }

        if (row_min > max_distance) {
            return max_distance + 1;
        }

        previous_previous = previous;
        previous = current;
    }

    return previous[rhs.size()];
}

[[nodiscard]] bool typo_match_terms(const std::string_view lhs, const std::string_view rhs) {
    if (lhs == rhs) {
        return true;
    }

    const auto lhs_codepoints = utf8_to_codepoints(lhs);
    const auto rhs_codepoints = utf8_to_codepoints(rhs);

    const auto min_size = std::min(lhs_codepoints.size(), rhs_codepoints.size());
    const auto max_size = std::max(lhs_codepoints.size(), rhs_codepoints.size());

    const auto max_distance = max_typo_distance_for_word(max_size);

    if (max_distance == 0) {
        return false;
    }

    if (max_size - min_size > max_distance) {
        return false;
    }

    return damerau_levenshtein_distance(lhs_codepoints, rhs_codepoints, max_distance) <= max_distance;
}

[[nodiscard]] bool is_ascii_separator(const unsigned char byte) noexcept {
    if (byte >= 128) {
        return false;
    }

    return std::isspace(byte) != 0 || std::ispunct(byte) != 0;
}

void strip_ascii_punctuation_edges(std::string &text) {
    while (!text.empty()) {
        const auto byte = static_cast<unsigned char>(text.front());

        if (byte >= 128 || std::ispunct(byte) == 0) {
            break;
        }

        text.erase(text.begin());
    }

    while (!text.empty()) {
        const auto byte = static_cast<unsigned char>(text.back());

        if (byte >= 128 || std::ispunct(byte) == 0) {
            break;
        }

        text.pop_back();
    }
}

void collapse_ascii_spaces(std::string &text) {
    auto result = std::string{};
    result.reserve(text.size());

    auto previous_space = false;

    for (const auto ch : text) {
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

    text = std::move(result);
}

[[nodiscard]] std::string make_search_key(const std::string_view text) {
    auto result = normalize_for_search(text);
    util::trim(result);
    strip_ascii_punctuation_edges(result);
    util::trim(result);
    collapse_ascii_spaces(result);
    util::trim(result);

    return result;
}

[[nodiscard]] bool is_search_term_long_enough(const std::string_view term) noexcept {
    return utf8_codepoints_count(term) >= 3;
}

[[nodiscard]] std::vector<std::string> make_search_terms(const std::string_view query) {
    const auto normalized = make_search_key(query);

    auto terms = std::vector<std::string>{};
    auto current = std::string{};

    for (const auto ch : normalized) {
        const auto byte = static_cast<unsigned char>(ch);

        if (is_ascii_separator(byte)) {
            if (is_search_term_long_enough(current)) {
                terms.push_back(std::move(current));
            }

            current.clear();
            continue;
        }

        current.push_back(ch);
    }

    if (is_search_term_long_enough(current)) {
        terms.push_back(std::move(current));
    }

    auto seen = std::unordered_set<std::string>{};
    auto unique_terms = std::vector<std::string>{};
    unique_terms.reserve(terms.size());

    for (auto &term : terms) {
        if (seen.insert(term).second) {
            unique_terms.push_back(std::move(term));
        }
    }

    return unique_terms;
}

[[nodiscard]] bool is_weak_intent_term(const std::string_view term) noexcept {
    constexpr auto weak_terms = std::array{
            std::string_view{"yclients"},   std::string_view{"что"},      std::string_view{"чем"},
            std::string_view{"как"},        std::string_view{"где"},      std::string_view{"когда"},
            std::string_view{"куда"},       std::string_view{"зачем"},    std::string_view{"почему"},
            std::string_view{"кто"},        std::string_view{"кого"},     std::string_view{"кому"},
            std::string_view{"какой"},      std::string_view{"какая"},    std::string_view{"какое"},
            std::string_view{"какие"},      std::string_view{"для"},      std::string_view{"про"},
            std::string_view{"без"},        std::string_view{"или"},      std::string_view{"если"},
            std::string_view{"надо"},       std::string_view{"нужно"},    std::string_view{"можно"},
            std::string_view{"такое"},      std::string_view{"это"},      std::string_view{"означает"},
            std::string_view{"значит"},     std::string_view{"объясни"},  std::string_view{"поясни"},
            std::string_view{"расскажи"},   std::string_view{"подскажи"}, std::string_view{"помоги"},
            std::string_view{"пожалуйста"},
    };

    return std::ranges::any_of(weak_terms,
                               [&](const std::string_view weak_term) { return typo_match_terms(term, weak_term); });
}

[[nodiscard]] std::vector<std::string> make_effective_search_terms(std::vector<std::string> terms) {
    std::erase_if(terms, [](const std::string &term) { return is_weak_intent_term(term); });

    return terms;
}

[[nodiscard]] bool contains_term(const std::span<const std::string> terms, const std::string_view term) noexcept {
    return std::ranges::find(terms, term) != terms.end();
}

[[nodiscard]] bool contains_term_fuzzy(const std::span<const std::string> terms,
                                       const std::string_view query_term) noexcept {
    return std::ranges::any_of(terms, [&](const std::string &term) { return typo_match_terms(query_term, term); });
}

[[nodiscard]] bool query_has_domain_marker(const std::span<const std::string> terms) noexcept {
    constexpr auto domain_terms = std::array{
            std::string_view{"yclients"},  std::string_view{"бариста"},    std::string_view{"кофе"},
            std::string_view{"капучино"},  std::string_view{"латте"},      std::string_view{"раф"},
            std::string_view{"эспрессо"},  std::string_view{"американо"},  std::string_view{"запись"},
            std::string_view{"записи"},    std::string_view{"визит"},      std::string_view{"визита"},
            std::string_view{"клиент"},    std::string_view{"клиента"},    std::string_view{"услуга"},
            std::string_view{"услуги"},    std::string_view{"оплата"},     std::string_view{"оплаты"},
            std::string_view{"скидка"},    std::string_view{"бонусы"},     std::string_view{"сертификат"},
            std::string_view{"абонемент"}, std::string_view{"молоко"},     std::string_view{"сироп"},
            std::string_view{"напиток"},   std::string_view{"кофемашина"}, std::string_view{"холдер"},
            std::string_view{"питчер"},    std::string_view{"смена"},      std::string_view{"жалоба"},
    };

    return std::ranges::any_of(terms, [&](const std::string &term) {
        return std::ranges::any_of(domain_terms, [&](const std::string_view domain_term) {
            return typo_match_terms(term, domain_term);
        });
    });
}

[[nodiscard]] bool is_optional_extra_term(const std::string_view term) noexcept {
    constexpr auto optional_terms = std::array{
            std::string_view{"клиент"},
            std::string_view{"клиента"},
            std::string_view{"клиенту"},
            std::string_view{"клиентом"},
            std::string_view{"визит"},
            std::string_view{"визита"},
            std::string_view{"визитом"},
    };

    return std::ranges::any_of(optional_terms, [&](const std::string_view optional_term) {
        return typo_match_terms(term, optional_term);
    });
}

[[nodiscard]] std::size_t count_extra_strong_terms(const std::span<const std::string> query_terms,
                                                   const std::span<const std::string> frequent_query_terms) noexcept {
    auto extra_count = std::size_t{};

    for (const auto &term : frequent_query_terms) {
        if (is_weak_intent_term(term) || is_optional_extra_term(term)) {
            continue;
        }

        if (!contains_term(query_terms, term)) {
            ++extra_count;
        }
    }

    return extra_count;
}

[[nodiscard]] bool all_strong_query_terms_are_present_exactly(
        const std::span<const std::string> query_terms,
        const std::span<const std::string> frequent_query_terms) noexcept {
    auto strong_terms_count = std::size_t{};

    for (const auto &term : query_terms) {
        if (is_weak_intent_term(term)) {
            continue;
        }

        ++strong_terms_count;

        if (!contains_term(frequent_query_terms, term)) {
            return false;
        }
    }

    return strong_terms_count != 0;
}

[[nodiscard]] bool all_strong_query_terms_are_present_with_typos(
        const std::span<const std::string> query_terms,
        const std::span<const std::string> frequent_query_terms) noexcept {
    auto strong_terms_count = std::size_t{};

    for (const auto &term : query_terms) {
        if (is_weak_intent_term(term)) {
            continue;
        }

        ++strong_terms_count;

        if (!contains_term_fuzzy(frequent_query_terms, term)) {
            return false;
        }
    }

    return strong_terms_count != 0;
}

[[nodiscard]] std::size_t score_unordered_frequent_query(
        const std::span<const std::string> query_terms,
        const std::span<const std::string> frequent_query_terms) noexcept {
    if (!all_strong_query_terms_are_present_exactly(query_terms, frequent_query_terms)) {
        return 0;
    }

    const auto extra_count = count_extra_strong_terms(query_terms, frequent_query_terms);

    if (extra_count >= 4) {
        return 0;
    }

    return unordered_frequent_query_score - extra_count * 64;
}

[[nodiscard]] std::size_t score_unordered_fuzzy_frequent_query(
        const std::span<const std::string> query_terms,
        const std::span<const std::string> frequent_query_terms) noexcept {
    if (!all_strong_query_terms_are_present_with_typos(query_terms, frequent_query_terms)) {
        return 0;
    }

    const auto extra_count = count_extra_strong_terms(query_terms, frequent_query_terms);

    if (extra_count >= 4) {
        return 0;
    }

    return unordered_fuzzy_frequent_query_score - extra_count * 64;
}

[[nodiscard]] std::size_t best_unordered_frequent_query_score(const knowledge_document_s &document,
                                                              const std::span<const std::string> query_terms) noexcept {
    if (query_terms.size() < 2) {
        return 0;
    }

    auto best_score = std::size_t{};

    for (const auto &frequent_query : document.normalized_frequent_queries) {
        const auto frequent_query_terms = make_search_terms(frequent_query);

        if (frequent_query_terms.empty()) {
            continue;
        }

        best_score = std::max(best_score, score_unordered_frequent_query(query_terms, frequent_query_terms));
    }

    return best_score;
}

[[nodiscard]] std::size_t best_unordered_fuzzy_frequent_query_score(
        const knowledge_document_s &document,
        const std::span<const std::string> query_terms) noexcept {
    if (query_terms.size() < 2) {
        return 0;
    }

    auto best_score = std::size_t{};

    for (const auto &frequent_query : document.normalized_frequent_queries) {
        const auto frequent_query_terms = make_search_terms(frequent_query);

        if (frequent_query_terms.empty()) {
            continue;
        }

        best_score = std::max(best_score, score_unordered_fuzzy_frequent_query(query_terms, frequent_query_terms));
    }

    return best_score;
}

[[nodiscard]] std::size_t count_exact_term_occurrences(const std::span<const std::string> terms,
                                                       const std::string_view term) noexcept {
    return static_cast<std::size_t>(std::ranges::count(terms, term));
}

[[nodiscard]] std::string extract_title(const std::string_view content, const std::string &fallback) {
    auto position = std::size_t{};

    while (position < content.size()) {
        const auto line_end = content.find('\n', position);
        const auto line = content.substr(
                position,
                line_end == std::string_view::npos ? std::string_view::npos : line_end - position);

        if (line.starts_with("#")) {
            auto title = std::string{line};
            title.erase(0, title.find_first_not_of("# \t"));

            if (!title.empty()) {
                return title;
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }

        position = line_end + 1;
    }

    return fallback;
}

[[nodiscard]] bool is_markdown_heading(const std::string_view line) {
    auto trimmed = std::string{line};
    util::trim(trimmed);

    return trimmed.starts_with("#");
}

[[nodiscard]] std::string markdown_heading_text(const std::string_view line) {
    auto text = std::string{line};
    util::trim(text);

    while (!text.empty() && text.front() == '#') {
        text.erase(text.begin());
    }

    util::trim(text);

    return make_search_key(text);
}

[[nodiscard]] bool is_frequent_queries_heading(const std::string_view line) {
    if (!is_markdown_heading(line)) {
        return false;
    }

    return markdown_heading_text(line) == "частые запросы пользователя";
}

void remove_markdown_list_marker(std::string &line) {
    util::trim(line);

    if (line.size() >= 2 && (line.starts_with("- ") || line.starts_with("* "))) {
        line.erase(0, 2);
        util::trim(line);
        return;
    }

    auto digit_count = std::size_t{};

    while (digit_count < line.size() && std::isdigit(static_cast<unsigned char>(line[digit_count])) != 0) {
        ++digit_count;
    }

    if (digit_count != 0 && digit_count + 1 < line.size() && (line[digit_count] == '.' || line[digit_count] == ')') &&
        std::isspace(static_cast<unsigned char>(line[digit_count + 1])) != 0) {
        line.erase(0, digit_count + 2);
        util::trim(line);
    }
}

[[nodiscard]] std::vector<std::string> extract_frequent_queries(const std::string_view content) {
    auto result = std::vector<std::string>{};

    auto inside_section = false;
    auto position = std::size_t{};

    while (position <= content.size()) {
        const auto line_end = content.find('\n', position);
        const auto line_view = content.substr(
                position,
                line_end == std::string_view::npos ? content.size() - position : line_end - position);

        auto line = std::string{line_view};
        util::trim(line);

        if (!inside_section) {
            if (is_frequent_queries_heading(line)) {
                inside_section = true;
            }
        } else {
            if (is_markdown_heading(line)) {
                break;
            }

            remove_markdown_list_marker(line);

            if (!line.empty()) {
                result.push_back(std::move(line));
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }

        position = line_end + 1;
    }

    return result;
}

[[nodiscard]] std::vector<std::string> normalize_frequent_queries(const std::span<const std::string> queries) {
    auto result = std::vector<std::string>{};
    auto seen = std::unordered_set<std::string>{};

    result.reserve(queries.size());

    for (const auto &query : queries) {
        auto normalized = make_search_key(query);

        if (normalized.empty()) {
            continue;
        }

        if (seen.insert(normalized).second) {
            result.push_back(std::move(normalized));
        }
    }

    return result;
}

[[nodiscard]] std::string take_prefix_utf8_safe(const std::string_view text, const std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string{text};
    }

    auto size = max_bytes;

    while (size > 0) {
        const auto byte = static_cast<unsigned char>(text[size]);

        if ((byte & 0xC0) != 0x80) {
            break;
        }

        --size;
    }

    auto result = std::string{text.substr(0, size)};
    result += "\n\n[...]";

    return result;
}

[[nodiscard]] std::optional<workplace_role_e> role_from_directory_name(const std::string_view directory_name) {
    if (directory_name == "general") {
        return workplace_role_e::general;
    }

    if (directory_name == "barista") {
        return workplace_role_e::barista;
    }

    if (directory_name == "seller") {
        return workplace_role_e::seller;
    }

    if (directory_name == "beauty_admin") {
        return workplace_role_e::beauty_admin;
    }

    return std::nullopt;
}

[[nodiscard]] std::optional<workplace_role_e> role_from_relative_filename(const std::string_view relative_filename) {
    const auto separator_position = relative_filename.find('/');

    if (separator_position == std::string_view::npos) {
        return std::nullopt;
    }

    return role_from_directory_name(relative_filename.substr(0, separator_position));
}

[[nodiscard]] knowledge_source_e detect_source_type(const std::string_view relative_filename) noexcept {
    const auto filename_position = relative_filename.find_last_of('/');
    const auto filename = filename_position == std::string_view::npos ? relative_filename
                                                                      : relative_filename.substr(filename_position + 1);

    if (filename.starts_with("custom_") || relative_filename.starts_with("custom/") ||
        relative_filename.find("/custom/") != std::string_view::npos) {
        return knowledge_source_e::custom;
    }

    return knowledge_source_e::builtin;
}

[[nodiscard]] bool is_direct_match(const knowledge_match_e match) noexcept {
    return match == knowledge_match_e::exact_frequent_query || match == knowledge_match_e::unordered_frequent_query ||
           match == knowledge_match_e::unordered_fuzzy_frequent_query;
}

[[nodiscard]] retrieved_knowledge_s make_retrieved_document(const knowledge_document_s &document,
                                                            const std::size_t score,
                                                            const std::size_t max_chars_per_document,
                                                            const knowledge_match_e match) {
    return retrieved_knowledge_s{
            .filename = document.filename,
            .title = document.title,
            .content = is_direct_match(match) ? document.content
                                              : take_prefix_utf8_safe(document.content, max_chars_per_document),
            .score = score,
            .source = document.source,
            .role = document.role,
            .match = match,
    };
}

} // namespace

std::string_view to_string(const knowledge_source_e source) noexcept {
    switch (source) {
        case knowledge_source_e::builtin: return "builtin";
        case knowledge_source_e::custom: return "custom";
    }

    return "builtin";
}

std::string_view to_string(const workplace_role_e role) noexcept {
    switch (role) {
        case workplace_role_e::general: return "general";
        case workplace_role_e::barista: return "barista";
        case workplace_role_e::seller: return "seller";
        case workplace_role_e::beauty_admin: return "beauty_admin";
    }

    return "general";
}

std::string_view to_string(const knowledge_match_e match) noexcept {
    switch (match) {
        case knowledge_match_e::none: return "none";
        case knowledge_match_e::exact_frequent_query: return "exact_frequent_query";
        case knowledge_match_e::unordered_frequent_query: return "unordered_frequent_query";
        case knowledge_match_e::unordered_fuzzy_frequent_query: return "unordered_fuzzy_frequent_query";
        case knowledge_match_e::ranked: return "ranked";
    }

    return "none";
}

workplace_role_e workplace_role_from_string(std::string_view text) {
    const auto normalized = make_search_key(text);

    if (normalized == "general" || normalized == "общий") {
        return workplace_role_e::general;
    }

    if (normalized == "barista" || normalized == "бариста") {
        return workplace_role_e::barista;
    }

    if (normalized == "seller" || normalized == "продавец" || normalized == "кассир" ||
        normalized == "продавец кассир" || normalized == "продавец-кассир") {
        return workplace_role_e::seller;
    }

    if (normalized == "beauty_admin" || normalized == "beauty admin" || normalized == "администратор салона" ||
        normalized == "администратор" || normalized == "ресепшен" || normalized == "reception") {
        return workplace_role_e::beauty_admin;
    }

    throw std::runtime_error{std::format("Unknown workplace role '{}'", text)};
}

KnowledgeStorage::KnowledgeStorage(std::filesystem::path directory, std::shared_ptr<spdlog::logger> logger)
    : m_directory{std::move(directory)},
      m_logger{std::move(logger)} {
    assert(!m_directory.empty());
    assert(m_logger != nullptr);
}

void KnowledgeStorage::load() {
    m_documents.clear();

    if (!std::filesystem::exists(m_directory)) {
        m_logger->warn("Knowledge directory does not exist: {}", m_directory.string());
        return;
    }

    for (const auto &entry : std::filesystem::recursive_directory_iterator{m_directory}) {
        if (!entry.is_regular_file() || !is_markdown_file(entry.path())) {
            continue;
        }

        auto relative_filename = std::filesystem::relative(entry.path(), m_directory).generic_string();
        const auto role = role_from_relative_filename(relative_filename);

        if (!role.has_value()) {
            m_logger->warn("Skipping knowledge file outside role directory: {}", relative_filename);
            continue;
        }

        auto content = util::read_text_file(entry.path());
        auto title = extract_title(content, entry.path().filename().string());
        auto frequent_queries = extract_frequent_queries(content);

        auto document = knowledge_document_s{
                .filename = std::move(relative_filename),
                .title = std::move(title),
                .content = std::move(content),
                .frequent_queries = std::move(frequent_queries),
                .normalized_filename = {},
                .normalized_title = {},
                .normalized_frequent_queries = {},
                .source = detect_source_type(relative_filename),
                .role = *role,
        };

        document.normalized_filename = make_search_key(document.filename);
        document.normalized_title = make_search_key(document.title);
        document.normalized_frequent_queries = normalize_frequent_queries(document.frequent_queries);

        m_documents.push_back(std::move(document));
    }

    std::ranges::sort(m_documents, {}, &knowledge_document_s::filename);

    auto general_count = std::size_t{};
    auto barista_count = std::size_t{};
    auto seller_count = std::size_t{};
    auto beauty_admin_count = std::size_t{};
    auto custom_count = std::size_t{};
    auto documents_without_frequent_queries = std::size_t{};

    for (const auto &document : m_documents) {
        if (document.normalized_frequent_queries.empty()) {
            ++documents_without_frequent_queries;
        }

        if (document.source == knowledge_source_e::custom) {
            ++custom_count;
        }

        switch (document.role) {
            case workplace_role_e::general: ++general_count; break;
            case workplace_role_e::barista: ++barista_count; break;
            case workplace_role_e::seller: ++seller_count; break;
            case workplace_role_e::beauty_admin: ++beauty_admin_count; break;
        }
    }

    m_logger->info("Loaded {} knowledge markdown files from '{}'", m_documents.size(), m_directory.string());

    m_logger->info("Knowledge map: general={}, barista={}, seller={}, beauty_admin={}, custom={}",
                   general_count,
                   barista_count,
                   seller_count,
                   beauty_admin_count,
                   custom_count);

    if (documents_without_frequent_queries != 0) {
        m_logger->warn("{} knowledge files have no 'Частые запросы пользователя' section",
                       documents_without_frequent_queries);
    }
}

std::vector<retrieved_knowledge_s> KnowledgeStorage::retrieve(const std::string_view query,
                                                              const knowledge_retrieve_options_s &options) const {
    if (m_documents.empty() || options.limit == 0) {
        return {};
    }

    const auto normalized_query = make_search_key(query);

    if (normalized_query.empty()) {
        return {};
    }

    const auto original_terms = make_search_terms(normalized_query);
    auto terms = make_effective_search_terms(original_terms);

    if (terms.empty()) {
        return {};
    }

    if (!query_has_domain_marker(original_terms)) {
        m_logger->debug("Knowledge retrieval skipped: query has no domain marker");
        return {};
    }

    auto direct_results = std::vector<retrieved_knowledge_s>{};
    auto ranked_results = std::vector<retrieved_knowledge_s>{};

    ranked_results.reserve(std::min(options.limit * 2, m_documents.size()));

    for (const auto &document : m_documents) {
        if (!document_matches_options(document, options)) {
            continue;
        }

        if (has_exact_frequent_query_match(document, normalized_query)) {
            direct_results.push_back(make_retrieved_document(document,
                                                             exact_frequent_query_score,
                                                             options.max_chars_per_document,
                                                             knowledge_match_e::exact_frequent_query));
            continue;
        }

        const auto unordered_score = best_unordered_frequent_query_score(document, terms);

        if (unordered_score != 0) {
            direct_results.push_back(make_retrieved_document(document,
                                                             unordered_score,
                                                             options.max_chars_per_document,
                                                             knowledge_match_e::unordered_frequent_query));
            continue;
        }

        const auto unordered_fuzzy_score = best_unordered_fuzzy_frequent_query_score(document, terms);

        if (unordered_fuzzy_score != 0) {
            direct_results.push_back(make_retrieved_document(document,
                                                             unordered_fuzzy_score,
                                                             options.max_chars_per_document,
                                                             knowledge_match_e::unordered_fuzzy_frequent_query));
            continue;
        }

        const auto score = score_document(document, terms, normalized_query);

        if (score < options.min_ranked_score) {
            if (score != 0) {
                m_logger->debug("Knowledge candidate rejected by score threshold: {} score={} min_score={}",
                                document.filename,
                                score,
                                options.min_ranked_score);
            }

            continue;
        }

        ranked_results.push_back(
                make_retrieved_document(document, score, options.max_chars_per_document, knowledge_match_e::ranked));
    }

    if (!direct_results.empty()) {
        std::ranges::sort(direct_results, [](const retrieved_knowledge_s &lhs, const retrieved_knowledge_s &rhs) {
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }

            return lhs.filename < rhs.filename;
        });

        direct_results.resize(1);
        return direct_results;
    }

    std::ranges::sort(ranked_results, [](const retrieved_knowledge_s &lhs, const retrieved_knowledge_s &rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }

        return lhs.filename < rhs.filename;
    });

    if (ranked_results.size() > options.limit) {
        ranked_results.resize(options.limit);
    }

    return ranked_results;
}

std::vector<retrieved_knowledge_s> KnowledgeStorage::retrieve_by_filenames(
        const std::span<const std::string> filenames,
        const knowledge_retrieve_options_s &options) const {
    if (m_documents.empty() || filenames.empty() || options.limit == 0) {
        return {};
    }

    auto result = std::vector<retrieved_knowledge_s>{};
    auto seen = std::unordered_set<std::string>{};

    result.reserve(std::min(options.limit, filenames.size()));
    seen.reserve(filenames.size());

    for (const auto &filename : filenames) {
        if (result.size() >= options.limit) {
            break;
        }

        if (!seen.insert(filename).second) {
            continue;
        }

        const auto it = std::ranges::find_if(m_documents, [&](const knowledge_document_s &document) noexcept {
            return document.filename == filename && document_matches_options(document, options);
        });

        if (it == m_documents.end()) {
            m_logger->debug("Knowledge file referenced by chat history was not found: {}", filename);
            continue;
        }

        result.push_back(make_retrieved_document(*it,
                                                 exact_frequent_query_score,
                                                 options.max_chars_per_document,
                                                 knowledge_match_e::ranked));
    }

    return result;
}

bool KnowledgeStorage::empty() const noexcept { return m_documents.empty(); }

std::size_t KnowledgeStorage::size() const noexcept { return m_documents.size(); }

bool KnowledgeStorage::document_matches_options(const knowledge_document_s &document,
                                                const knowledge_retrieve_options_s &options) noexcept {
    if (!options.include_custom && document.source == knowledge_source_e::custom) {
        return false;
    }

    if (document.role == options.workplace_role) {
        return true;
    }

    return options.include_general && document.role == workplace_role_e::general;
}

std::size_t KnowledgeStorage::score_document(const knowledge_document_s &document,
                                             const std::span<const std::string> terms,
                                             const std::string_view normalized_query) noexcept {
    auto score = std::size_t{};

    const auto title_terms = make_search_terms(document.normalized_title);

    if (document.normalized_title == normalized_query) {
        score += 768;
    } else {
        if (document.normalized_title.find(normalized_query) != std::string::npos) {
            score += 384;
        }

        if (normalized_query.find(document.normalized_title) != std::string::npos) {
            score += 256;
        }
    }

    for (const auto &term : terms) {
        score += count_exact_term_occurrences(title_terms, term) * term.size() * 16;
    }

    for (const auto &frequent_query : document.normalized_frequent_queries) {
        const auto frequent_query_terms = make_search_terms(frequent_query);

        if (frequent_query == normalized_query) {
            score += 512;
        } else {
            if (frequent_query.find(normalized_query) != std::string::npos) {
                score += 384;
            }

            if (normalized_query.find(frequent_query) != std::string::npos) {
                score += 256;
            }
        }

        for (const auto &term : terms) {
            score += count_exact_term_occurrences(frequent_query_terms, term) * term.size() * 8;
        }
    }

    return score;
}

bool KnowledgeStorage::has_exact_frequent_query_match(const knowledge_document_s &document,
                                                      const std::string_view normalized_query) noexcept {
    return std::ranges::any_of(document.normalized_frequent_queries,
                               [&](const std::string &frequent_query) { return frequent_query == normalized_query; });
}

} // namespace stz::intern