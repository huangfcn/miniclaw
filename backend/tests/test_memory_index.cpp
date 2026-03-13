#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include "../src/agent/memory_index.hpp"
#include "../src/agent/fiber_pool.hpp"
#include "../src/agent.hpp"
#include <spdlog/spdlog.h>

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::flush_on(spdlog::level::debug);
    spdlog::info("Starting MemoryIndex test...");

    // Initialize FiberPool
    spdlog::info("Initializing FiberPool...");
    FiberPool::instance().init(1, nullptr);
    spdlog::info("FiberPool initialized.");

    const std::string test_db = "test_memory_db";
    int test_dim = 1024;
    spdlog::info("Creating MemoryIndex instance...");
    MemoryIndex index(test_db, test_dim);
    spdlog::info("MemoryIndex instance created.");

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
    int exit_code = 0;

    FiberPool::instance().spawn([&]() {
        try {
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
            std::vector<SearchResult> results1;
            // Retry since indexing is async
            for (int i = 0; i < 50; i++) {
                results1 = index.search("fox", {}, 5);
                if (!results1.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (results1.empty()) {
                spdlog::error("Keyword search failed: no results for 'fox'");
                exit_code = 1;
            } else {
                assert(results1[0].id == doc1_id);
                spdlog::info("Keyword search successful: found 'fox' in {}", results1[0].id);
            }

            if (exit_code == 0) {
                spdlog::info("Testing vector search (Faiss)...");
                std::vector<float> query_emb(test_dim, 0.21f); 
                std::vector<SearchResult> results2;
                for (int i = 0; i < 50; i++) {
                    results2 = index.search("", query_emb, 5);
                    if (!results2.empty()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                if (results2.empty()) {
                    spdlog::error("Vector search failed: no results");
                    exit_code = 1;
                } else {
                    assert(results2[0].id == doc2_id);
                    spdlog::info("Vector search successful: found doc closest to query embedding: {}", results2[0].id);
                }
            }

            if (exit_code == 0) {
                spdlog::info("Testing hybrid search...");
                std::vector<float> query_emb(test_dim, 0.21f);
                auto results3 = index.search("programming", query_emb, 5);
                if (results3.empty() || results3[0].id != doc2_id) {
                     spdlog::error("Hybrid search failed");
                     exit_code = 1;
                } else {
                    spdlog::info("Hybrid search successful: found {} for query 'programming'", results3[0].id);
                }
            }

        } catch (const std::exception& e) {
            spdlog::error("Exception in test task: {}", e.what());
            exit_code = 1;
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            done = true;
            cv.notify_one();
        }
    });

    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&]{ return done; });

    FiberPool::instance().stop();

    if (exit_code == 0) {
        spdlog::info("MemoryIndex test PASSED!");
    }
    return exit_code;
}
