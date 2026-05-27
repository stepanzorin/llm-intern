// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/logger.h>

namespace stz::intern {

struct knowledge_document_s {
    std::string filename = {};
    std::string title = {};
    std::string content = {};
};

struct retrieved_knowledge_s {
    std::string filename = {};
    std::string title = {};
    std::string content = {};
    std::size_t score = {};
};

class KnowledgeStorage final {
public:
    KnowledgeStorage(std::filesystem::path directory, std::shared_ptr<spdlog::logger> logger);

    void load();

    [[nodiscard]] std::vector<retrieved_knowledge_s> retrieve(
            std::string_view query,
            std::size_t limit,
            std::size_t max_chars_per_document) const;

    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::filesystem::path m_directory;
    std::vector<knowledge_document_s> m_documents;
    std::shared_ptr<spdlog::logger> m_logger;
};

} // namespace stz::intern