#include "knowledge_storage.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <charconv>
#include <format>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#include "util/file_io.hpp"
#include "util/string_helpers.hpp"

namespace stz::intern {

namespace {

[[nodiscard]] bool is_markdown_file(const std::filesystem::path &filename) {
    auto extension = filename.extension().string();

    std::ranges::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c) noexcept {
        return static_cast<char>(std::tolower(c));
    });

    return extension == ".md" || extension == ".markdown";
}

[[nodiscard]] bool starts_with_digit_prefix(std::string_view filename) noexcept {
    return filename.size() >= 2 && std::isdigit(static_cast<unsigned char>(filename[0])) != 0 &&
           std::isdigit(static_cast<unsigned char>(filename[1])) != 0;
}

[[nodiscard]] std::int32_t parse_file_number_prefix(std::string_view filename) noexcept {
    if (!starts_with_digit_prefix(filename)) {
        return -1;
    }

    auto value = std::int32_t{};
    const auto *first = filename.data();
    const auto *last = filename.data() + 2;

    const auto result = std::from_chars(first, last, value);

    if (result.ec != std::errc{}) {
        return -1;
    }

    return value;
}

[[nodiscard]] workplace_role_e role_from_file_number(const std::int32_t number) noexcept {
    if (number == 70) {
        return workplace_role_e::general;
    }

    if (number >= 1 && number <= 10) {
        return workplace_role_e::barista;
    }

    if (number >= 11 && number <= 20) {
        return workplace_role_e::cashier;
    }

    if (number >= 21 && number <= 30) {
        return workplace_role_e::reception;
    }

    if (number >= 31 && number <= 40) {
        return workplace_role_e::callcenter;
    }

    if (number >= 41 && number <= 50) {
        return workplace_role_e::seller;
    }

    if (number >= 51 && number <= 60) {
        return workplace_role_e::beauty_admin;
    }

    return workplace_role_e::general;
}

[[nodiscard]] bool is_policy_file(const std::string_view filename) noexcept {
    return filename.find("70_llm_answer_policy") != std::string_view::npos;
}

[[nodiscard]] knowledge_source_e detect_source_type(std::string_view relative_filename) noexcept {
    if (relative_filename.starts_with("custom/") || relative_filename.starts_with("custom\\") ||
        relative_filename.find("/custom/") != std::string_view::npos ||
        relative_filename.find("\\custom\\") != std::string_view::npos) {
        return knowledge_source_e::custom;
    }

    return knowledge_source_e::builtin;
}

[[nodiscard]] std::string normalize_for_search(const std::string_view text) {
    auto result = std::string{};
    result.reserve(text.size());

    for (const auto ch : text) {
        const auto byte = static_cast<unsigned char>(ch);

        if (byte < 128) {
            result.push_back(static_cast<char>(std::tolower(byte)));
        } else {
            result.push_back(ch);
        }
    }

    return result;
}

[[nodiscard]] bool is_ascii_separator(const unsigned char byte) noexcept {
    if (byte >= 128) {
        return false;
    }

    return std::isspace(byte) != 0 || std::ispunct(byte) != 0;
}

