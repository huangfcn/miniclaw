#include <iostream>
#include <cassert>
#include "tools/busybox.hpp"
#include <spdlog/spdlog.h>

int main() {
    spdlog::set_level(spdlog::level::debug);
    spdlog::info("Starting BusyBoxTool test...");

    BusyBoxTool tool;
    
    std::cout << "Testing 'ls' command..." << std::endl;
    std::string result = tool.execute("ls");
    std::cout << "Result:\n" << result << std::endl;
    assert(!result.empty() && result.find("Error") == std::string::npos);

    std::cout << "Testing 'grep' command..." << std::endl;
    // Assuming we have a file or can grep something
    result = tool.execute("echo 'hello world' | grep 'hello'");
    std::cout << "Result:\n" << result << std::endl;
    assert(result.find("hello") != std::string::npos);

    std::cout << "Testing non-existent command..." << std::endl;
    result = tool.execute("nonexistentcommand");
    std::cout << "Result:\n" << result << std::endl;
    
    std::cout << "\n✅ BusyBoxTool Test PASSED!" << std::endl;
    return 0;
}
