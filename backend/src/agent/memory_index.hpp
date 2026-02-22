#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>

struct SearchResult {
    std::string id;
    std::string path;
    int start_line;
    int end_line;
    std::string text;
    float score;
    std::string source; // "sessions", "memory", "long-term"
};

class MemoryIndex {
public:
    explicit MemoryIndex(const std::string& index_path, int dimension = 1536);
    ~MemoryIndex();

    void add_document(
        const std::string& id,
        const std::string& path,
        int start_line,
        int end_line,
        const std::string& text,
        const std::vector<float>& embedding,
        const std::string& source
    );

    std::vector<SearchResult> search(
        const std::string& query,
        const std::vector<float>& query_embedding,
        int top_k = 10
    );

    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
