#include <nlohmann/json.hpp>
#include "agent.hpp"
#include <crow.h>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

int main() {
    spdlog::info("Starting miniclaw Backend (C++)");

    crow::SimpleApp app;

    CROW_ROUTE(app, "/api/health")([](){
        return "OK";
    });

    static Agent global_agent;

    CROW_ROUTE(app, "/api/chat").methods(crow::HTTPMethod::POST)
    ([](const crow::request& req, crow::response& res){
        auto x = crow::json::load(req.body);
        if (!x) {
            res.code = 400;
            res.end();
            return;
        }
        
        std::string message = x["message"].s();
        std::string session_id = x["session_id"].s();
        
        spdlog::info("Received chat request: {}", message);

        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        
        global_agent.run(message, session_id, [&](const AgentEvent& ev) {
            json j = {{"type", ev.type}, {"content", ev.content}};
            std::string data = "data: " + j.dump() + "\n\n";
            res.write(data);
        });

        res.end();
    });

    app.port(8080).multithreaded().run();
}
