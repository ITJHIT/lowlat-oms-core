// Minimal flat-YAML-subset config loader (header-only).
//
// Supports exactly what an operational "config-driven" process needs at start:
// `key: value` lines, `#` comments, blank lines. Nested YAML is intentionally
// out of scope -- the point is a dependency-free, testable config surface, not a
// YAML implementation.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace lloms {

class Config {
  public:
    static Config from_string(const std::string& text) {
        Config c;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            c.parse_line(line);
        }
        return c;
    }

    static Config from_file(const std::string& path) {
        std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        return from_string(ss.str());
    }

    bool has(const std::string& key) const { return kv_.count(key) != 0; }

    std::string get_str(const std::string& key, const std::string& fallback = "") const {
        auto it = kv_.find(key);
        return it == kv_.end() ? fallback : it->second;
    }

    std::int64_t get_int(const std::string& key, std::int64_t fallback = 0) const {
        auto it = kv_.find(key);
        if (it == kv_.end()) {
            return fallback;
        }
        try {
            return std::stoll(it->second);
        } catch (...) {
            return fallback;
        }
    }

    std::size_t size() const { return kv_.size(); }

    // Every key present, sorted. Needed by callers whose config is open-ended --
    // `route.<symbol>: <venue>` entries, for instance, cannot be fetched by name
    // because the names are not known until the file is read.
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        out.reserve(kv_.size());
        for (const auto& kv : kv_) {
            out.push_back(kv.first);
        }
        std::sort(out.begin(), out.end());  // hash order is not reproducible
        return out;
    }

  private:
    void parse_line(std::string line) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.erase(hash);
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            return;
        }
        std::string key = trim(line.substr(0, colon));
        std::string val = trim(line.substr(colon + 1));
        if (!key.empty()) {
            kv_[key] = val;
        }
    }

    static std::string trim(const std::string& s) {
        std::size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    }

    std::unordered_map<std::string, std::string> kv_;
};

}  // namespace lloms
