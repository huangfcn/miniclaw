#pragma once
#include <string>
#include <map>

class Tool {
public:
    virtual ~Tool() = default;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;

    // Returns an OpenAI-compatible function schema JSON string.
    // e.g. {"type":"function","function":{"name":"exec","description":"...","parameters":{...}}}
    virtual std::string schema() const = 0;

    // Execute with named arguments (primary interface for native function calling).
    // The base implementation falls back to the legacy string-based execute.
    virtual std::string execute(const std::map<std::string, std::string>& args) {
        // Default: pass the first arg value to legacy execute, or empty string
        if (!args.empty()) return execute(args.begin()->second);
        return execute(std::string{});
    }

    // Legacy single-string execute (kept for backward compat, used by base execute above).
    virtual std::string execute(const std::string& input) { return "Error: not implemented"; }
};
