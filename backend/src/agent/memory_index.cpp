#include "memory_index.hpp"
#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>
#include <cmath>
#include <fstream>
#include <chrono>
#include <ctime>
#include <regex>
#include <lucene++/LuceneHeaders.h>
#include <lucene++/IndexWriter.h>
#include <lucene++/IndexReader.h>
#include <lucene++/Directory.h>
#include <lucene++/FSDirectory.h>
#include <lucene++/StandardAnalyzer.h>
#include <lucene++/Document.h>
#include <lucene++/Field.h>
#include <lucene++/IndexSearcher.h>
#include <lucene++/QueryParser.h>
#include <lucene++/TopDocs.h>
#include <lucene++/ScoreDoc.h>
#include <lucene++/Term.h>
#include <filesystem>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;
using namespace Lucene;

struct MemoryIndex::Impl {
    std::string index_path;
    int dimension;
    std::unique_ptr<faiss::IndexFlatIP> faiss_index;
    std::vector<std::string> doc_ids;
    
    // Lucene++ components
    String lucene_path;
    AnalyzerPtr analyzer;
    DirectoryPtr directory;

    Impl(const std::string& path, int dim) : index_path(path), dimension(dim) {
        fs::create_directories(path);
        
        fs::path faiss_path = fs::path(path) / "faiss.index";
        if (fs::exists(faiss_path)) {
            try {
                faiss::Index* idx = faiss::read_index(faiss_path.string().c_str());
                faiss_index.reset(dynamic_cast<faiss::IndexFlatIP*>(idx));
                if (faiss_index && faiss_index->d != dimension) {
                    spdlog::warn("Faiss index dimension mismatch: found {}, expected {}. Resetting index.", faiss_index->d, dimension);
                    faiss_index.reset();
                }
            } catch (...) {
                spdlog::warn("Failed to load Faiss index from {}", faiss_path.string());
            }
        }
        
        if (!faiss_index) {
            faiss_index = std::make_unique<faiss::IndexFlatIP>(dimension);
        }

        fs::path ids_path = fs::path(path) / "doc_ids.txt";
        if (fs::exists(ids_path)) {
            std::ifstream f(ids_path);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty()) doc_ids.push_back(line);
            }
        }
        
        lucene_path = StringUtils::toUnicode(path + "/lucene");
        analyzer = newLucene<StandardAnalyzer>(LuceneVersion::LUCENE_CURRENT);
        directory = FSDirectory::open(lucene_path);
    }

    void add_doc(
        const std::string& id,
        const std::string& path,
        int start_line,
        int end_line,
        const std::string& text,
        const std::vector<float>& embedding,
        const std::string& source
    ) {
        // 1. Add to Faiss
        if ((int)embedding.size() == dimension) {
            faiss_index->add(1, embedding.data());
            doc_ids.push_back(id);
            
            // Persist Faiss
            try {
                fs::path faiss_path = fs::path(index_path) / "faiss.index";
                faiss::write_index(faiss_index.get(), faiss_path.string().c_str());
                
                fs::path ids_path = fs::path(index_path) / "doc_ids.txt";
                std::ofstream f(ids_path, std::ios::app);
                if (f.is_open()) f << id << "\n";
            } catch (...) {
                spdlog::warn("Failed to persist Faiss index");
            }
        }

        // 2. Add to Lucene++
        try {
            if (!directory || !analyzer) return;

            bool create = !IndexReader::indexExists(directory);
            IndexWriterPtr writer = newLucene<IndexWriter>(directory, analyzer, create, IndexWriter::MaxFieldLengthLIMITED);

            DocumentPtr doc = newLucene<Document>();
            doc->add(newLucene<Field>(StringUtils::toUnicode("id"), StringUtils::toUnicode(id), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("path"), StringUtils::toUnicode(path), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("text"), StringUtils::toUnicode(text), Field::STORE_YES, Field::INDEX_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("start_line"), StringUtils::toUnicode(std::to_string(start_line)), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("end_line"), StringUtils::toUnicode(std::to_string(end_line)), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("source"), StringUtils::toUnicode(source), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));

            writer->addDocument(doc);
            writer->close();
        } catch (...) {
            spdlog::warn("Lucene add_doc failed");
        }
    }

    std::vector<SearchResult> search(
        const std::string& query,
        const std::vector<float>& query_embedding,
        int top_k
    ) {
        std::map<std::string, float> vector_scores;
        std::map<std::string, float> text_scores;
        std::map<std::string, SearchResult> metadata;

        // 1. Vector Search
        if ((int)query_embedding.size() == dimension && faiss_index->ntotal > 0) {
            int candidate_k = top_k * 4; // Candidate multiplier
            std::vector<float> distances(candidate_k);
            std::vector<faiss::idx_t> labels(candidate_k);
            
            // Note: Faiss IndexFlatIP returns inner product.
            // If vectors are normalized, this is cosine similarity.
            faiss_index->search(1, query_embedding.data(), candidate_k, distances.data(), labels.data());

            for (int i = 0; i < candidate_k; ++i) {
                if (labels[i] >= 0 && labels[i] < (faiss::idx_t)doc_ids.size()) {
                    std::string id = doc_ids[labels[i]];
                    vector_scores[id] = distances[i];
                    // Metadata placeholder for now, will be filled by Lucene or if not present
                }
            }
        }

        // 2. Keyword Search
        if (!query.empty()) {
            try {
                IndexReaderPtr reader = IndexReader::open(directory);
                if (reader) {
                    IndexSearcherPtr searcher = newLucene<IndexSearcher>(reader);
                    QueryParserPtr parser = newLucene<QueryParser>(LuceneVersion::LUCENE_CURRENT, StringUtils::toUnicode("text"), analyzer);
                    QueryPtr lucene_query = parser->parse(StringUtils::toUnicode(query));
                    
                    int candidate_k = top_k * 4;
                    TopDocsPtr top_docs = searcher->search(lucene_query, candidate_k);

                    for (int i = 0; i < (int)top_docs->scoreDocs.size(); ++i) {
                        ScoreDocPtr score_doc = top_docs->scoreDocs[i];
                        DocumentPtr doc = searcher->doc(score_doc->doc);
                        std::string id = StringUtils::toUTF8(doc->get(StringUtils::toUnicode("id")));
                        
                        // Reciprocal rank scoring for text matches
                        text_scores[id] = 1.0f / (1.0f + (float)i);

                        // Store metadata if not already there (vector search might have missed it or vice versa)
                        if (metadata.find(id) == metadata.end()) {
                            metadata[id] = {
                                id,
                                StringUtils::toUTF8(doc->get(StringUtils::toUnicode("path"))),
                                std::stoi(StringUtils::toUTF8(doc->get(StringUtils::toUnicode("start_line")))),
                                std::stoi(StringUtils::toUTF8(doc->get(StringUtils::toUnicode("end_line")))),
                                StringUtils::toUTF8(doc->get(StringUtils::toUnicode("text"))),
                                0.0f,
                                StringUtils::toUTF8(doc->get(StringUtils::toUnicode("source")))
                            };
                        }
                    }
                    reader->close();
                }
            } catch (...) {
                spdlog::warn("Lucene search failed");
            }
        }

        // 3. Fusion & Decay
        std::vector<SearchResult> results;
        
        // We need to fetch metadata for items found only in vector search
        // In a real implementation, we'd store metadata in a DB or keep it in memory
        // For now, if metadata is missing, we try to fetch it from Lucene by ID
        for (auto const& [id, vec_score] : vector_scores) {
            if (metadata.find(id) == metadata.end()) {
                fetch_metadata_from_lucene(id, metadata);
            }
        }

        float vec_weight = 0.7f;
        float text_weight = 0.3f;
        double lambda = std::log(2.0) / 30.0; // 30-day half-life

        for (auto const& [id, res_meta] : metadata) {
            float v = vector_scores.count(id) ? vector_scores.at(id) : 0.0f;
            float t = text_scores.count(id) ? text_scores.at(id) : 0.0f;
            
            float final_score = (vec_weight * v) + (text_weight * t);
            
            // Apply Temporal Decay for daily logs
            final_score *= apply_temporal_decay(res_meta.path, lambda);

            SearchResult final_res = res_meta;
            final_res.score = final_score;
            results.push_back(final_res);
        }

        std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
            return a.score > b.score;
        });
        
        if ((int)results.size() > top_k) {
            results.resize(top_k);
        }
        return results;
    }

