#include "cron_service.hpp"
#include <fstream>
#include <spdlog/spdlog.h>
#include <fiber.hpp>
#include <simdjson.h>
#include "json_util.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <algorithm>
#include "fiber_pool.hpp"
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <croncpp.h>

struct CronService::Impl {
    struct Request {
        enum Type { ADD, REMOVE, LIST } type;
        std::string schedule;
        std::string task;
        std::string id;
        std::string* out_id = nullptr;
        bool* out_success = nullptr;
        std::vector<CronJob>* out_list = nullptr;
        std::function<void()> on_complete;
    };

    std::string storage_path;
    std::vector<CronJob> jobs;
    std::thread worker_thread;
    std::queue<Request> queue;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> running{false};

    Impl() = default;
    ~Impl() {
        stop();
    }

    void start() {
        if (running) return;
        running = true;
        worker_thread = std::thread(&Impl::worker_loop, this);
    }

    void stop() {
        if (!running) return;
        running = false;
        cv.notify_all();
        if (worker_thread.joinable()) worker_thread.join();
    }

    void worker_loop() {
        load_internal();
        while (running) {
            std::unique_lock<std::mutex> lock(mtx);
            
            std::time_t now = std::time(nullptr);
            std::time_t next_trigger = 0;
            for (const auto& job : jobs) {
                if (next_trigger == 0 || job.next_run < next_trigger) {
                    next_trigger = job.next_run;
                }
            }

            if (next_trigger == 0 || next_trigger > now + 60) {
                next_trigger = now + 60;
            }

            auto wait_until = std::chrono::system_clock::from_time_t(next_trigger);
            cv.wait_until(lock, wait_until, [this] { return !queue.empty() || !running; });

            if (!running && queue.empty()) break;

            while (!queue.empty()) {
                auto req = std::move(queue.front());
                queue.pop();
                process_request(req);
                if (req.on_complete) req.on_complete();
            }

            now = std::time(nullptr);
            std::vector<CronJob> triggered;
            bool changed = false;
            for (auto& job : jobs) {
                if (now >= job.next_run) {
                    triggered.push_back(job);
                    if (job.recurring) {
                        job.next_run = calculate_next_run(job, now);
                        changed = true;
                    }
                }
            }
            if (changed) save_internal();

            lock.unlock();

            for (const auto& job : triggered) {
                spdlog::info("Triggering cron job: {} - {}", job.id, job.task_description);
            }
        }
    }

    void process_request(Request& req) {
        switch (req.type) {
            case Request::ADD: {
                CronJob job;
                job.id = "job_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
                job.task_description = req.task;
                if (req.schedule.find("every ") == 0) {
                    std::string s = req.schedule.substr(6);
                    if (!s.empty() && s.back() == 's') {
                        try {
                            job.interval_seconds = std::stoi(s.substr(0, s.size() - 1));
                            job.recurring = true;
                            job.next_run = std::time(nullptr) + job.interval_seconds;
                        } catch (...) {
                            if (req.out_id) *req.out_id = "";
                            return;
                        }
                    }
                } else {
                    try {
                        auto expr = cron::make_cron(req.schedule);
                        job.cron_expr = req.schedule;
                        job.recurring = true;
                        job.next_run = cron::cron_next(expr, std::time(nullptr));
                    } catch (...) {
                        if (req.out_id) *req.out_id = "";
                        return;
                    }
                }
                jobs.push_back(job);
                save_internal();
                if (req.out_id) *req.out_id = job.id;
                break;
            }
            case Request::REMOVE: {
                auto it = std::remove_if(jobs.begin(), jobs.end(), [&](const CronJob& j) { return j.id == req.id; });
                if (it != jobs.end()) {
                    jobs.erase(it, jobs.end());
                    save_internal();
                    if (req.out_success) *req.out_success = true;
                } else if (req.out_success) *req.out_success = false;
                break;
            }
            case Request::LIST: {
                if (req.out_list) *req.out_list = jobs;
                break;
            }
        }
    }

    std::time_t calculate_next_run(const CronJob& job, std::time_t now) {
        if (job.interval_seconds > 0) return now + job.interval_seconds;
        if (!job.cron_expr.empty()) {
            try {
                auto expr = cron::make_cron(job.cron_expr);
                return cron::cron_next(expr, now);
            } catch (...) {}
        }
        return now + 60;
    }

    void save_internal() {
        std::ofstream f(storage_path);
        if (!f.is_open()) return;
        f << "[\n";
        for (size_t i = 0; i < jobs.size(); ++i) {
            const auto& j = jobs[i];
            f << "  {\n";
            f << "    \"id\": \"" << j.id << "\",\n";
            f << "    \"cron_expr\": \"" << j.cron_expr << "\",\n";
            f << "    \"task\": \"" << json_util::escape(j.task_description) << "\",\n";
            f << "    \"next_run\": " << j.next_run << ",\n";
            f << "    \"recurring\": " << (j.recurring ? "true" : "false") << ",\n";
            f << "    \"interval\": " << j.interval_seconds << "\n";
            f << "  }" << (i == jobs.size() - 1 ? "" : ",") << "\n";
        }
        f << "]";
    }

