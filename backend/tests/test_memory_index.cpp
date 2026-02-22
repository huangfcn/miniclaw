#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include "../src/agent/memory_index.hpp"
#include <spdlog/spdlog.h>

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::info("Starting MemoryIndex test...");

    const std::string test_db = "test_memory_db";
    int test_dim = 1024;
    MemoryIndex index(test_db, test_dim);

    // Test data
    std::string doc1_id = "doc1";
    std::string doc1_text = "The quick brown fox jumps over the lazy dog.";
    std::vector<float> doc1_emb(test_dim, 0.1f);

    std::string doc2_id = "doc2";
    std::string doc2_text = "C++ is a high-performance programming language used in systems.";
    std::vector<float> doc2_emb(test_dim, 0.2f);

    spdlog::info("Indexing documents...");
    index.add_document(doc1_id, "file1.txt", 1, 1, doc1_text, doc1_emb, "session1");
    index.add_document(doc2_id, "file2.cpp", 1, 1, doc2_text, doc2_emb, "session1");

    spdlog::info("Testing keyword search (Lucene++)...");
    auto results1 = index.search("fox", {}, 5);
    if (results1.empty()) {
        spdlog::error("Keyword search failed: no results for 'fox'");
        return 1;
    }
    assert(results1[0].id == doc1_id);
    spdlog::info("Keyword search successful: found 'fox' in {}", results1[0].id);

    spdlog::info("Testing vector search (Faiss)...");
    // Query embedding close to doc2
    std::vector<float> query_emb(test_dim, 0.21f); 
    auto results2 = index.search("", query_emb, 5);
    if (results2.empty()) {
        spdlog::error("Vector search failed: no results");
        return 1;
    }
    assert(results2[0].id == doc2_id);
    spdlog::info("Vector search successful: found doc closest to query embedding: {}", results2[0].id);

    spdlog::info("Testing hybrid search...");
    auto results3 = index.search("programming", query_emb, 5);
    assert(!results3.empty());
    assert(results3[0].id == doc2_id);
    spdlog::info("Hybrid search successful: found {} for query 'programming'", results3[0].id);

    spdlog::info("MemoryIndex test PASSED!");
    return 0;
}
