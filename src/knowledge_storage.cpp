#include "knowledge_storage.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <format>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#include "util/file_io.hpp"

namespace stz::intern {

namespace {

[[nodiscard]] bool is_markdown_file(const std::filesystem::path &filename) {
    auto extension = filename.extension().string();

    std::ranges::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c) noexcept {
        return std::tolower(c);
    });

    return extension == ".md" || extension == ".markdown";
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

    for (auto &term : terms) {
        if (seen.insert(term).second) {
            unique_terms.push_back(std::move(term));
        }
    }

    return unique_terms;
}

[[nodiscard]] std::size_t count_occurrences(std::string_view text, const std::string_view term) {
    if (term.empty()) {
        return 0;
    }

    auto count = 0zu;
    auto position = 0zu;

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
    auto position = 0zu;

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

[[nodiscard]] std::size_t score_document(const knowledge_document_s &document,
                                         const std::span<const std::string> terms) {
    const auto normalized_content = normalize_for_search(document.content);
    const auto normalized_filename = normalize_for_search(document.filename);
    const auto normalized_title = normalize_for_search(document.title);

    auto score = 0zu;

    for (const auto &term : terms) {
        score += count_occurrences(normalized_content, term) * term.size();

        if (normalized_filename.find(term) != std::string::npos) {
            score += 50;
        }

        if (normalized_title.find(term) != std::string::npos) {
            score += 100;
        }
    }

    return score;
}

} // namespace

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
        auto content = util::read_text_file(entry.path());

        auto document = knowledge_document_s{
                .filename = std::move(relative_filename),
                .title = extract_title(content, entry.path().filename().string()),
                .content = std::move(content),
        };

        m_documents.push_back(std::move(document));
    }

    std::ranges::sort(m_documents, {}, &knowledge_document_s::filename);

    m_logger->info("Loaded {} knowledge markdown files from '{}'", m_documents.size(), m_directory.string());
}

std::vector<retrieved_knowledge_s> KnowledgeStorage::retrieve(const std::string_view query,
                                                              const std::size_t limit,
                                                              const std::size_t max_chars_per_document) const {
    if (m_documents.empty() || limit == 0) {
        return {};
    }

    const auto terms = make_search_terms(query);

    if (terms.empty()) {
        return {};
    }

    auto results = std::vector<retrieved_knowledge_s>{};

    for (const auto &document : m_documents) {
        const auto score = score_document(document, terms);

        if (score == 0) {
            continue;
        }

        results.push_back(retrieved_knowledge_s{
                .filename = document.filename,
                .title = document.title,
                .content = take_prefix_utf8_safe(document.content, max_chars_per_document),
                .score = score,
        });
    }

    std::ranges::sort(results, [](const retrieved_knowledge_s &lhs, const retrieved_knowledge_s &rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }

        return lhs.filename < rhs.filename;
    });

    if (results.size() > limit) {
        results.resize(limit);
    }

    return results;
}

bool KnowledgeStorage::empty() const noexcept { return m_documents.empty(); }

std::size_t KnowledgeStorage::size() const noexcept { return m_documents.size(); }

} // namespace stz::intern