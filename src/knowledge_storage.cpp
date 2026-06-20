#include "knowledge_storage.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <format>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#include "util/file_io.hpp"
#include "util/string_helpers.hpp"

namespace stz::intern {

namespace {

constexpr auto exact_frequent_query_score = std::size_t{10000};
constexpr auto unordered_frequent_query_score = std::size_t{9300};
constexpr auto unordered_fuzzy_frequent_query_score = std::size_t{8600};
constexpr auto direct_frequent_query_min_score = std::size_t{7600};
constexpr auto exact_glossary_heading_score = std::size_t{10000};
constexpr auto unordered_glossary_heading_score = std::size_t{9300};
constexpr auto unordered_fuzzy_glossary_heading_score = std::size_t{8600};
constexpr auto direct_glossary_heading_min_score = std::size_t{7600};
constexpr auto ranked_context_query_min_score = std::size_t{4200};
constexpr auto ranked_context_query_secondary_gap = std::size_t{900};

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
        return U'е';
    }

    if (codepoint == U'ё') {
        return U'е';
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

[[nodiscard]] bool prefix_match_search_terms(const std::string_view lhs, const std::string_view rhs) {
    if (lhs == rhs) {
        return true;
    }

    const auto lhs_codepoints = utf8_to_codepoints(lhs);
    const auto rhs_codepoints = utf8_to_codepoints(rhs);
    const auto min_size = std::min(lhs_codepoints.size(), rhs_codepoints.size());

    if (min_size < 5) {
        return false;
    }

    auto common_prefix_size = std::size_t{};

    while (common_prefix_size < min_size && lhs_codepoints[common_prefix_size] == rhs_codepoints[common_prefix_size]) {
        ++common_prefix_size;
    }

    const auto required_prefix_size = min_size >= 7 ? std::size_t{5} : std::size_t{4};

    return common_prefix_size >= required_prefix_size;
}

[[nodiscard]] bool search_match_terms(const std::string_view lhs, const std::string_view rhs) {
    return typo_match_terms(lhs, rhs) || prefix_match_search_terms(lhs, rhs);
}

[[nodiscard]] std::string make_recall_stem(const std::string_view term) {
    auto stem = std::string{term};

    if (utf8_codepoints_count(stem) < 5) {
        return stem;
    }

    constexpr auto long_suffixes = std::array{
            std::string_view{"ениями"}, std::string_view{"ение"}, std::string_view{"ении"},
            std::string_view{"ения"},   std::string_view{"ению"}, std::string_view{"ением"},
            std::string_view{"иями"},   std::string_view{"ями"},  std::string_view{"ами"},
            std::string_view{"ого"},    std::string_view{"ему"},  std::string_view{"ыми"},
            std::string_view{"ими"},    std::string_view{"ить"},  std::string_view{"ать"},
            std::string_view{"ять"},    std::string_view{"еть"},  std::string_view{"уть"},
            std::string_view{"ешь"},    std::string_view{"ете"},  std::string_view{"ют"},
            std::string_view{"ет"},     std::string_view{"ит"},
    };

    for (const auto suffix : long_suffixes) {
        if (!stem.ends_with(suffix)) {
            continue;
        }

        const auto stem_codepoints = utf8_codepoints_count(stem);
        const auto suffix_codepoints = utf8_codepoints_count(suffix);

        if (stem_codepoints <= suffix_codepoints + 3) {
            continue;
        }

        stem.erase(stem.size() - suffix.size());
        return stem;
    }

    constexpr auto short_suffixes = std::array{
            std::string_view{"а"}, std::string_view{"я"}, std::string_view{"у"},
            std::string_view{"ю"}, std::string_view{"е"}, std::string_view{"ы"},
            std::string_view{"и"}, std::string_view{"ь"},
    };

    for (const auto suffix : short_suffixes) {
        if (!stem.ends_with(suffix)) {
            continue;
        }

        const auto stem_codepoints = utf8_codepoints_count(stem);
        const auto suffix_codepoints = utf8_codepoints_count(suffix);

        if (stem_codepoints <= suffix_codepoints + 3) {
            continue;
        }

        stem.erase(stem.size() - suffix.size());
        return stem;
    }

    return stem;
}

[[nodiscard]] bool recall_match_terms(const std::string_view lhs, const std::string_view rhs) {
    if (search_match_terms(lhs, rhs)) {
        return true;
    }

    const auto lhs_stem = make_recall_stem(lhs);
    const auto rhs_stem = make_recall_stem(rhs);

    if (lhs_stem == lhs && rhs_stem == rhs) {
        return false;
    }

    return search_match_terms(lhs_stem, rhs_stem);
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

[[nodiscard]] bool is_kept_short_search_term(const std::string_view term) noexcept {
    constexpr auto short_terms = std::array{
            std::string_view{"не"},
            std::string_view{"но"},
            std::string_view{"а"},
    };

    return std::ranges::find(short_terms, term) != short_terms.end();
}

[[nodiscard]] bool is_search_term_long_enough(const std::string_view term) noexcept {
    return utf8_codepoints_count(term) >= 3 || is_kept_short_search_term(term);
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
    /*
     * These are not domain words. They are only intent/noise words that users
     * naturally add around the real request: "как", "что делать если",
     * "каким образом", "нужно", etc.
     */
    constexpr auto weak_terms = std::array{
            std::string_view{"yclients"},    std::string_view{"что"},       std::string_view{"чем"},
            std::string_view{"че"},          std::string_view{"чё"},        std::string_view{"как"},
            std::string_view{"каким"},       std::string_view{"образом"},
            std::string_view{"где"},         std::string_view{"когда"},     std::string_view{"куда"},
            std::string_view{"зачем"},       std::string_view{"почему"},    std::string_view{"кто"},
            std::string_view{"кого"},        std::string_view{"кому"},      std::string_view{"какой"},
            std::string_view{"какая"},       std::string_view{"какое"},     std::string_view{"какие"},
            std::string_view{"для"},         std::string_view{"про"},       std::string_view{"при"},
            std::string_view{"без"},         std::string_view{"или"},       std::string_view{"если"},
            std::string_view{"чтобы"},       std::string_view{"надо"},      std::string_view{"нужно"},
            std::string_view{"нужен"},       std::string_view{"нужна"},     std::string_view{"нужны"},
            std::string_view{"можно"},       std::string_view{"сделать"},   std::string_view{"сделай"},
            std::string_view{"делать"},      std::string_view{"действия"},  std::string_view{"действовать"},
            std::string_view{"поступать"},   std::string_view{"поступить"}, std::string_view{"это"},
            std::string_view{"такое"},       std::string_view{"означает"},  std::string_view{"обозначает"},
            std::string_view{"значит"},      std::string_view{"объясни"},   std::string_view{"поясни"},
            std::string_view{"расскажи"},
            std::string_view{"определение"}, std::string_view{"термин"},    std::string_view{"понятие"},
            std::string_view{"значение"},    std::string_view{"подскажи"},  std::string_view{"помоги"},
            std::string_view{"пожалуйста"},  std::string_view{"проблема"},  std::string_view{"проблемы"},
            std::string_view{"могут"},       std::string_view{"может"},     std::string_view{"возникнуть"},
            std::string_view{"возникнут"},   std::string_view{"возникают"}, std::string_view{"появиться"},
            std::string_view{"появятся"},
    };

    return std::ranges::any_of(weak_terms,
                               [&](const std::string_view weak_term) { return search_match_terms(term, weak_term); });
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
    return std::ranges::any_of(terms, [&](const std::string &term) { return search_match_terms(query_term, term); });
}

[[nodiscard]] bool contains_all_terms_exactly(const std::span<const std::string> needles,
                                              const std::span<const std::string> haystack) noexcept {
    return std::ranges::all_of(needles, [&](const std::string &term) { return contains_term(haystack, term); });
}

[[nodiscard]] bool contains_all_terms_fuzzy(const std::span<const std::string> needles,
                                            const std::span<const std::string> haystack) noexcept {
    return std::ranges::all_of(needles, [&](const std::string &term) { return contains_term_fuzzy(haystack, term); });
}

[[nodiscard]] std::size_t count_exact_term_overlap(const std::span<const std::string> lhs,
                                                   const std::span<const std::string> rhs) noexcept {
    auto count = std::size_t{};

    for (const auto &term : lhs) {
        if (contains_term(rhs, term)) {
            ++count;
        }
    }

    return count;
}

[[nodiscard]] std::size_t count_fuzzy_term_overlap(const std::span<const std::string> lhs,
                                                   const std::span<const std::string> rhs) noexcept {
    auto count = std::size_t{};
    auto used_rhs = std::vector<bool>(rhs.size(), false);

    for (const auto &lhs_term : lhs) {
        for (auto index = std::size_t{}; index < rhs.size(); ++index) {
            if (used_rhs[index]) {
                continue;
            }

            if (!typo_match_terms(lhs_term, rhs[index])) {
                continue;
            }

            used_rhs[index] = true;
            ++count;
            break;
        }
    }

    return count;
}

[[nodiscard]] std::size_t bounded_subtract(const std::size_t value, const std::size_t delta) noexcept {
    return value > delta ? value - delta : std::size_t{};
}

[[nodiscard]] std::size_t dice_score(const std::size_t overlap,
                                     const std::size_t lhs_size,
                                     const std::size_t rhs_size,
                                     const std::size_t max_score) noexcept {
    if (overlap == 0 || lhs_size == 0 || rhs_size == 0) {
        return 0;
    }

    return (2 * overlap * max_score) / (lhs_size + rhs_size);
}

struct frequent_query_score_s {
    std::size_t score = {};
    knowledge_match_e match = knowledge_match_e::none;
};

[[nodiscard]] frequent_query_score_s score_frequent_query_terms(
        const std::span<const std::string> query_terms,
        const std::span<const std::string> frequent_query_terms) noexcept {
    if (query_terms.empty() || frequent_query_terms.empty()) {
        return {};
    }

    if (query_terms.size() == frequent_query_terms.size() &&
        contains_all_terms_exactly(query_terms, frequent_query_terms)) {
        return {.score = exact_frequent_query_score, .match = knowledge_match_e::exact_frequent_query};
    }

    const auto exact_overlap = count_exact_term_overlap(query_terms, frequent_query_terms);
    auto exact_score = dice_score(exact_overlap,
                                  query_terms.size(),
                                  frequent_query_terms.size(),
                                  unordered_frequent_query_score);

    if (exact_overlap != 0 && contains_all_terms_exactly(frequent_query_terms, query_terms)) {
        const auto extra_query_terms = query_terms.size() - frequent_query_terms.size();
        exact_score = std::max(exact_score, bounded_subtract(std::size_t{9200}, extra_query_terms * 96));
    }

    if (query_terms.size() >= 2 && contains_all_terms_exactly(query_terms, frequent_query_terms)) {
        const auto extra_frequent_query_terms = frequent_query_terms.size() - query_terms.size();
        exact_score = std::max(exact_score, bounded_subtract(std::size_t{9100}, extra_frequent_query_terms * 96));
    }

    const auto fuzzy_overlap = count_fuzzy_term_overlap(query_terms, frequent_query_terms);
    auto fuzzy_score = dice_score(fuzzy_overlap,
                                  query_terms.size(),
                                  frequent_query_terms.size(),
                                  unordered_fuzzy_frequent_query_score);

    if (fuzzy_overlap != 0 && contains_all_terms_fuzzy(frequent_query_terms, query_terms)) {
        const auto extra_query_terms = query_terms.size() > frequent_query_terms.size()
                                               ? query_terms.size() - frequent_query_terms.size()
                                               : std::size_t{};
        fuzzy_score = std::max(fuzzy_score, bounded_subtract(std::size_t{8400}, extra_query_terms * 96));
    }

    if (query_terms.size() >= 2 && contains_all_terms_fuzzy(query_terms, frequent_query_terms)) {
        const auto extra_frequent_query_terms = frequent_query_terms.size() > query_terms.size()
                                                        ? frequent_query_terms.size() - query_terms.size()
                                                        : std::size_t{};
        fuzzy_score = std::max(fuzzy_score, bounded_subtract(std::size_t{8300}, extra_frequent_query_terms * 96));
    }

    if (exact_score >= fuzzy_score) {
        return {.score = exact_score,
                .match = exact_score == 0 ? knowledge_match_e::none : knowledge_match_e::unordered_frequent_query};
    }

    return {.score = fuzzy_score, .match = knowledge_match_e::unordered_fuzzy_frequent_query};
}

[[nodiscard]] frequent_query_score_s score_glossary_heading_terms(
        const std::span<const std::string> query_terms,
        const std::span<const std::string> heading_terms) noexcept {
    if (query_terms.empty() || heading_terms.empty()) {
        return {};
    }

    /*
     * Glossary lookup is intentionally stricter than frequent-query lookup.
     * A bare term or a definition-style question may resolve to a definition,
     * but action queries such as "как посмотреть личный счет клиента" must not
     * be swallowed by a glossary heading just because the heading is a subset
     * of the query.
     */
    if (query_terms.size() != heading_terms.size()) {
        return {};
    }

    if (contains_all_terms_exactly(query_terms, heading_terms)) {
        return {.score = unordered_glossary_heading_score,
                .match = knowledge_match_e::unordered_glossary_heading};
    }

    if (contains_all_terms_fuzzy(query_terms, heading_terms)) {
        return {.score = unordered_fuzzy_glossary_heading_score,
                .match = knowledge_match_e::unordered_fuzzy_glossary_heading};
    }

    return {};
}

[[nodiscard]] bool erase_suffix_if_possible(std::string &term,
                                            const std::string_view suffix,
                                            const std::size_t min_stem_codepoints) {
    if (!term.ends_with(suffix)) {
        return false;
    }

    const auto stem_byte_size = term.size() - suffix.size();
    const auto stem = std::string_view{term}.substr(0, stem_byte_size);

    if (utf8_codepoints_count(stem) < min_stem_codepoints) {
        return false;
    }

    term.erase(stem_byte_size);
    return true;
}

[[nodiscard]] std::string make_loose_search_term(const std::string_view term) {
    auto result = std::string{term};

    /*
     * This is not a linguistic stemmer. It is a conservative retrieval helper
     * for Russian action queries:
     *
     *   "удалении записи"  -> should match "удалить запись"
     *   "изменении услуги" -> should match "изменить услугу"
     *
     * It is intentionally used only for ranked LLM context selection below,
     * not for direct instruction extraction. Therefore a broad analytical
     * question can select the right file for the model without being answered
     * by raw instruction inlining.
     */
    constexpr auto suffixes = std::array{
            std::string_view{"ениями"},
            std::string_view{"аниями"},
            std::string_view{"ением"},
            std::string_view{"анием"},
            std::string_view{"ении"},
            std::string_view{"ание"},
            std::string_view{"ания"},
            std::string_view{"анию"},
            std::string_view{"аний"},
            std::string_view{"ение"},
            std::string_view{"ения"},
            std::string_view{"ению"},
            std::string_view{"ений"},
            std::string_view{"овать"},
            std::string_view{"евать"},
            std::string_view{"ывать"},
            std::string_view{"ивать"},
            std::string_view{"нуть"},
            std::string_view{"ести"},
            std::string_view{"ить"},
            std::string_view{"ать"},
            std::string_view{"ять"},
            std::string_view{"еть"},
            std::string_view{"уть"},
    };

    for (const auto suffix : suffixes) {
        if (erase_suffix_if_possible(result, suffix, 4)) {
            return result;
        }
    }

    return result;
}

[[nodiscard]] bool loose_match_search_terms(const std::string_view lhs, const std::string_view rhs) {
    if (search_match_terms(lhs, rhs)) {
        return true;
    }

    const auto lhs_loose = make_loose_search_term(lhs);
    const auto rhs_loose = make_loose_search_term(rhs);

    if (lhs_loose == lhs && rhs_loose == rhs) {
        return false;
    }

    return search_match_terms(lhs_loose, rhs_loose);
}

[[nodiscard]] std::size_t count_loose_term_overlap(const std::span<const std::string> lhs,
                                                   const std::span<const std::string> rhs) {
    auto count = std::size_t{};
    auto used_rhs = std::vector<bool>(rhs.size(), false);

    for (const auto &lhs_term : lhs) {
        for (auto index = std::size_t{}; index < rhs.size(); ++index) {
            if (used_rhs[index]) {
                continue;
            }

            if (!loose_match_search_terms(lhs_term, rhs[index])) {
                continue;
            }

            used_rhs[index] = true;
            ++count;
            break;
        }
    }

    return count;
}

[[nodiscard]] bool contains_all_terms_loosely(const std::span<const std::string> needles,
                                              const std::span<const std::string> haystack) {
    return std::ranges::all_of(needles, [&](const std::string &term) {
        return std::ranges::any_of(haystack, [&](const std::string &candidate) {
            return loose_match_search_terms(term, candidate);
        });
    });
}

[[nodiscard]] std::size_t score_ranked_context_query_terms(
        const std::span<const std::string> query_terms,
        const std::span<const std::string> candidate_terms) {
    if (query_terms.empty() || candidate_terms.empty()) {
        return {};
    }

    const auto loose_overlap = count_loose_term_overlap(query_terms, candidate_terms);

    if (loose_overlap == 0) {
        return {};
    }

    const auto exact_overlap = count_exact_term_overlap(query_terms, candidate_terms);

    auto score = dice_score(loose_overlap, query_terms.size(), candidate_terms.size(), std::size_t{7200});
    score += exact_overlap * 192;

    if (loose_overlap >= 2) {
        score += 900;
    }

    if (contains_all_terms_loosely(candidate_terms, query_terms)) {
        const auto extra_query_terms = query_terms.size() > candidate_terms.size()
                                               ? query_terms.size() - candidate_terms.size()
                                               : std::size_t{};
        score = std::max(score, bounded_subtract(std::size_t{7200}, extra_query_terms * 96));
    }

    if (query_terms.size() >= 2 && contains_all_terms_loosely(query_terms, candidate_terms)) {
        const auto extra_candidate_terms = candidate_terms.size() > query_terms.size()
                                                   ? candidate_terms.size() - query_terms.size()
                                                   : std::size_t{};
        score = std::max(score, bounded_subtract(std::size_t{7000}, extra_candidate_terms * 96));
    }

    return score;
}

[[nodiscard]] std::size_t best_ranked_context_query_score(
        const knowledge_document_s &document,
        const std::span<const std::string> query_terms) {
    auto best = score_ranked_context_query_terms(query_terms, make_search_terms(document.normalized_title));

    for (const auto &frequent_query_terms : document.frequent_query_terms) {
        best = std::max(best, score_ranked_context_query_terms(query_terms, frequent_query_terms));
    }

    return best;
}

[[nodiscard]] frequent_query_score_s best_frequent_query_score(
        const knowledge_document_s &document,
        const std::span<const std::string> query_terms) noexcept {
    auto best = frequent_query_score_s{};

    for (const auto &frequent_query_terms : document.frequent_query_terms) {
        const auto score = score_frequent_query_terms(query_terms, frequent_query_terms);

        if (score.score > best.score) {
            best = score;
        }
    }

    return best;
}

[[nodiscard]] std::size_t count_exact_term_occurrences(const std::span<const std::string> terms,
                                                       const std::string_view term) noexcept {
    return static_cast<std::size_t>(std::ranges::count(terms, term));
}

[[nodiscard]] std::size_t count_recall_term_occurrences(const std::span<const std::string> terms,
                                                        const std::string_view query_term) {
    return static_cast<std::size_t>(std::ranges::count_if(terms, [&](const std::string &term) {
        return recall_match_terms(query_term, term);
    }));
}

[[nodiscard]] bool contains_term_by_recall(const std::span<const std::string> terms,
                                           const std::string_view query_term) {
    return std::ranges::any_of(terms, [&](const std::string &term) {
        return recall_match_terms(query_term, term);
    });
}

[[nodiscard]] bool contains_all_terms_by_recall(const std::span<const std::string> needles,
                                                const std::span<const std::string> haystack) {
    return std::ranges::all_of(needles, [&](const std::string &term) {
        return contains_term_by_recall(haystack, term);
    });
}

[[nodiscard]] std::size_t count_recall_term_overlap(const std::span<const std::string> lhs,
                                                    const std::span<const std::string> rhs) {
    auto count = std::size_t{};
    auto used_rhs = std::vector<bool>(rhs.size(), false);

    for (const auto &lhs_term : lhs) {
        for (auto index = std::size_t{}; index < rhs.size(); ++index) {
            if (used_rhs[index]) {
                continue;
            }

            if (!recall_match_terms(lhs_term, rhs[index])) {
                continue;
            }

            used_rhs[index] = true;
            ++count;
            break;
        }
    }

    return count;
}

[[nodiscard]] std::string join_search_terms(const std::span<const std::string> terms) {
    auto result = std::string{};

    for (const auto &term : terms) {
        if (!result.empty()) {
            result.push_back(' ');
        }

        result += term;
    }

    return result;
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

[[nodiscard]] std::size_t markdown_heading_level(const std::string_view line) {
    auto trimmed = std::string{line};
    util::trim(trimmed);

    auto level = std::size_t{};

    while (level < trimmed.size() && trimmed[level] == '#') {
        ++level;
    }

    if (level == 0 || level >= trimmed.size()) {
        return 0;
    }

    const auto separator = static_cast<unsigned char>(trimmed[level]);

    if (std::isspace(separator) == 0) {
        return 0;
    }

    return level;
}

[[nodiscard]] bool is_second_level_markdown_heading(const std::string_view line) {
    return markdown_heading_level(line) == 2;
}

[[nodiscard]] std::string raw_markdown_heading_text(const std::string_view line) {
    auto text = std::string{line};
    util::trim(text);

    while (!text.empty() && text.front() == '#') {
        text.erase(text.begin());
    }

    util::trim(text);

    return text;
}

void strip_balanced_marker_edges(std::string &text, const std::string_view marker) {
    while (text.size() >= marker.size() * 2 && text.starts_with(marker) && text.ends_with(marker)) {
        text.erase(0, marker.size());
        text.erase(text.size() - marker.size());
        util::trim(text);
    }
}

[[nodiscard]] std::string clean_glossary_term(const std::string_view heading_line) {
    auto term = raw_markdown_heading_text(heading_line);

    strip_balanced_marker_edges(term, "**");
    strip_balanced_marker_edges(term, "__");
    strip_balanced_marker_edges(term, "`");

    return term;
}

[[nodiscard]] bool is_glossary_file(const std::string_view relative_filename) {
    const auto filename_position = relative_filename.find_last_of('/');
    const auto filename = filename_position == std::string_view::npos ? relative_filename
                                                                      : relative_filename.substr(filename_position + 1);

    return filename.starts_with("glossary_");
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

void flush_glossary_entry(std::vector<knowledge_document_s> &result,
                          const std::string &relative_filename,
                          const std::string &term,
                          std::string body,
                          const knowledge_source_e source,
                          const workplace_role_e role) {
    if (term.empty()) {
        return;
    }

    util::trim(body);

    if (body.empty()) {
        return;
    }

    auto filename = std::format("{}#{}", relative_filename, term);

    auto entry = knowledge_document_s{
            .filename = std::move(filename),
            .title = term,
            .content = std::move(body),
            .frequent_queries = {},
            .normalized_filename = {},
            .normalized_title = {},
            .normalized_frequent_queries = {},
            .source = source,
            .role = role,
    };

    entry.normalized_filename = make_search_key(entry.filename);
    entry.normalized_title = make_search_key(entry.title);

    result.push_back(std::move(entry));
}

[[nodiscard]] std::vector<knowledge_document_s> extract_glossary_entries(const std::string_view content,
                                                                         const std::string &relative_filename,
                                                                         const knowledge_source_e source,
                                                                         const workplace_role_e role) {
    auto result = std::vector<knowledge_document_s>{};
    auto current_term = std::string{};
    auto current_body = std::string{};
    auto inside_entry = false;
    auto position = std::size_t{};

    while (position <= content.size()) {
        const auto line_end = content.find('\n', position);
        const auto line = content.substr(
                position,
                line_end == std::string_view::npos ? content.size() - position : line_end - position);

        if (is_second_level_markdown_heading(line)) {
            flush_glossary_entry(result, relative_filename, current_term, std::move(current_body), source, role);

            current_term = clean_glossary_term(line);
            current_body.clear();
            inside_entry = true;
        } else if (inside_entry) {
            if (!current_body.empty()) {
                current_body.push_back('\n');
            }

            current_body += line;
        }

        if (line_end == std::string_view::npos) {
            break;
        }

        position = line_end + 1;
    }

    flush_glossary_entry(result, relative_filename, current_term, std::move(current_body), source, role);

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

[[nodiscard]] std::vector<std::vector<std::string>> make_frequent_query_terms(
        const std::span<const std::string> normalized_queries) {
    auto result = std::vector<std::vector<std::string>>{};
    result.reserve(normalized_queries.size());

    for (const auto &query : normalized_queries) {
        auto terms = make_effective_search_terms(make_search_terms(query));

        if (!terms.empty()) {
            result.push_back(std::move(terms));
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
    if (directory_name == "general" || directory_name == "first_aid" || directory_name == "emergency") {
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
           match == knowledge_match_e::unordered_fuzzy_frequent_query ||
           match == knowledge_match_e::exact_glossary_heading ||
           match == knowledge_match_e::unordered_glossary_heading ||
           match == knowledge_match_e::unordered_fuzzy_glossary_heading;
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
        case knowledge_match_e::exact_glossary_heading: return "exact_glossary_heading";
        case knowledge_match_e::unordered_glossary_heading: return "unordered_glossary_heading";
        case knowledge_match_e::unordered_fuzzy_glossary_heading: return "unordered_fuzzy_glossary_heading";
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
    m_glossaries.clear();

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
        const auto source = detect_source_type(relative_filename);

        if (is_glossary_file(relative_filename)) {
            auto entries = extract_glossary_entries(content, relative_filename, source, *role);

            if (entries.empty()) {
                m_logger->warn("Glossary file has no usable level-2 entries: {}", relative_filename);
            }

            m_glossaries.insert(m_glossaries.end(),
                                std::make_move_iterator(entries.begin()),
                                std::make_move_iterator(entries.end()));
            continue;
        }

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
                .source = source,
                .role = *role,
        };

        document.normalized_filename = make_search_key(document.filename);
        document.normalized_title = make_search_key(document.title);
        document.normalized_frequent_queries = normalize_frequent_queries(document.frequent_queries);
        document.frequent_query_terms = make_frequent_query_terms(document.normalized_frequent_queries);

        m_documents.push_back(std::move(document));
    }

    std::ranges::sort(m_documents, {}, &knowledge_document_s::filename);
    std::ranges::sort(m_glossaries, {}, &knowledge_document_s::filename);

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

    m_logger->info("Loaded {} knowledge markdown files and {} glossary entries from '{}'",
                   m_documents.size(),
                   m_glossaries.size(),
                   m_directory.string());

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

    const auto terms = make_effective_search_terms(make_search_terms(normalized_query));

    if (terms.empty()) {
        return {};
    }

    auto frequent_query_results = std::vector<retrieved_knowledge_s>{};
    frequent_query_results.reserve(std::min(options.limit * 2, m_documents.size()));

    for (const auto &document : m_documents) {
        if (!document_matches_options(document, options)) {
            continue;
        }

        const auto frequent_score = best_frequent_query_score(document, terms);

        if (frequent_score.score < direct_frequent_query_min_score) {
            continue;
        }

        frequent_query_results.push_back(make_retrieved_document(document,
                                                                 frequent_score.score,
                                                                 options.max_chars_per_document,
                                                                 frequent_score.match));
    }

    if (!frequent_query_results.empty()) {
        std::ranges::sort(frequent_query_results,
                          [](const retrieved_knowledge_s &lhs, const retrieved_knowledge_s &rhs) {
                              if (lhs.score != rhs.score) {
                                  return lhs.score > rhs.score;
                              }

                              return lhs.filename < rhs.filename;
                          });

        frequent_query_results.resize(1);
        return frequent_query_results;
    }

    auto ranked_context_query_results = std::vector<retrieved_knowledge_s>{};
    ranked_context_query_results.reserve(std::min(options.limit * 2, m_documents.size()));

    for (const auto &document : m_documents) {
        if (!document_matches_options(document, options)) {
            continue;
        }

        const auto score = best_ranked_context_query_score(document, terms);

        if (score < ranked_context_query_min_score) {
            if (score != 0) {
                m_logger->debug("Knowledge candidate rejected by ranked context query threshold: {} score={} min_score={}",
                                document.filename,
                                score,
                                ranked_context_query_min_score);
            }

            continue;
        }

        ranked_context_query_results.push_back(
                make_retrieved_document(document, score, options.max_chars_per_document, knowledge_match_e::ranked));
    }

    if (!ranked_context_query_results.empty()) {
        std::ranges::sort(ranked_context_query_results,
                          [](const retrieved_knowledge_s &lhs, const retrieved_knowledge_s &rhs) {
                              if (lhs.score != rhs.score) {
                                  return lhs.score > rhs.score;
                              }

                              return lhs.filename < rhs.filename;
                          });

        const auto best_score = ranked_context_query_results.front().score;

        std::erase_if(ranked_context_query_results, [best_score](const retrieved_knowledge_s &item) noexcept {
            return item.score + ranked_context_query_secondary_gap < best_score;
        });

        if (ranked_context_query_results.size() > options.limit) {
            ranked_context_query_results.resize(options.limit);
        }

        return ranked_context_query_results;
    }

    auto ranked_results = std::vector<retrieved_knowledge_s>{};
    ranked_results.reserve(std::min(options.limit, m_documents.size()));

    for (const auto &document : m_documents) {
        if (!document_matches_options(document, options)) {
            continue;
        }

        const auto score = score_document(document, terms, normalized_query);

        if (score < options.min_ranked_score) {
            if (score != 0) {
                m_logger->debug("Knowledge candidate rejected by fallback score threshold: {} score={} min_score={}",
                                document.filename,
                                score,
                                options.min_ranked_score);
            }

            continue;
        }

        ranked_results.push_back(
                make_retrieved_document(document, score, options.max_chars_per_document, knowledge_match_e::ranked));
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

std::vector<retrieved_knowledge_s> KnowledgeStorage::retrieve_glossary(
        const std::string_view query,
        const knowledge_retrieve_options_s &options) const {
    if (m_glossaries.empty() || options.limit == 0) {
        return {};
    }

    const auto normalized_query = make_search_key(query);

    if (normalized_query.empty()) {
        return {};
    }

    const auto terms = make_effective_search_terms(make_search_terms(normalized_query));

    if (terms.empty()) {
        return {};
    }

    const auto normalized_effective_query = join_search_terms(terms);

    auto results = std::vector<retrieved_knowledge_s>{};
    results.reserve(std::min(options.limit, m_glossaries.size()));

    for (const auto &entry : m_glossaries) {
        if (!document_matches_options(entry, options)) {
            continue;
        }

        const auto heading_terms = make_search_terms(entry.normalized_title);
        auto score = score_glossary_heading_terms(terms, heading_terms);

        if (entry.normalized_title == normalized_query || entry.normalized_title == normalized_effective_query) {
            score = {.score = exact_glossary_heading_score, .match = knowledge_match_e::exact_glossary_heading};
        }

        if (score.score < direct_glossary_heading_min_score) {
            continue;
        }

        auto match = knowledge_match_e::none;

        switch (score.match) {
            case knowledge_match_e::exact_frequent_query: match = knowledge_match_e::exact_glossary_heading; break;
            case knowledge_match_e::unordered_frequent_query:
                match = knowledge_match_e::unordered_glossary_heading;
                break;
            case knowledge_match_e::unordered_fuzzy_frequent_query:
                match = knowledge_match_e::unordered_fuzzy_glossary_heading;
                break;
            case knowledge_match_e::exact_glossary_heading:
            case knowledge_match_e::unordered_glossary_heading:
            case knowledge_match_e::unordered_fuzzy_glossary_heading: match = score.match; break;
            case knowledge_match_e::none:
            case knowledge_match_e::ranked: continue;
        }

        results.push_back(make_retrieved_document(entry, score.score, options.max_chars_per_document, match));
    }

    std::ranges::sort(results, [](const retrieved_knowledge_s &lhs, const retrieved_knowledge_s &rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }

        return lhs.filename < rhs.filename;
    });

    if (results.size() > 1) {
        results.resize(1);
    }

    return results;
}

std::vector<retrieved_knowledge_s> KnowledgeStorage::retrieve_by_filenames(
        const std::span<const std::string> filenames,
        const knowledge_retrieve_options_s &options) const {
    if ((m_documents.empty() && m_glossaries.empty()) || filenames.empty() || options.limit == 0) {
        return {};
    }

    auto result = std::vector<retrieved_knowledge_s>{};
    auto seen = std::unordered_set<std::string>{};

    result.reserve(std::min(options.limit, filenames.size()));

    for (const auto &filename : filenames) {
        if (!seen.insert(filename).second) {
            continue;
        }

        const auto find_in = [&filename](const std::vector<knowledge_document_s> &documents) {
            return std::ranges::find_if(documents, [&filename](const knowledge_document_s &document) noexcept {
                return document.filename == filename;
            });
        };

        auto document_it = find_in(m_documents);
        const auto *document = document_it == m_documents.end() ? nullptr : &*document_it;

        if (document == nullptr) {
            auto glossary_it = find_in(m_glossaries);
            document = glossary_it == m_glossaries.end() ? nullptr : &*glossary_it;
        }

        if (document == nullptr) {
            m_logger->debug("Contextual knowledge source from history was not found: {}", filename);
            continue;
        }

        if (!document_matches_options(*document, options)) {
            m_logger->debug("Contextual knowledge source from history was rejected by options: {}", filename);
            continue;
        }

        result.push_back(
                make_retrieved_document(*document, 1, options.max_chars_per_document, knowledge_match_e::ranked));

        if (result.size() >= options.limit) {
            break;
        }
    }

    return result;
}

bool KnowledgeStorage::empty() const noexcept { return m_documents.empty() && m_glossaries.empty(); }

std::size_t KnowledgeStorage::size() const noexcept { return m_documents.size() + m_glossaries.size(); }

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

    const auto title_overlap = count_recall_term_overlap(terms, title_terms);

    if (title_overlap != 0) {
        score += dice_score(title_overlap, terms.size(), title_terms.size(), std::size_t{512});
    }

    if (contains_all_terms_by_recall(terms, title_terms)) {
        score += 512;
    }

    for (const auto &term : terms) {
        score += count_recall_term_occurrences(title_terms, term) * term.size() * 16;
    }

    auto best_frequent_query_score = std::size_t{};

    for (const auto &frequent_query : document.normalized_frequent_queries) {
        auto query_score = std::size_t{};
        const auto frequent_query_terms = make_search_terms(frequent_query);

        if (frequent_query == normalized_query) {
            query_score += 512;
        } else {
            if (frequent_query.find(normalized_query) != std::string::npos) {
                query_score += 384;
            }

            if (normalized_query.find(frequent_query) != std::string::npos) {
                query_score += 256;
            }
        }

        const auto frequent_query_overlap = count_recall_term_overlap(terms, frequent_query_terms);

        if (frequent_query_overlap != 0) {
            query_score += dice_score(frequent_query_overlap,
                                      terms.size(),
                                      frequent_query_terms.size(),
                                      std::size_t{640});
        }

        if (contains_all_terms_by_recall(terms, frequent_query_terms)) {
            query_score += 512;
        }

        for (const auto &term : terms) {
            query_score += count_recall_term_occurrences(frequent_query_terms, term) * term.size() * 8;
        }

        best_frequent_query_score = std::max(best_frequent_query_score, query_score);
    }

    score += best_frequent_query_score;

    return score;
}

} // namespace stz::intern