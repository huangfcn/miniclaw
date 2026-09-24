#pragma once
#include <string>
#include <sstream>
#include <iomanip>
#include <cstdint>

namespace json_util {

// Replace any ill-formed UTF-8 byte sequences with U+FFFD (EF BF BD) so the
// result is always valid UTF-8. Tool output captured from Windows consoles is
// frequently cp1252/cp936/etc.; passing those raw bytes into a JSON payload
// makes strict parsers (e.g. llama.cpp's nlohmann-json) reject the whole
// request with a 500. Sanitizing here — the single chokepoint every message
// passes through before hitting the LLM or the session file — keeps both safe.
inline std::string sanitize_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        unsigned char b = p[i];
        size_t len = 0;
        uint32_t cp = 0;
        if (b < 0x80) {
            len = 1; cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            len = 2; cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            len = 3; cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            len = 4; cp = b & 0x07;
        } else {
            // Stray continuation byte or invalid lead (e.g. cp1252 0xb0).
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        if (i + len > n) {
            // Truncated sequence at end of input.
            out += "\xEF\xBF\xBD";
            break;
        }
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        // Reject overlong encodings, surrogates, and out-of-range codepoints.
        static const uint32_t min_cp[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (ok && (cp < min_cp[len] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF))
            ok = false;
        if (!ok) {
            out += "\xEF\xBF\xBD";
            ++i;  // resync after the bad lead byte
        } else {
            out.append(reinterpret_cast<const char*>(p + i), len);
            i += len;
        }
    }
    return out;
}

inline std::string escape(const std::string& s) {
    std::string safe = sanitize_utf8(s);
    std::ostringstream o;
    for (auto c : safe) {
        switch (c) {
        case '"': o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\b': o << "\\b"; break;
        case '\f': o << "\\f"; break;
        case '\n': o << "\\n"; break;
        case '\r': o << "\\r"; break;
        case '\t': o << "\\t"; break;
        default:
            if ('\x00' <= c && c <= '\x1f') {
                o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)(unsigned char)c;
            } else {
                o << c;
            }
        }
    }
    return o.str();
}

} // namespace json_util
