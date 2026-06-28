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
constexpr auto tag_heading_min_score = std::size_t{5600};
constexpr auto compound_query_tag_focus_bonus = std::size_t{1800};

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
            std::string_view{"ениями"}, std::string_view{"ение"},  std::string_view{"ении"}, std::string_view{"ения"},
            std::string_view{"ению"},   std::string_view{"ением"}, std::string_view{"иями"}, std::string_view{"ями"},
            std::string_view{"ами"},    std::string_view{"ого"},   std::string_view{"ему"},  std::string_view{"ыми"},
            std::string_view{"ими"},    std::string_view{"ить"},   std::string_view{"ать"},  std::string_view{"ять"},
            std::string_view{"еть"},    std::string_view{"уть"},   std::string_view{"ешь"},  std::string_view{"ете"},
            std::string_view{"ют"},     std::string_view{"ет"},    std::string_view{"ит"},
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
            std::string_view{"а"},
            std::string_view{"я"},
            std::string_view{"у"},
            std::string_view{"ю"},
            std::string_view{"е"},
            std::string_view{"ы"},
            std::string_view{"и"},
            std::string_view{"ь"},
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
            std::string_view{"yclients"},    std::string_view{"что"},         std::string_view{"чем"},
            std::string_view{"че"},          std::string_view{"чё"},          std::string_view{"как"},
            std::string_view{"каким"},       std::string_view{"образом"},     std::string_view{"где"},
            std::string_view{"когда"},       std::string_view{"куда"},        std::string_view{"зачем"},
            std::string_view{"почему"},      std::string_view{"кто"},         std::string_view{"кого"},
            std::string_view{"кому"},        std::string_view{"какой"},       std::string_view{"какая"},
            std::string_view{"какое"},       std::string_view{"какие"},       std::string_view{"для"},
            std::string_view{"про"},         std::string_view{"при"},         std::string_view{"без"},
            std::string_view{"или"},         std::string_view{"если"},        std::string_view{"чтобы"},
            std::string_view{"надо"},        std::string_view{"нужно"},       std::string_view{"нужен"},
            std::string_view{"нужна"},       std::string_view{"нужны"},       std::string_view{"можно"},
            std::string_view{"сделать"},     std::string_view{"сделай"},      std::string_view{"делать"},
            std::string_view{"действия"},    std::string_view{"действовать"}, std::string_view{"поступать"},
            std::string_view{"поступить"},   std::string_view{"это"},         std::string_view{"такое"},
            std::string_view{"означает"},    std::string_view{"обозначает"},  std::string_view{"значит"},
            std::string_view{"объясни"},     std::string_view{"поясни"},      std::string_view{"расскажи"},
            std::string_view{"определение"}, std::string_view{"термин"},      std::string_view{"понятие"},
            std::string_view{"значение"},    std::string_view{"подскажи"},    std::string_view{"помоги"},
            std::string_view{"пожалуйста"},  std::string_view{"проблема"},    std::string_view{"проблемы"},
            std::string_view{"могут"},       std::string_view{"может"},       std::string_view{"возникнуть"},
            std::string_view{"возникнут"},   std::string_view{"возникают"},   std::string_view{"появиться"},
            std::string_view{"появятся"},
    };

    return std::ranges::any_of(weak_terms,
                               [&](const std::string_view weak_term) { return search_match_terms(term, weak_term); });
}

[[nodiscard]] std::vector<std::string> make_effective_search_terms(std::vector<std::string> terms) {
    std::erase_if(terms, [](const std::string &term) { return is_weak_intent_term(term); });

    return terms;
}

[[nodiscard]] bool is_tag_query_noise_term(const std::string_view term) noexcept {
    /*
     * Unlike document retrieval, section selection must preserve semantic
     * intent words such as "проблемы", "риски", "важно" and "учесть".
     * Only generic question scaffolding is removed here.
     */
    constexpr auto noise_terms = std::array{
            std::string_view{"yclients"},   std::string_view{"что"},       std::string_view{"чем"},
            std::string_view{"че"},         std::string_view{"чё"},        std::string_view{"как"},
            std::string_view{"каким"},      std::string_view{"образом"},  std::string_view{"где"},
            std::string_view{"когда"},      std::string_view{"куда"},     std::string_view{"зачем"},
            std::string_view{"почему"},     std::string_view{"кто"},      std::string_view{"кого"},
            std::string_view{"кому"},       std::string_view{"какой"},    std::string_view{"какая"},
            std::string_view{"какое"},      std::string_view{"какие"},    std::string_view{"для"},
            std::string_view{"про"},        std::string_view{"при"},      std::string_view{"без"},
            std::string_view{"или"},        std::string_view{"если"},     std::string_view{"чтобы"},
            std::string_view{"надо"},       std::string_view{"нужно"},    std::string_view{"нужен"},
            std::string_view{"нужна"},      std::string_view{"нужны"},    std::string_view{"можно"},
            std::string_view{"могут"},      std::string_view{"может"},    std::string_view{"возникнуть"},
            std::string_view{"возникнут"},  std::string_view{"возникают"}, std::string_view{"появиться"},
            std::string_view{"появятся"},   std::string_view{"быть"},     std::string_view{"будет"},
            std::string_view{"это"},        std::string_view{"такое"},    std::string_view{"подскажи"},
            std::string_view{"пожалуйста"}, std::string_view{"расскажи"}, std::string_view{"объясни"},
    };

    return std::ranges::find(noise_terms, term) != noise_terms.end();
}