private:
    void fetch_metadata_from_lucene(const std::string& id, std::map<std::string, SearchResult>& metadata) {
        try {
            IndexReaderPtr reader = IndexReader::open(directory);
            if (reader) {
                IndexSearcherPtr searcher = newLucene<IndexSearcher>(reader);
                TermPtr term = newLucene<Term>(StringUtils::toUnicode("id"), StringUtils::toUnicode(id));
                QueryPtr query = newLucene<TermQuery>(term);
                TopDocsPtr top_docs = searcher->search(query, 1);
                
                if (top_docs->totalHits > 0) {
                    DocumentPtr doc = searcher->doc(top_docs->scoreDocs[0]->doc);
                    metadata[id] = {
                        id,
                        StringUtils::toUTF8(doc->get(StringUtils::toUnicode("path"))),
                        std::stoi(StringUtils::toUTF8(doc->get(StringUtils::toUnicode("start_line")))),
                        std::stoi(StringUtils::toUTF8(doc->get(StringUtils::toUnicode("end_line")))),
                        StringUtils::toUTF8(doc->get(StringUtils::toUnicode("text"))),
                        0.0f,
                        StringUtils::toUTF8(doc->get(StringUtils::toUnicode("source")))
                    };
                }
                reader->close();
            }
        } catch (...) {
            // Silently fail if metadata can't be fetched
        }
    }

    float apply_temporal_decay(const std::string& path, double lambda) {
        // Look for YYYY-MM-DD in path
        std::regex date_regex("(\\d{4})-(\\d{2})-(\\d{2})");
        std::smatch match;
        if (std::regex_search(path, match, date_regex)) {
            try {
                std::tm tm = {};
                tm.tm_year = std::stoi(match[1]) - 1900;
                tm.tm_mon = std::stoi(match[2]) - 1;
                tm.tm_mday = std::stoi(match[3]);
                
                auto file_time = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                auto now = std::chrono::system_clock::now();
                
                auto age = std::chrono::duration_cast<std::chrono::hours>(now - file_time).count() / 24.0;
                if (age < 0) age = 0;
                
                return (float)std::exp(-lambda * age);
            } catch (...) {
                return 1.0f;
            }
        }
        return 1.0f; // No decay for non-dated files (MEMORY.md)
    }
};

MemoryIndex::MemoryIndex(const std::string& index_path, int dimension)
    : impl_(std::make_unique<Impl>(index_path, dimension)) {}

MemoryIndex::~MemoryIndex() = default;

void MemoryIndex::add_document(
    const std::string& id,
    const std::string& path,
    int start_line,
    int end_line,
    const std::string& text,
    const std::vector<float>& embedding,
    const std::string& source
) {
    impl_->add_doc(id, path, start_line, end_line, text, embedding, source);
}

std::vector<SearchResult> MemoryIndex::search(
    const std::string& query,
    const std::vector<float>& query_embedding,
    int top_k
) {
    return impl_->search(query, query_embedding, top_k);
}

void MemoryIndex::clear() {
    // Basic clear: recreate Faiss and Lucene dir
    impl_->faiss_index = std::make_unique<faiss::IndexFlatIP>(impl_->dimension);
    impl_->doc_ids.clear();
    
    fs::remove(fs::path(impl_->index_path) / "faiss.index");
    fs::remove(fs::path(impl_->index_path) / "doc_ids.txt");
    
    // Lucene clear would need writer->deleteAll()
}
