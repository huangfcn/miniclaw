#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

int main() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    
    std::stringstream ss_local;
    ss_local << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %H:%M:%S");
    
    std::time_t t0 = std::chrono::system_clock::to_time_t(now);
    std::tm tm0 = *std::localtime(&t0);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm0);
    
    std::cout << "current_date() would return: " << buf << std::endl;
    return 0;
}
