#ifdef _WIN32
#include <winsock2.h>
#endif
#include "memory_index.hpp"
#include <faiss/IndexFlat.h>
#include <faiss/index_io.h>
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
#include <spdlog/spdlog.h>
#include <cmath>
#include <fstream>
#include <condition_variable>
#include <queue>
#include <thread>
#include <regex>

namespace fs = std::filesystem;
using namespace Lucene;

struct MemoryIndex::Impl {
    std::string index_path;
    int dimension;
    std::unique_ptr<faiss::IndexFlatIP> faiss_index;
    std::vector<std::string> doc_ids;
    
    // Lucene++ components
    Lucene::String lucene_path;
    Lucene::AnalyzerPtr analyzer;
    Lucene::DirectoryPtr directory;

    // Service Thread components
    enum class CommandType { ADD_DOC, SEARCH, CLEAR, STOP };
    struct Command {
        CommandType type;
        // Data for ADD_DOC
        std::string id, path, text, source;
        int start_line, end_line;
        std::vector<float> embedding;
        // Data for SEARCH
        std::string query;
        std::vector<float> query_embedding;
        int top_k;
        std::vector<SearchResult>* out_results;
        std::mutex* out_mtx;
        std::condition_variable* out_cv;
        bool* out_done;
    };

    std::queue<Command> queue;
    std::mutex mtx;
    std::condition_variable cv;
    std::thread service_thread;
    bool running = true;

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