    void load_internal() {
        std::ifstream f(storage_path);
        if (!f.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (content.empty()) return;
        try {
            simdjson::dom::parser parser;
            simdjson::dom::array arr;
            auto padded = simdjson::padded_string(content);
            if (parser.parse(padded).get(arr)) return;
            jobs.clear();
            for (auto entry : arr) {
                CronJob j;
                std::string_view sv;
                if (!entry["id"].get(sv)) j.id = std::string(sv);
                if (!entry["cron_expr"].get(sv)) j.cron_expr = std::string(sv);
                if (!entry["task"].get(sv)) j.task_description = std::string(sv);
                int64_t next;
                if (!entry["next_run"].get(next)) j.next_run = next;
                bool rec;
                if (!entry["recurring"].get(rec)) j.recurring = rec;
                int64_t interval;
                if (!entry["interval"].get(interval)) j.interval_seconds = static_cast<int>(interval);
                jobs.push_back(j);
            }
        } catch (...) {}
    }
};

CronService::CronService() : impl_(std::make_unique<Impl>()) {}
CronService::~CronService() = default;

void CronService::init(const std::string& workspace_path) {
    impl_->storage_path = workspace_path + "/cron_jobs.json";
}

void CronService::start() {
    impl_->start();
}

void CronService::stop() {
    impl_->stop();
}

std::string CronService::add_job(const std::string& schedule, const std::string& task) {
    std::string result_id;
    Impl::Request req;
    req.type = Impl::Request::ADD;
    req.schedule = schedule;
    req.task = task;
    req.out_id = &result_id;

    auto calling_fiber = fiber_ident();
    auto calling_node = FiberNode::current();

    if (calling_node) {
        req.on_complete = [calling_node, calling_fiber]() {
            calling_node->spawn([calling_fiber]() {
                fiber_resume(calling_fiber);
            });
        };
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->queue.push(std::move(req));
        }
        impl_->cv.notify_one();
        fiber_suspend(0);
    } else {
        std::condition_variable cv_req;
        std::mutex mtx_req;
        bool done = false;
        req.on_complete = [&cv_req, &mtx_req, &done]() {
            {
                std::lock_guard<std::mutex> lock(mtx_req);
                done = true;
            }
            cv_req.notify_one();
        };
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->queue.push(std::move(req));
        }
        impl_->cv.notify_one();
        std::unique_lock<std::mutex> lock(mtx_req);
        cv_req.wait(lock, [&done] { return done; });
    }
    return result_id;
}

bool CronService::remove_job(const std::string& id) {
    bool success = false;
    Impl::Request req;
    req.type = Impl::Request::REMOVE;
    req.id = id;
    req.out_success = &success;

    auto calling_fiber = fiber_ident();
    auto calling_node = FiberNode::current();

    if (calling_node) {
        req.on_complete = [calling_node, calling_fiber]() {
            calling_node->spawn([calling_fiber]() {
                fiber_resume(calling_fiber);
            });
        };
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->queue.push(std::move(req));
        }
        impl_->cv.notify_one();
        fiber_suspend(0);
    } else {
        std::condition_variable cv_req;
        std::mutex mtx_req;
        bool done = false;
        req.on_complete = [&cv_req, &mtx_req, &done]() {
            {
                std::lock_guard<std::mutex> lock(mtx_req);
                done = true;
            }
            cv_req.notify_one();
        };
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->queue.push(std::move(req));
        }
        impl_->cv.notify_one();
        std::unique_lock<std::mutex> lock(mtx_req);
        cv_req.wait(lock, [&done] { return done; });
    }
    return success;
}

std::vector<CronJob> CronService::list_jobs() {
    std::vector<CronJob> results;
    Impl::Request req;
    req.type = Impl::Request::LIST;
    req.out_list = &results;

    auto calling_fiber = fiber_ident();
    auto calling_node = FiberNode::current();

    if (calling_node) {
        req.on_complete = [calling_node, calling_fiber]() {
            calling_node->spawn([calling_fiber]() {
                fiber_resume(calling_fiber);
            });
        };
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->queue.push(std::move(req));
        }
        impl_->cv.notify_one();
        fiber_suspend(0);
    } else {
        std::condition_variable cv_req;
        std::mutex mtx_req;
        bool done = false;
        req.on_complete = [&cv_req, &mtx_req, &done]() {
            {
                std::lock_guard<std::mutex> lock(mtx_req);
                done = true;
            }
            cv_req.notify_one();
        };
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->queue.push(std::move(req));
        }
        impl_->cv.notify_one();
        std::unique_lock<std::mutex> lock(mtx_req);
        cv_req.wait(lock, [&done] { return done; });
    }
    return results;
}
