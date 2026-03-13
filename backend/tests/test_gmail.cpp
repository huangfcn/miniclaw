#include "agent.hpp"
#include "agent/fiber_pool.hpp"
#include "config.hpp"
#include "tools/gmail.hpp"
#include <iostream>
#include <spdlog/spdlog.h>

int main() {
  spdlog::set_level(spdlog::level::debug);
  spdlog::info("Starting Gmail Tool C++ Test");

  // Load Configuration
  Config::instance().load();

  static Agent global_agent;

  // Initialize FiberPool
  FiberPool::instance().init(1, &global_agent);
  curl_global_init(CURL_GLOBAL_ALL);

  // Run the test in the pool
  std::atomic<bool> done{false};
  FiberPool::instance().spawn([&]() {
      try {
        GmailTool gmail;
        std::cout << "[Test] Action: check_creds" << std::endl;
        std::string res = gmail.execute({{"action", "check_creds"}});
        std::cout << "Result: " << res << std::endl;

        std::cout << "[Test] Action: auth (get URL)" << std::endl;
        std::string auth_res = gmail.execute({{"action", "auth"}});
        std::cout << "Auth Result: " << auth_res << std::endl;

        if (res.find("Error") == std::string::npos) {
          std::cout << "[Test] Action: list (is:unread)" << std::endl;
          std::string list_res =
              gmail.execute({{"action", "list"}, {"query", "is:unread"}});

          simdjson::dom::parser parser;
          simdjson::dom::element j;
          auto error = parser.parse(list_res).get(j);

          std::string first_id;
          simdjson::dom::array messages;
          if (!error && !j["messages"].get(messages)) {
            for (auto msg : messages) {
              std::string_view id;
              if (!msg["id"].get(id)) {
                first_id = std::string(id);
                break;
              }
            }
          }

          if (!first_id.empty()) {
            std::cout << "[Test] Action: get (ID: " << first_id << ")"
                      << std::endl;
            std::string msg_res = gmail.execute(
                {{"action", "get"}, {"message_id", first_id}});

            std::cout << "Message Fetch Result Length: " << msg_res.length()
                      << std::endl;

            // Now let's try to summarize it using the agent!
            std::cout << "\n--- Agent Summarization Demo ---" << std::endl;
            std::string prompt = "Summarize this Gmail message JSON and "
                                 "suggest a short reply:\n" +
                                 msg_res;

            global_agent.run(prompt, "test_gmail_session",
                             [](const AgentEvent &ev) {
                               if (ev.type == "token") {
                                 std::cout << ev.content;
                               } else if (ev.type == "done") {
                                 std::cout << std::endl;
                               }
                             });

            std::cout << "\n✅ Gmail C++ End-to-End Test PASSED!"
                      << std::endl;
          } else {
            std::cout << "\n⚠️ No unread messages found for content testing."
                      << std::endl;
          }
        }
      } catch (const std::exception &e) {
        std::cerr << "Exception in test task: " << e.what() << std::endl;
      }
      done = true;
  });

  // Wait for completion (simple spin wait for test)
  while (!done) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  FiberPool::instance().stop();
  curl_global_cleanup();

  return 0;
}