        service_thread = std::thread(&Impl::service_loop, this);
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            running = false;
            Command cmd;
            cmd.type = CommandType::STOP;
            queue.push(cmd);
        }
        cv.notify_one();
        if (service_thread.joinable()) service_thread.join();
    }

    void service_loop() {
        while (true) {
            Command cmd;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this] { return !queue.empty(); });
                cmd = std::move(queue.front());
                queue.pop();
            }

            if (cmd.type == CommandType::STOP) break;
            process_command(cmd);
        }
    }

    void process_command(Command& cmd) {
        if (cmd.type == CommandType::ADD_DOC) {
            do_add_doc(cmd);
        } else if (cmd.type == CommandType::SEARCH) {
            do_search(cmd);
        } else if (cmd.type == CommandType::CLEAR) {
            do_clear_all(cmd);
        }
    }

    void add_doc(const std::string& id, const std::string& path, int start_line, int end_line,
                 const std::string& text, const std::vector<float>& embedding, const std::string& source) {
        std::lock_guard<std::mutex> lock(mtx);
        Command cmd;
        cmd.type = CommandType::ADD_DOC;
        cmd.id = id;
        cmd.path = path;
        cmd.text = text;
        cmd.source = source;
        cmd.start_line = start_line;
        cmd.end_line = end_line;
        cmd.embedding = embedding;
        queue.push(std::move(cmd));
        cv.notify_one();
    }

    void do_add_doc(Command& cmd) {
        // 1. Add to Faiss
        if ((int)cmd.embedding.size() == dimension) {
            faiss_index->add(1, cmd.embedding.data());
            doc_ids.push_back(cmd.id);
            
            try {
                fs::path faiss_path = fs::path(index_path) / "faiss.index";
                faiss::write_index(faiss_index.get(), faiss_path.string().c_str());
                fs::path ids_path = fs::path(index_path) / "doc_ids.txt";
                std::ofstream f(ids_path, std::ios::app);
                if (f.is_open()) f << cmd.id << "\n";
            } catch (...) {}
        }

        // 2. Add to Lucene++
        try {
            bool create = !IndexReader::indexExists(directory);
            IndexWriterPtr writer = newLucene<IndexWriter>(directory, analyzer, create, IndexWriter::MaxFieldLengthLIMITED);
            DocumentPtr doc = newLucene<Document>();
            doc->add(newLucene<Field>(StringUtils::toUnicode("id"), StringUtils::toUnicode(cmd.id), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("path"), StringUtils::toUnicode(cmd.path), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("text"), StringUtils::toUnicode(cmd.text), Field::STORE_YES, Field::INDEX_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("start_line"), StringUtils::toUnicode(std::to_string(cmd.start_line)), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("end_line"), StringUtils::toUnicode(std::to_string(cmd.end_line)), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));
            doc->add(newLucene<Field>(StringUtils::toUnicode("source"), StringUtils::toUnicode(cmd.source), Field::STORE_YES, Field::INDEX_NOT_ANALYZED));
            writer->addDocument(doc);
            writer->close();
        } catch (...) {}
    }

    std::vector<SearchResult> search(const std::string& query, const std::vector<float>& query_embedding, int top_k) {
        std::vector<SearchResult> results;
        std::mutex out_mtx;
        std::condition_variable out_cv;
        bool out_done = false;

        {
            std::lock_guard<std::mutex> lock(mtx);
            Command cmd;
            cmd.type = CommandType::SEARCH;
            cmd.query = query;
            cmd.query_embedding = query_embedding;
            cmd.top_k = top_k;
            cmd.out_results = &results;
            cmd.out_mtx = &out_mtx;
            cmd.out_cv = &out_cv;
            cmd.out_done = &out_done;
            queue.push(std::move(cmd));
        }
        cv.notify_one();

        std::unique_lock<std::mutex> lock(out_mtx);
        out_cv.wait(lock, [&] { return out_done; });
        return results;
    }

    void do_search(Command& cmd) {
        std::map<std::string, float> vector_scores;
        std::map<std::string, float> text_scores;
        std::map<std::string, SearchResult> metadata;

        if ((int)cmd.query_embedding.size() == dimension && faiss_index->ntotal > 0) {
            int candidate_k = std::min((int)faiss_index->ntotal, cmd.top_k * 4);
            std::vector<float> distances(candidate_k);
            std::vector<faiss::idx_t> labels(candidate_k);
            faiss_index->search(1, cmd.query_embedding.data(), candidate_k, distances.data(), labels.data());
            for (int i = 0; i < candidate_k; ++i) {
                if (labels[i] >= 0 && labels[i] < (faiss::idx_t)doc_ids.size()) {
                    vector_scores[doc_ids[labels[i]]] = distances[i];
                }
            }
        }

        if (!cmd.query.empty()) {
            try {
                IndexReaderPtr reader = IndexReader::open(directory);
                if (reader) {
                    IndexSearcherPtr searcher = newLucene<IndexSearcher>(reader);
                    QueryParserPtr parser = newLucene<QueryParser>(LuceneVersion::LUCENE_CURRENT, StringUtils::toUnicode("text"), analyzer);
                    QueryPtr lucene_query = parser->parse(StringUtils::toUnicode(cmd.query));
                    TopDocsPtr top_docs = searcher->search(lucene_query, cmd.top_k * 4);
                    for (int i = 0; i < (int)top_docs->scoreDocs.size(); ++i) {
                        DocumentPtr doc = searcher->doc(top_docs->scoreDocs[i]->doc);
                        std::string id = StringUtils::toUTF8(doc->get(StringUtils::toUnicode("id")));
                        text_scores[id] = 1.0f / (1.0f + (float)i);
                        metadata[id] = { id, StringUtils::toUTF8(doc->get(StringUtils::toUnicode("path"))),
                            std::stoi(StringUtils::toUTF8(doc->get(StringUtils::toUnicode("start_line")))),
                            std::stoi(StringUtils::toUTF8(doc->get(StringUtils::toUnicode("end_line")))),
                            StringUtils::toUTF8(doc->get(StringUtils::toUnicode("text"))), 0.0f,
                            StringUtils::toUTF8(doc->get(StringUtils::toUnicode("source"))) };
                    }
                    reader->close();
                }
            } catch (...) {}
        }

        for (auto const& [id, vec_score] : vector_scores) {
            if (metadata.find(id) == metadata.end()) fetch_metadata_from_lucene(id, metadata);
        }

        std::vector<SearchResult> final_results;
        for (auto const& [id, res_meta] : metadata) {
            float v = vector_scores.count(id) ? vector_scores.at(id) : 0.0f;
            float t = text_scores.count(id) ? text_scores.at(id) : 0.0f;
            SearchResult res = res_meta;
            res.score = (0.7f * v) + (0.3f * t);
            final_results.push_back(res);
        }

        std::sort(final_results.begin(), final_results.end(), [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
        if ((int)final_results.size() > cmd.top_k) final_results.resize(cmd.top_k);
        
        {
            std::lock_guard<std::mutex> lock(*cmd.out_mtx);
            *cmd.out_results = std::move(final_results);
            *cmd.out_done = true;
        }
        cmd.out_cv->notify_one();
    }

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
                    metadata[id] = { id, StringUtils::toUTF8(doc->get(StringUtils::toUnicode("path"))),
                        std::stoi(StringUtils::toUTF8(doc->get(StringUtils::toUnicode("start_line")))),
                        std::stoi(StringUtils::toUTF8(doc->get(StringUtils::toUnicode("end_line")))),
                        StringUtils::toUTF8(doc->get(StringUtils::toUnicode("text"))), 0.0f,
                        StringUtils::toUTF8(doc->get(StringUtils::toUnicode("source"))) };
                }
                reader->close();
            }
        } catch (...) {}
    }

    void do_clear_all(Command& cmd) {
        faiss_index = std::make_unique<faiss::IndexFlatIP>(dimension);
        doc_ids.clear();
        fs::remove_all(index_path);
        fs::create_directories(index_path);
    }
};

MemoryIndex::MemoryIndex(const std::string& index_path, int dimension)
    : impl_(std::make_unique<Impl>(index_path, dimension)) {}

MemoryIndex::MemoryIndex(const fs::path& index_path, int dimension)
    : impl_(std::make_unique<Impl>(index_path.string(), dimension)) {}

MemoryIndex::~MemoryIndex() = default;

void MemoryIndex::add_document(const std::string& id, const std::string& path, int start_line, int end_line,
                             const std::string& text, const std::vector<float>& embedding, const std::string& source) {
    impl_->add_doc(id, path, start_line, end_line, text, embedding, source);
}

std::vector<SearchResult> MemoryIndex::search(const std::string& query, const std::vector<float>& query_embedding, int top_k) {
    return impl_->search(query, query_embedding, top_k);
}

void MemoryIndex::clear() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    Impl::Command cmd;
    cmd.type = Impl::CommandType::CLEAR;
    impl_->queue.push(cmd);
    impl_->cv.notify_one();
}