[[nodiscard]] std::vector<std::string> make_search_terms(const std::string_view query) {
    const auto normalized = normalize_for_search(query);

    auto terms = std::vector<std::string>{};
    auto current = std::string{};

    for (const auto ch : normalized) {
        const auto byte = static_cast<unsigned char>(ch);

        if (is_ascii_separator(byte)) {
            if (current.size() >= 3) {
                terms.push_back(std::move(current));
            }

            current.clear();
            continue;
        }

        current.push_back(ch);
    }

    if (current.size() >= 3) {
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

[[nodiscard]] std::size_t count_occurrences(std::string_view text, const std::string_view term) noexcept {
    if (term.empty()) {
        return 0;
    }

    auto count = std::size_t{};
    auto position = std::size_t{};

    while (true) {
        position = text.find(term, position);

        if (position == std::string_view::npos) {
            break;
        }

        ++count;
        position += term.size();
    }

    return count;
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

void append_unique_indices(std::vector<std::size_t> &target, std::span<const std::size_t> source) {
    auto seen = std::unordered_set<std::size_t>{};
    seen.reserve(target.size() + source.size());

    for (const auto index : target) {
        seen.insert(index);
    }

    for (const auto index : source) {
        if (seen.insert(index).second) {
            target.push_back(index);
        }
    }
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
        case workplace_role_e::all: return "all";
        case workplace_role_e::general: return "general";
        case workplace_role_e::barista: return "barista";
        case workplace_role_e::cashier: return "cashier";
        case workplace_role_e::reception: return "reception";
        case workplace_role_e::callcenter: return "callcenter";
        case workplace_role_e::seller: return "seller";
        case workplace_role_e::beauty_admin: return "beauty_admin";
    }

    return "all";
}

workplace_role_e workplace_role_from_string(std::string_view text) {
    auto normalized = normalize_for_search(text);
    util::trim(normalized);

    if (normalized == "all") {
        return workplace_role_e::all;
    }

    if (normalized == "general") {
        return workplace_role_e::general;
    }

    if (normalized == "barista" || normalized == "бариста") {
        return workplace_role_e::barista;
    }

    if (normalized == "cashier" || normalized == "кассир") {
        return workplace_role_e::cashier;
    }

    if (normalized == "reception" || normalized == "ресепшен" || normalized == "administrator") {
        return workplace_role_e::reception;
    }

    if (normalized == "callcenter" || normalized == "call_center" || normalized == "коллцентр") {
        return workplace_role_e::callcenter;
    }

    if (normalized == "seller" || normalized == "продавец") {
        return workplace_role_e::seller;
    }

    if (normalized == "beauty_admin" || normalized == "администратор салона") {
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

    m_general_indices.clear();
    m_barista_indices.clear();
    m_cashier_indices.clear();
    m_reception_indices.clear();
    m_callcenter_indices.clear();
    m_seller_indices.clear();
    m_beauty_admin_indices.clear();
    m_policy_indices.clear();

    if (!std::filesystem::exists(m_directory)) {
        m_logger->warn("Knowledge directory does not exist: {}", m_directory.string());
        return;
    }

    for (const auto &entry : std::filesystem::recursive_directory_iterator{m_directory}) {
        if (!entry.is_regular_file() || !is_markdown_file(entry.path())) {
            continue;
        }

        auto relative_filename = std::filesystem::relative(entry.path(), m_directory).generic_string();
        auto content = util::read_text_file(entry.path());
        auto title = extract_title(content, entry.path().filename().string());

        const auto number = parse_file_number_prefix(entry.path().filename().string());
        const auto role = role_from_file_number(number);
        const auto policy = is_policy_file(relative_filename);

        auto document = knowledge_document_s{
                .filename = std::move(relative_filename),
                .title = std::move(title),
                .content = std::move(content),
                .normalized_filename = {},
                .normalized_title = {},
                .normalized_content = {},
                .source = detect_source_type(relative_filename),
                .role = role,
                .policy = policy,
        };

        document.normalized_filename = normalize_for_search(document.filename);
        document.normalized_title = normalize_for_search(document.title);
        document.normalized_content = normalize_for_search(document.content);

        m_documents.push_back(std::move(document));
    }

    std::ranges::sort(m_documents, {}, &knowledge_document_s::filename);

    for (auto index = std::size_t{}; index < m_documents.size(); ++index) {
        const auto &document = m_documents[index];

        if (document.policy) {
            m_policy_indices.push_back(index);
            continue;
        }

        switch (document.role) {
            case workplace_role_e::all: [[fallthrough]];
            case workplace_role_e::general: m_general_indices.push_back(index); break;

            case workplace_role_e::barista: m_barista_indices.push_back(index); break;

            case workplace_role_e::cashier: m_cashier_indices.push_back(index); break;

            case workplace_role_e::reception: m_reception_indices.push_back(index); break;

            case workplace_role_e::callcenter: m_callcenter_indices.push_back(index); break;

            case workplace_role_e::seller: m_seller_indices.push_back(index); break;

            case workplace_role_e::beauty_admin: m_beauty_admin_indices.push_back(index); break;
        }
    }

    m_logger->info("Loaded {} knowledge markdown files from '{}'", m_documents.size(), m_directory.string());

    m_logger->info("Knowledge map: general={}, policy={}, barista={}, cashier={}, reception={}, callcenter={}, "
                   "seller={}, beauty_admin={}",
                   m_general_indices.size(),
                   m_policy_indices.size(),
                   m_barista_indices.size(),
                   m_cashier_indices.size(),
                   m_reception_indices.size(),
                   m_callcenter_indices.size(),
                   m_seller_indices.size(),
                   m_beauty_admin_indices.size());
}

std::vector<retrieved_knowledge_s> KnowledgeStorage::retrieve(const std::string_view query,
                                                              const knowledge_retrieve_options_s &options) const {
    if (m_documents.empty() || options.limit == 0) {
        return {};
    }

    const auto terms = make_search_terms(query);

    if (terms.empty()) {
        return {};
    }

    const auto candidate_indices = make_candidate_indices(options);

    auto results = std::vector<retrieved_knowledge_s>{};
    results.reserve(std::min(options.limit * 2, candidate_indices.size()));

    for (const auto document_index : candidate_indices) {
        assert(document_index < m_documents.size());

        const auto &document = m_documents[document_index];

        if (!options.include_custom && document.source == knowledge_source_e::custom) {
            continue;
        }

        const auto score = score_document(document, terms);

        if (score == 0) {
            continue;
        }

        results.push_back(retrieved_knowledge_s{
                .filename = document.filename,
                .title = document.title,
                .content = take_prefix_utf8_safe(document.content, options.max_chars_per_document),
                .score = score,
                .source = document.source,
                .role = document.role,
        });
    }

    std::ranges::sort(results, [](const retrieved_knowledge_s &lhs, const retrieved_knowledge_s &rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }

        return lhs.filename < rhs.filename;
    });

    if (results.size() > options.limit) {
        results.resize(options.limit);
    }

    return results;
}

bool KnowledgeStorage::empty() const noexcept { return m_documents.empty(); }

std::size_t KnowledgeStorage::size() const noexcept { return m_documents.size(); }

std::vector<std::size_t> KnowledgeStorage::make_candidate_indices(const knowledge_retrieve_options_s &options) const {
    auto result = std::vector<std::size_t>{};

    if (options.include_policy) {
        append_unique_indices(result, m_policy_indices);
    }

    if (options.include_general) {
        append_unique_indices(result, m_general_indices);
    }

    const auto append_role = [&](const std::vector<std::size_t> &indices) { append_unique_indices(result, indices); };

    switch (options.workplace_role) {
        case workplace_role_e::all:
            append_role(m_barista_indices);
            append_role(m_cashier_indices);
            append_role(m_reception_indices);
            append_role(m_callcenter_indices);
            append_role(m_seller_indices);
            append_role(m_beauty_admin_indices);
            break;

        case workplace_role_e::general: break;

        case workplace_role_e::barista: append_role(m_barista_indices); break;

        case workplace_role_e::cashier: append_role(m_cashier_indices); break;

        case workplace_role_e::reception: append_role(m_reception_indices); break;

        case workplace_role_e::callcenter: append_role(m_callcenter_indices); break;

        case workplace_role_e::seller: append_role(m_seller_indices); break;

        case workplace_role_e::beauty_admin: append_role(m_beauty_admin_indices); break;
    }

    return result;
}

std::size_t KnowledgeStorage::score_document(const knowledge_document_s &document,
                                             const std::span<const std::string> terms) noexcept {
    auto score = std::size_t{};

    for (const auto &term : terms) {
        score += count_occurrences(document.normalized_content, term) * term.size();

        if (document.normalized_filename.find(term) != std::string::npos) {
            score += 50;
        }

        if (document.normalized_title.find(term) != std::string::npos) {
            score += 100;
        }
    }

    return score;
}

} // namespace stz::intern