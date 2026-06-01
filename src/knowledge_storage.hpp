// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/logger.h>

namespace stz::intern {

enum class knowledge_source_e {
    builtin,
    custom,
};

enum class workplace_role_e {
    all,
    general,
    barista,
    cashier,
    reception,
    callcenter,
    seller,
    beauty_admin,
};

enum class knowledge_match_e {
    none,
    exact_frequent_query,
    unordered_frequent_query,
    ranked,
};

[[nodiscard]] std::string_view to_string(knowledge_source_e source) noexcept;

[[nodiscard]] std::string_view to_string(workplace_role_e role) noexcept;

[[nodiscard]] std::string_view to_string(knowledge_match_e match) noexcept;

[[nodiscard]] workplace_role_e workplace_role_from_string(std::string_view text);

struct knowledge_document_s {
    std::string filename = {};
    std::string title = {};
    std::string content = {};

    std::vector<std::string> frequent_queries = {};

    std::string normalized_filename = {};
    std::string normalized_title = {};
    std::vector<std::string> normalized_frequent_queries = {};

    knowledge_source_e source = knowledge_source_e::builtin;
    workplace_role_e role = workplace_role_e::general;

    bool policy = false;
};

struct retrieved_knowledge_s {
    std::string filename = {};
    std::string title = {};
    std::string content = {};
    std::size_t score = {};
    knowledge_source_e source = knowledge_source_e::builtin;
    workplace_role_e role = workplace_role_e::general;
    knowledge_match_e match = knowledge_match_e::none;
};

struct knowledge_retrieve_options_s {
    workplace_role_e workplace_role = workplace_role_e::all;
    bool include_general = true;
    bool include_policy = false;
    bool include_custom = true;
    std::size_t limit = 3;
    std::size_t max_chars_per_document = 3000;
};

class KnowledgeStorage final {
public:
    KnowledgeStorage(std::filesystem::path directory, std::shared_ptr<spdlog::logger> logger);

    void load();

    [[nodiscard]] std::vector<retrieved_knowledge_s> retrieve(std::string_view query,
                                                              const knowledge_retrieve_options_s &options) const;

    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

private:
    [[nodiscard]] std::vector<std::size_t> make_candidate_indices(const knowledge_retrieve_options_s &options) const;

    [[nodiscard]] static std::size_t score_document(const knowledge_document_s &document,
                                                    std::span<const std::string> terms,
                                                    std::string_view normalized_query) noexcept;

    [[nodiscard]] static bool has_exact_frequent_query_match(const knowledge_document_s &document,
                                                             std::string_view normalized_query) noexcept;

    [[nodiscard]] static bool has_unordered_frequent_query_match(const knowledge_document_s &document,
                                                                 std::span<const std::string> query_terms) noexcept;

    std::filesystem::path m_directory;
    std::vector<knowledge_document_s> m_documents;

    std::vector<std::size_t> m_general_indices;
    std::vector<std::size_t> m_barista_indices;
    std::vector<std::size_t> m_cashier_indices;
    std::vector<std::size_t> m_reception_indices;
    std::vector<std::size_t> m_callcenter_indices;
    std::vector<std::size_t> m_seller_indices;
    std::vector<std::size_t> m_beauty_admin_indices;
    std::vector<std::size_t> m_policy_indices;

    std::shared_ptr<spdlog::logger> m_logger;
};

} // namespace stz::intern