[[nodiscard]] std::vector<std::string> make_tag_query_terms(const std::span<const std::string> terms) {
    auto result = std::vector<std::string>{};
    result.reserve(terms.size());

    for (const auto &term : terms) {
        if (!is_tag_query_noise_term(term)) {
            result.push_back(term);
        }
    }

    return result;
}

[[nodiscard]] std::vector<std::string> make_tag_heading_terms(const std::string_view tag_name) {
    return make_search_terms(tag_name);
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
        return {.score = unordered_glossary_heading_score, .match = knowledge_match_e::unordered_glossary_heading};
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
            std::string_view{"ениями"}, std::string_view{"аниями"}, std::string_view{"ением"},
            std::string_view{"анием"},  std::string_view{"ении"},   std::string_view{"ание"},
            std::string_view{"ания"},   std::string_view{"анию"},   std::string_view{"аний"},
            std::string_view{"ение"},   std::string_view{"ения"},   std::string_view{"ению"},
            std::string_view{"ений"},   std::string_view{"овать"},  std::string_view{"евать"},
            std::string_view{"ывать"},  std::string_view{"ивать"},  std::string_view{"нуть"},
            std::string_view{"ести"},   std::string_view{"ить"},    std::string_view{"ать"},
            std::string_view{"ять"},    std::string_view{"еть"},    std::string_view{"уть"},
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

[[nodiscard]] std::size_t score_ranked_context_query_terms(const std::span<const std::string> query_terms,
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

[[nodiscard]] std::size_t best_ranked_context_query_score(const knowledge_document_s &document,
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

[[nodiscard]] std::size_t count_recall_term_occurrences(const std::span<const std::string> terms,
                                                        const std::string_view query_term) {
    return static_cast<std::size_t>(std::ranges::count_if(terms, [&](const std::string &term) {
        return recall_match_terms(query_term, term);
    }));
}

[[nodiscard]] bool contains_term_by_recall(const std::span<const std::string> terms,
                                           const std::string_view query_term) {
    return std::ranges::any_of(terms, [&](const std::string &term) { return recall_match_terms(query_term, term); });
}

[[nodiscard]] bool contains_all_terms_by_recall(const std::span<const std::string> needles,
                                                const std::span<const std::string> haystack) {
    return std::ranges::all_of(needles,
                               [&](const std::string &term) { return contains_term_by_recall(haystack, term); });
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

enum class knowledge_tag_priority_e : std::size_t {
    instruction = 0,
    problems = 1,
    critical = 2,
    important = 3,
    consider = 4,
    other = 5,
};

[[nodiscard]] bool tag_name_contains_any(const knowledge_tag_content_s &tag,
                                         const std::span<const std::string_view> terms) {
    return std::ranges::any_of(terms, [&](const std::string_view term) {
        return contains_term_by_recall(tag.search_terms, term);
    });
}

[[nodiscard]] knowledge_tag_priority_e classify_knowledge_tag(const knowledge_tag_content_s &tag) {
    constexpr auto instruction_terms = std::array{
            std::string_view{"инструкция"},
            std::string_view{"пошаговая"},
            std::string_view{"шаги"},
            std::string_view{"порядок"},
    };

    if (tag_name_contains_any(tag, std::span{instruction_terms})) {
        return knowledge_tag_priority_e::instruction;
    }

    constexpr auto problem_terms = std::array{
            std::string_view{"проблема"},
            std::string_view{"проблемы"},
    };

    if (tag_name_contains_any(tag, std::span{problem_terms})) {
        return knowledge_tag_priority_e::problems;
    }

    constexpr auto critical_terms = std::array{
            std::string_view{"критически"},
            std::string_view{"критичное"},
            std::string_view{"критично"},
    };

    if (tag_name_contains_any(tag, std::span{critical_terms})) {
        return knowledge_tag_priority_e::critical;
    }

    constexpr auto important_terms = std::array{
            std::string_view{"важно"},
            std::string_view{"важный"},
            std::string_view{"важная"},
    };

    if (tag_name_contains_any(tag, std::span{important_terms})) {
        return knowledge_tag_priority_e::important;
    }

    constexpr auto consider_terms = std::array{
            std::string_view{"учесть"},
            std::string_view{"учитывать"},
            std::string_view{"внимание"},
    };

    if (tag_name_contains_any(tag, std::span{consider_terms})) {
        return knowledge_tag_priority_e::consider;
    }

    return knowledge_tag_priority_e::other;
}

[[nodiscard]] bool query_contains_any_by_recall(const std::span<const std::string> query_terms,
                                                const std::span<const std::string_view> expected_terms) {
    return std::ranges::any_of(expected_terms, [&](const std::string_view expected_term) {
        return contains_term_by_recall(query_terms, expected_term);
    });
}

[[nodiscard]] bool has_instruction_tag_intent(const std::string_view normalized_query,
                                              const std::span<const std::string> query_terms) {
    constexpr auto prefixes = std::array{
            std::string_view{"как "},
            std::string_view{"а как "},
            std::string_view{"каким образом "},
            std::string_view{"а каким образом "},
            std::string_view{"что делать"},
            std::string_view{"а что делать"},
    };

    if (std::ranges::any_of(prefixes,
                            [&](const std::string_view prefix) { return normalized_query.starts_with(prefix); })) {
        return true;
    }

    constexpr auto terms = std::array{
            std::string_view{"инструкция"},
            std::string_view{"пошагово"},
            std::string_view{"шаги"},
            std::string_view{"порядок"},
    };

    return query_contains_any_by_recall(query_terms, std::span{terms});
}

[[nodiscard]] std::size_t preferred_tag_intent_score(const knowledge_tag_priority_e priority,
                                                     const std::string_view normalized_query,
                                                     const std::span<const std::string> query_terms) {
    switch (priority) {
        case knowledge_tag_priority_e::instruction:
            return has_instruction_tag_intent(normalized_query, query_terms) ? std::size_t{7000} : std::size_t{};

        case knowledge_tag_priority_e::problems: {
            constexpr auto terms = std::array{
                    std::string_view{"проблема"},
                    std::string_view{"ошибка"},
                    std::string_view{"риск"},
                    std::string_view{"сложность"},
                    std::string_view{"затруднение"},
            };

            return query_contains_any_by_recall(query_terms, std::span{terms}) ? std::size_t{9000} : std::size_t{};
        }

        case knowledge_tag_priority_e::critical: {
            constexpr auto terms = std::array{
                    std::string_view{"критически"},
                    std::string_view{"критично"},
                    std::string_view{"критичный"},
            };

            return query_contains_any_by_recall(query_terms, std::span{terms}) ? std::size_t{9000} : std::size_t{};
        }

        case knowledge_tag_priority_e::important: {
            constexpr auto terms = std::array{
                    std::string_view{"важно"},
                    std::string_view{"важный"},
                    std::string_view{"обязательно"},
            };

            return query_contains_any_by_recall(query_terms, std::span{terms}) ? std::size_t{8800} : std::size_t{};
        }

        case knowledge_tag_priority_e::consider: {
            constexpr auto terms = std::array{
                    std::string_view{"учесть"},
                    std::string_view{"учитывать"},
                    std::string_view{"внимание"},
            };

            return query_contains_any_by_recall(query_terms, std::span{terms}) ? std::size_t{8600} : std::size_t{};
        }

        case knowledge_tag_priority_e::other: return {};
    }

    return {};
}

[[nodiscard]] bool is_section_intent_term(const std::string_view term) {
    constexpr auto terms = std::array{
            std::string_view{"проблема"},      std::string_view{"ошибка"},       std::string_view{"риск"},
            std::string_view{"нюанс"},         std::string_view{"ограничение"},  std::string_view{"исключение"},
            std::string_view{"последствие"},   std::string_view{"причина"},      std::string_view{"решение"},
            std::string_view{"пример"},        std::string_view{"проверка"},     std::string_view{"предупреждение"},
            std::string_view{"требование"},    std::string_view{"важно"},       std::string_view{"критично"},
            std::string_view{"учесть"},        std::string_view{"внимание"},    std::string_view{"документы"},
    };

    return std::ranges::any_of(terms,
                               [&](const std::string_view expected) { return recall_match_terms(term, expected); });
}

[[nodiscard]] std::size_t score_tag_heading(const knowledge_tag_content_s &tag,
                                            const std::span<const std::string> query_terms) {
    if (tag.search_terms.empty() || query_terms.empty()) {
        return {};
    }

    const auto recall_overlap = count_recall_term_overlap(tag.search_terms, query_terms);

    if (recall_overlap == 0) {
        return {};
    }

    const auto exact_overlap = count_exact_term_overlap(tag.search_terms, query_terms);

    auto score = recall_overlap * 6200 / tag.search_terms.size();
    score += recall_overlap * 2200 / query_terms.size();
    score += exact_overlap * 300;

    if (contains_all_terms_by_recall(tag.search_terms, query_terms)) {
        score += 900;
    }

    const auto section_intent_match = std::ranges::any_of(tag.search_terms, [&](const std::string &tag_term) {
        return is_section_intent_term(tag_term) && contains_term_by_recall(query_terms, tag_term);
    });

    if (section_intent_match) {
        score += 1800;
    }

    if (tag.search_terms.size() == 1 && query_terms.size() > 2 &&
        !is_section_intent_term(tag.search_terms.front())) {
        score = std::min(score, tag_heading_min_score - 1);
    }

    return std::min(score, exact_frequent_query_score);
}

[[nodiscard]] std::vector<std::string> make_tag_specific_query_terms(
        const knowledge_tag_content_s &tag,
        const std::span<const std::string> query_terms) {
    auto result = std::vector<std::string>{};
    result.reserve(query_terms.size());

    for (const auto &query_term : query_terms) {
        if (contains_term_by_recall(tag.search_terms, query_term)) {
            result.push_back(query_term);
        }
    }

    return result;
}

[[nodiscard]] bool can_score_tag_specific_query(const knowledge_tag_content_s &tag,
                                                const std::span<const std::string> query_terms) noexcept {
    if (query_terms.size() >= 2) {
        return true;
    }

    return query_terms.size() == 1 && tag.search_terms.size() == 1;
}

struct selected_knowledge_tag_s {
    const knowledge_tag_name_t *name = nullptr;
    const knowledge_tag_content_s *tag = nullptr;
    std::vector<std::string> matched_query_terms = {};
    std::size_t score = {};
    knowledge_tag_priority_e priority = knowledge_tag_priority_e::other;
};

void consider_knowledge_tag(selected_knowledge_tag_s &best,
                            const knowledge_tag_name_t &tag_name,
                            const knowledge_tag_content_s &tag,
                            const std::string_view normalized_query,
                            const std::span<const std::string> query_terms) {
    const auto priority = classify_knowledge_tag(tag);
    auto matched_query_terms = make_tag_specific_query_terms(tag, query_terms);

    const auto full_query_lexical_score = tag.normalized_name == normalized_query
                                                  ? exact_frequent_query_score
                                                  : score_tag_heading(tag, query_terms);
    auto focused_query_lexical_score = can_score_tag_specific_query(tag, matched_query_terms)
                                               ? score_tag_heading(tag, matched_query_terms)
                                               : std::size_t{};

    if (priority == knowledge_tag_priority_e::other && matched_query_terms.size() >= 2 &&
        matched_query_terms.size() < query_terms.size()) {
        focused_query_lexical_score = std::min(exact_frequent_query_score,
                                               focused_query_lexical_score + compound_query_tag_focus_bonus);
    }

    const auto lexical_score = std::max(full_query_lexical_score, focused_query_lexical_score);
    const auto intent_score = preferred_tag_intent_score(priority, normalized_query, query_terms);
    const auto score = std::max(lexical_score, intent_score);

    if (score < tag_heading_min_score) {
        return;
    }

    if (best.tag != nullptr &&
        (score < best.score || (score == best.score && priority >= best.priority))) {
        return;
    }

    best = {
            .name = &tag_name,
            .tag = &tag,
            .matched_query_terms = std::move(matched_query_terms),
            .score = score,
            .priority = priority,
    };
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

[[nodiscard]] std::string clean_glossary_term(std::string term) {
    util::trim(term);

    strip_balanced_marker_edges(term, "**");
    strip_balanced_marker_edges(term, "__");
    strip_balanced_marker_edges(term, "`");

    return term;
}

[[nodiscard]] std::vector<std::string> extract_glossary_aliases(const std::string_view heading_line) {
    auto heading = clean_glossary_term(raw_markdown_heading_text(heading_line));
    auto result = std::vector<std::string>{};
    auto seen = std::unordered_set<std::string>{};
    auto position = std::size_t{};

    while (position <= heading.size()) {
        const auto separator = heading.find(';', position);
        auto alias = clean_glossary_term(
                heading.substr(position,
                               separator == std::string::npos ? heading.size() - position : separator - position));
        auto normalized_alias = make_search_key(alias);

        if (!normalized_alias.empty() && seen.insert(normalized_alias).second) {
            result.push_back(std::move(alias));
        }

        if (separator == std::string::npos) {
            break;
        }

        position = separator + 1;
    }

    return result;
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

[[nodiscard]] std::vector<std::string> normalize_search_keys(const std::span<const std::string> values) {
    auto result = std::vector<std::string>{};
    auto seen = std::unordered_set<std::string>{};

    result.reserve(values.size());

    for (const auto &value : values) {
        auto normalized = make_search_key(value);

        if (normalized.empty()) {
            continue;
        }

        if (seen.insert(normalized).second) {
            result.push_back(std::move(normalized));
        }
    }

    return result;
}

void remove_markdown_bullet_marker(std::string &line) {
    util::trim(line);

    if (line.size() >= 2 && (line.starts_with("- ") || line.starts_with("* "))) {
        line.erase(0, 2);
        util::trim(line);
    }
}

void remove_inline_markdown_markers(std::string &line) {
    std::erase_if(line, [](const char ch) noexcept { return ch == '*' || ch == '_' || ch == '`'; });
}

[[nodiscard]] std::string make_model_content(const std::string_view markdown) {
    auto result = std::string{};
    auto position = std::size_t{};

    while (position <= markdown.size()) {
        const auto line_end = markdown.find('\n', position);
        const auto line_view = markdown.substr(
                position,
                line_end == std::string_view::npos ? markdown.size() - position : line_end - position);

        auto line = std::string{line_view};
        util::trim(line);

        if (!line.empty()) {
            if (is_markdown_heading(line)) {
                line = raw_markdown_heading_text(line);
            } else {
                remove_markdown_bullet_marker(line);
            }

            remove_inline_markdown_markers(line);
            collapse_ascii_spaces(line);
            util::trim(line);

            if (!line.empty()) {
                if (!result.empty()) {
                    result.push_back(' ');
                }

                result += line;
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }

        position = line_end + 1;
    }

    return result;
}

struct parsed_knowledge_tags_s {
    knowledge_tags_t tags = {};
    std::vector<knowledge_tag_name_t> order = {};
};

void flush_knowledge_tag(parsed_knowledge_tags_s &result, std::string tag_name, std::string body) {
    util::trim(tag_name);
    util::trim(body);

    if (tag_name.empty() || body.empty() || make_search_key(tag_name) == "частые запросы пользователя") {
        return;
    }

    auto tag = knowledge_tag_content_s{
            .content = std::move(body),
            .model_content = {},
            .normalized_name = make_search_key(tag_name),
            .search_terms = make_tag_heading_terms(tag_name),
    };
    tag.model_content = make_model_content(tag.content);

    const auto [it, inserted] = result.tags.try_emplace(tag_name, std::move(tag));

    if (inserted) {
        result.order.push_back(std::move(tag_name));
        return;
    }

    if (!it->second.content.empty()) {
        it->second.content += "\n\n";
    }
    it->second.content += tag.content;

    if (!it->second.model_content.empty() && !tag.model_content.empty()) {
        it->second.model_content.push_back(' ');
    }
    it->second.model_content += tag.model_content;
}

[[nodiscard]] parsed_knowledge_tags_s extract_knowledge_tags(const std::string_view content) {
    auto result = parsed_knowledge_tags_s{};
    auto current_tag = std::string{};
    auto current_body = std::string{};
    auto inside_tag = false;
    auto position = std::size_t{};

    while (position <= content.size()) {
        const auto line_end = content.find('\n', position);
        const auto line = content.substr(
                position,
                line_end == std::string_view::npos ? content.size() - position : line_end - position);

        if (is_second_level_markdown_heading(line)) {
            if (inside_tag) {
                flush_knowledge_tag(result, std::move(current_tag), std::move(current_body));
            }

            current_tag = clean_glossary_term(raw_markdown_heading_text(line));
            current_body.clear();
            inside_tag = true;
        } else if (inside_tag) {
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

    if (inside_tag) {
        flush_knowledge_tag(result, std::move(current_tag), std::move(current_body));
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

[[nodiscard]] selected_knowledge_tag_s find_default_knowledge_tag(const knowledge_document_s &document) {
    constexpr auto priorities = std::array{
            knowledge_tag_priority_e::instruction,
            knowledge_tag_priority_e::problems,
            knowledge_tag_priority_e::critical,
            knowledge_tag_priority_e::important,
            knowledge_tag_priority_e::consider,
    };

    for (const auto priority : priorities) {
        for (const auto &tag_name : document.tag_order) {
            const auto it = document.tags.find(tag_name);

            if (it == document.tags.end()) {
                assert(false);
                continue;
            }

            if (classify_knowledge_tag(it->second) == priority) {
                return {
                        .name = &it->first,
                        .tag = &it->second,
                        .score = {},
                        .priority = priority,
                };
            }
        }
    }

    if (document.tag_order.empty()) {
        return {};
    }

    const auto it = document.tags.find(document.tag_order.front());

    assert(it != document.tags.end());

    if (it == document.tags.end()) {
        std::terminate();
    }

    return {
            .name = &it->first,
            .tag = &it->second,
            .score = {},
            .priority = knowledge_tag_priority_e::other,
    };
}

[[nodiscard]] selected_knowledge_tag_s select_knowledge_tag(
        const knowledge_document_s &document,
        const std::string_view normalized_query,
        const std::span<const std::string> query_terms,
        const bool use_default_fallback) {
    auto best = selected_knowledge_tag_s{};

    constexpr auto preferred_priorities = std::array{
            knowledge_tag_priority_e::instruction,
            knowledge_tag_priority_e::problems,
            knowledge_tag_priority_e::critical,
            knowledge_tag_priority_e::important,
            knowledge_tag_priority_e::consider,
    };

    /*
     * Preferred built-in sections are evaluated first. Unknown/custom H2
     * headings are evaluated afterwards and may still win with a stronger
     * lexical match to the current query.
     */
    for (const auto priority : preferred_priorities) {
        for (const auto &tag_name : document.tag_order) {
            const auto it = document.tags.find(tag_name);

            if (it == document.tags.end()) {
                assert(false);
                continue;
            }

            if (classify_knowledge_tag(it->second) != priority) {
                continue;
            }

            consider_knowledge_tag(best, it->first, it->second, normalized_query, query_terms);
        }
    }

    for (const auto &tag_name : document.tag_order) {
        const auto it = document.tags.find(tag_name);

        if (it == document.tags.end()) {
            assert(false);
            continue;
        }

        if (classify_knowledge_tag(it->second) != knowledge_tag_priority_e::other) {
            continue;
        }

        consider_knowledge_tag(best, it->first, it->second, normalized_query, query_terms);
    }

    if (best.tag != nullptr || !use_default_fallback) {
        return best;
    }

    return find_default_knowledge_tag(document);
}

[[nodiscard]] std::string make_document_content(const knowledge_document_s &document, const bool for_model) {
    auto result = std::string{};

    for (const auto &tag_name : document.tag_order) {
        const auto it = document.tags.find(tag_name);

        if (it == document.tags.end()) {
            assert(false);
            continue;
        }

        const auto &content = for_model ? it->second.model_content : it->second.content;

        if (content.empty()) {
            continue;
        }

        if (!result.empty()) {
            result += for_model ? "\n" : "\n\n";
        }

        if (for_model) {
            result += std::format("Раздел «{}»: {}", tag_name, content);
        } else {
            result += std::format("## {}\n\n{}", tag_name, content);
        }
    }

    return result;
}

struct knowledge_query_parts_s {
    std::string document = {};
    std::string section = {};
};

[[nodiscard]] knowledge_query_parts_s make_knowledge_query_parts(
        const std::string_view normalized_query,
        const selected_knowledge_tag_s &selected_tag) {
    if (selected_tag.tag == nullptr || selected_tag.score < tag_heading_min_score ||
        selected_tag.matched_query_terms.empty()) {
        return {};
    }

    auto document_terms = make_tag_query_terms(make_search_terms(normalized_query));

    std::erase_if(document_terms, [&](const std::string &document_term) {
        return contains_term_by_recall(selected_tag.matched_query_terms, document_term);
    });

    if (document_terms.empty()) {
        return {};
    }

    auto result = knowledge_query_parts_s{
            .document = join_search_terms(document_terms),
            .section = join_search_terms(selected_tag.matched_query_terms),
    };

    if (result.document.empty() || result.section.empty() || result.document == result.section) {
        return {};
    }

    return result;
}

[[nodiscard]] retrieved_knowledge_s make_retrieved_document(const knowledge_file_path_t &file_path,
                                                            const knowledge_document_s &document,
                                                            const std::size_t score,
                                                            const std::size_t max_chars_per_document,
                                                            const knowledge_match_e match,
                                                            const std::string_view normalized_query,
                                                            const std::span<const std::string> tag_query_terms,
                                                            const bool use_default_tag_fallback) {
    const auto selected_tag = select_knowledge_tag(document,
                                                   normalized_query,
                                                   tag_query_terms,
                                                   use_default_tag_fallback);

    auto tag_name = std::string{};
    auto content = std::string{};
    auto direct_content = std::string{};
    const auto tag_matched_query = selected_tag.tag != nullptr && selected_tag.score >= tag_heading_min_score;
    const auto query_parts = make_knowledge_query_parts(normalized_query, selected_tag);

    if (selected_tag.tag != nullptr) {
        assert(selected_tag.name != nullptr);

        tag_name = *selected_tag.name;
        content = is_direct_match(match) ? selected_tag.tag->content : selected_tag.tag->model_content;

        if (tag_matched_query) {
            direct_content = selected_tag.tag->content;
        }
    } else {
        content = make_document_content(document, is_direct_match(match) ? false : true);
    }

    if (!is_direct_match(match)) {
        content = take_prefix_utf8_safe(content, max_chars_per_document);
    }

    return retrieved_knowledge_s{
            .filename = file_path.generic_string(),
            .title = document.title,
            .tag_name = std::move(tag_name),
            .content = std::move(content),
            .direct_content = std::move(direct_content),
            .document_query = query_parts.document,
            .section_query = query_parts.section,
            .score = score,
            .source = document.source,
            .role = document.role,
            .match = match,
            .tag_matched_query = tag_matched_query,
    };
}

struct knowledge_document_candidate_s {
    knowledge_file_path_t file_path = {};
    const knowledge_document_s *document = nullptr;
    std::size_t score = {};
    knowledge_match_e match = knowledge_match_e::none;
};

void sort_document_candidates(std::vector<knowledge_document_candidate_s> &candidates) {
    std::ranges::sort(candidates, [](const knowledge_document_candidate_s &lhs,
                                    const knowledge_document_candidate_s &rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }

        return lhs.file_path.generic_string() < rhs.file_path.generic_string();
    });
}

[[nodiscard]] std::vector<retrieved_knowledge_s> materialize_document_candidates(
        const std::span<const knowledge_document_candidate_s> candidates,
        const std::size_t max_chars_per_document,
        const std::string_view normalized_query,
        const std::span<const std::string> tag_query_terms) {
    auto result = std::vector<retrieved_knowledge_s>{};
    result.reserve(candidates.size());

    for (const auto &candidate : candidates) {
        assert(candidate.document != nullptr);

        if (candidate.document == nullptr) {
            std::terminate();
        }

        result.push_back(make_retrieved_document(candidate.file_path,
                                                 *candidate.document,
                                                 candidate.score,
                                                 max_chars_per_document,
                                                 candidate.match,
                                                 normalized_query,
                                                 tag_query_terms,
                                                 true));
    }

    return result;
}

[[nodiscard]] retrieved_knowledge_s make_retrieved_glossary_entry(
        const knowledge_glossary_entry_s &entry,
        const knowledge_document_s &document,
        const std::size_t score,
        const std::size_t max_chars_per_document,
        const knowledge_match_e match) {
    const auto tag_it = document.tags.find(entry.tag_name);

    assert(tag_it != document.tags.end());

    if (tag_it == document.tags.end()) {
        std::terminate();
    }

    auto content = is_direct_match(match) ? tag_it->second.content : tag_it->second.model_content;

    if (!is_direct_match(match)) {
        content = take_prefix_utf8_safe(content, max_chars_per_document);
    }

    return retrieved_knowledge_s{
            .filename = entry.filename,
            .title = entry.title,
            .tag_name = entry.tag_name,
            .content = std::move(content),
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

    auto total_tags = std::size_t{};
    auto glossary_files = std::size_t{};

    for (const auto &entry : std::filesystem::recursive_directory_iterator{m_directory}) {
        if (!entry.is_regular_file() || !is_markdown_file(entry.path())) {
            continue;
        }

        auto relative_path = std::filesystem::relative(entry.path(), m_directory).lexically_normal();
        auto relative_filename = relative_path.generic_string();
        const auto role = role_from_relative_filename(relative_filename);

        if (!role.has_value()) {
            m_logger->warn("Skipping knowledge file outside role directory: {}", relative_filename);
            continue;
        }

        const auto source = detect_source_type(relative_filename);
        const auto glossary = is_glossary_file(relative_filename);
        auto content = util::read_text_file(entry.path());
        auto parsed_tags = extract_knowledge_tags(content);

        if (parsed_tags.tags.empty()) {
            m_logger->warn("Knowledge file has no usable level-2 sections: {}", relative_filename);
            continue;
        }

        auto title = extract_title(content, entry.path().filename().string());
        auto frequent_queries = glossary ? std::vector<std::string>{} : extract_frequent_queries(content);

        auto document = knowledge_document_s{
                .title = std::move(title),
                .tags = std::move(parsed_tags.tags),
                .tag_order = std::move(parsed_tags.order),
                .frequent_queries = std::move(frequent_queries),
                .glossary_aliases = {},
                .normalized_filename = make_search_key(relative_filename),
                .normalized_title = {},
                .normalized_frequent_queries = {},
                .normalized_glossary_aliases = {},
                .frequent_query_terms = {},
                .source = source,
                .role = *role,
                .glossary = glossary,
        };

        document.normalized_title = make_search_key(document.title);
        document.normalized_frequent_queries = normalize_search_keys(document.frequent_queries);
        document.frequent_query_terms = make_frequent_query_terms(document.normalized_frequent_queries);

        if (glossary) {
            ++glossary_files;

            auto seen_aliases = std::unordered_set<std::string>{};

            for (const auto &tag_name : document.tag_order) {
                auto aliases = extract_glossary_aliases(tag_name);
                auto normalized_aliases = normalize_search_keys(aliases);

                for (const auto &alias : aliases) {
                    const auto normalized_alias = make_search_key(alias);

                    if (!normalized_alias.empty() && seen_aliases.insert(normalized_alias).second) {
                        document.glossary_aliases.push_back(alias);
                    }
                }

                const auto title = aliases.empty() ? tag_name : aliases.front();

                m_glossaries.push_back(knowledge_glossary_entry_s{
                        .file_path = relative_path,
                        .tag_name = tag_name,
                        .filename = std::format("{}#{}", relative_filename, title),
                        .title = title,
                        .aliases = std::move(aliases),
                        .normalized_aliases = std::move(normalized_aliases),
                });
            }

            document.normalized_glossary_aliases = normalize_search_keys(document.glossary_aliases);
        }

        total_tags += document.tags.size();

        const auto [_, inserted] = m_documents.emplace(std::move(relative_path), std::move(document));

        if (!inserted) {
            throw std::runtime_error{std::format("Duplicate knowledge file path '{}'", relative_filename)};
        }
    }

    std::ranges::sort(m_glossaries, {}, &knowledge_glossary_entry_s::filename);

    auto general_count = std::size_t{};
    auto barista_count = std::size_t{};
    auto seller_count = std::size_t{};
    auto beauty_admin_count = std::size_t{};
    auto custom_count = std::size_t{};
    auto documents_without_frequent_queries = std::size_t{};

    for (const auto &[_, document] : m_documents) {
        if (!document.glossary && document.normalized_frequent_queries.empty()) {
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

    m_logger->info("Loaded {} knowledge markdown files, {} cached H2 sections and {} glossary entries from '{}'",
                   m_documents.size(),
                   total_tags,
                   m_glossaries.size(),
                   m_directory.string());

    m_logger->info("Knowledge map: general={}, barista={}, seller={}, beauty_admin={}, custom={}, glossary_files={}",
                   general_count,
                   barista_count,
                   seller_count,
                   beauty_admin_count,
                   custom_count,
                   glossary_files);

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

    const auto search_terms = make_search_terms(normalized_query);
    const auto terms = make_effective_search_terms(search_terms);
    const auto tag_query_terms = make_tag_query_terms(search_terms);

    if (terms.empty()) {
        return {};
    }

    auto frequent_query_candidates = std::vector<knowledge_document_candidate_s>{};
    frequent_query_candidates.reserve(std::min(options.limit * 2, m_documents.size()));

    for (const auto &[file_path, document] : m_documents) {
        if (document.glossary || !document_matches_options(document, options)) {
            continue;
        }

        const auto frequent_score = best_frequent_query_score(document, terms);

        if (frequent_score.score < direct_frequent_query_min_score) {
            continue;
        }

        frequent_query_candidates.push_back(knowledge_document_candidate_s{
                .file_path = file_path,
                .document = &document,
                .score = frequent_score.score,
                .match = frequent_score.match,
        });
    }

    if (!frequent_query_candidates.empty()) {
        sort_document_candidates(frequent_query_candidates);
        frequent_query_candidates.resize(1);

        return materialize_document_candidates(frequent_query_candidates,
                                               options.max_chars_per_document,
                                               normalized_query,
                                               tag_query_terms);
    }

    auto ranked_context_query_candidates = std::vector<knowledge_document_candidate_s>{};
    ranked_context_query_candidates.reserve(std::min(options.limit * 2, m_documents.size()));

    for (const auto &[file_path, document] : m_documents) {
        if (document.glossary || !document_matches_options(document, options)) {
            continue;
        }

        const auto score = best_ranked_context_query_score(document, terms);

        if (score < ranked_context_query_min_score) {
            if (score != 0) {
                m_logger->debug(
                        "Knowledge candidate rejected by ranked context query threshold: {} score={} min_score={}",
                        file_path.generic_string(),
                        score,
                        ranked_context_query_min_score);
            }

            continue;
        }

        ranked_context_query_candidates.push_back(knowledge_document_candidate_s{
                .file_path = file_path,
                .document = &document,
                .score = score,
                .match = knowledge_match_e::ranked,
        });
    }

    if (!ranked_context_query_candidates.empty()) {
        sort_document_candidates(ranked_context_query_candidates);

        const auto best_score = ranked_context_query_candidates.front().score;

        std::erase_if(ranked_context_query_candidates,
                      [best_score](const knowledge_document_candidate_s &item) noexcept {
            return item.score + ranked_context_query_secondary_gap < best_score;
        });

        if (ranked_context_query_candidates.size() > options.limit) {
            ranked_context_query_candidates.resize(options.limit);
        }

        return materialize_document_candidates(ranked_context_query_candidates,
                                               options.max_chars_per_document,
                                               normalized_query,
                                               tag_query_terms);
    }

    auto ranked_candidates = std::vector<knowledge_document_candidate_s>{};
    ranked_candidates.reserve(std::min(options.limit, m_documents.size()));

    for (const auto &[file_path, document] : m_documents) {
        if (document.glossary || !document_matches_options(document, options)) {
            continue;
        }

        const auto score = score_document(document, terms, normalized_query);

        if (score < options.min_ranked_score) {
            if (score != 0) {
                m_logger->debug("Knowledge candidate rejected by fallback score threshold: {} score={} min_score={}",
                                file_path.generic_string(),
                                score,
                                options.min_ranked_score);
            }

            continue;
        }

        ranked_candidates.push_back(knowledge_document_candidate_s{
                .file_path = file_path,
                .document = &document,
                .score = score,
                .match = knowledge_match_e::ranked,
        });
    }

    sort_document_candidates(ranked_candidates);

    if (ranked_candidates.size() > options.limit) {
        ranked_candidates.resize(options.limit);
    }

    return materialize_document_candidates(ranked_candidates,
                                           options.max_chars_per_document,
                                           normalized_query,
                                           tag_query_terms);
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
        const auto document_it = m_documents.find(entry.file_path);

        if (document_it == m_documents.end()) {
            assert(false);
            continue;
        }

        const auto &document = document_it->second;

        if (!document_matches_options(document, options)) {
            continue;
        }

        auto score = frequent_query_score_s{};

        const auto score_alias = [&](const std::string_view normalized_alias) {
            auto alias_score = score_glossary_heading_terms(terms, make_search_terms(normalized_alias));

            if (normalized_alias == normalized_query || normalized_alias == normalized_effective_query) {
                alias_score = {
                        .score = exact_glossary_heading_score,
                        .match = knowledge_match_e::exact_glossary_heading,
                };
            }

            if (alias_score.score > score.score) {
                score = alias_score;
            }
        };

        if (entry.normalized_aliases.empty()) {
            score_alias(make_search_key(entry.title));
        } else {
            for (const auto &alias : entry.normalized_aliases) {
                score_alias(alias);
            }
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

        results.push_back(make_retrieved_glossary_entry(entry,
                                                        document,
                                                        score.score,
                                                        options.max_chars_per_document,
                                                        match));
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
    return retrieve_by_filenames(filenames, {}, options);
}

std::vector<retrieved_knowledge_s> KnowledgeStorage::retrieve_by_filenames(
        const std::span<const std::string> filenames,
        const std::string_view query,
        const knowledge_retrieve_options_s &options) const {
    if (m_documents.empty() || filenames.empty() || options.limit == 0) {
        return {};
    }

    auto result = std::vector<retrieved_knowledge_s>{};
    auto seen = std::unordered_set<std::string>{};
    const auto normalized_query = make_search_key(query);
    const auto tag_query_terms = make_tag_query_terms(make_search_terms(normalized_query));

    result.reserve(std::min(options.limit, filenames.size()));

    for (const auto &filename : filenames) {
        if (!seen.insert(filename).second) {
            continue;
        }

        if (const auto glossary_it = std::ranges::find(m_glossaries, filename, &knowledge_glossary_entry_s::filename);
            glossary_it != m_glossaries.end()) {
            const auto document_it = m_documents.find(glossary_it->file_path);

            if (document_it == m_documents.end()) {
                m_logger->debug("Contextual glossary source from history was not found: {}", filename);
                continue;
            }

            if (!document_matches_options(document_it->second, options)) {
                m_logger->debug("Contextual glossary source from history was rejected by options: {}", filename);
                continue;
            }

            result.push_back(make_retrieved_glossary_entry(*glossary_it,
                                                           document_it->second,
                                                           1,
                                                           options.max_chars_per_document,
                                                           knowledge_match_e::ranked));
        } else {
            const auto file_path = std::filesystem::path{filename, std::filesystem::path::generic_format}.lexically_normal();
            const auto document_it = m_documents.find(file_path);

            if (document_it == m_documents.end()) {
                m_logger->debug("Contextual knowledge source from history was not found: {}", filename);
                continue;
            }

            if (!document_matches_options(document_it->second, options)) {
                m_logger->debug("Contextual knowledge source from history was rejected by options: {}", filename);
                continue;
            }

            result.push_back(make_retrieved_document(document_it->first,
                                                     document_it->second,
                                                     1,
                                                     options.max_chars_per_document,
                                                     knowledge_match_e::ranked,
                                                     normalized_query,
                                                     tag_query_terms,
                                                     false));
        }

        if (result.size() >= options.limit) {
            break;
        }
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