#pragma once
#include <string>
#include <vector>
#include <memory>
#include <ctime>

struct CronJob {
    std::string id;
    std::string cron_expr;
    std::string task_description;
    std::time_t next_run;
    bool recurring = false;
    int interval_seconds = 0;
};

class CronService {
public:
    static CronService& instance() {
        static CronService inst;
        return inst;
    }

    void init(const std::string& workspace_path);
    void start();
    void stop();

    // These now handle fiber suspension/resumption if called from a fiber
    std::string add_job(const std::string& schedule, const std::string& task);
    bool remove_job(const std::string& id);
    std::vector<CronJob> list_jobs();

private:
    CronService();
    ~CronService();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
