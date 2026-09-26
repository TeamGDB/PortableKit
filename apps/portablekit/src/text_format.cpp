#include "text_format.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace portablekit::app {

std::string trim(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string lower(std::string text) {
    for (char &c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

KeyValues read_key_values(const std::filesystem::path &path) {
    KeyValues values;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const std::string text = trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') continue;
        const auto equals = text.find('=');
        if (equals == std::string::npos) continue;
        values[trim(text.substr(0, equals))] = trim(text.substr(equals + 1));
    }
    return values;
}

bool write_key_values(const std::filesystem::path &path, const KeyValues &values, const std::string &header) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return false;
        if (!header.empty()) out << header;
        for (const auto &[key, value] : values) out << key << " = " << value << "\n";
        if (!out) return false;
    }
    std::filesystem::rename(temporary, path, ec);
    return !ec;
}

std::string to_hex(const std::uint8_t *bytes, std::size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> from_hex(const std::string &text) {
    std::string digits;
    for (char c : text) {
        if (c == ' ' || c == ':' || c == ',' || c == '\t') continue;
        digits.push_back(c);
    }
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits.erase(0, 2);
    if (digits.size() % 2 != 0) return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(digits.size() / 2);
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        const int high = nibble(digits[i]);
        const int low = nibble(digits[i + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

std::string hex32_text(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", value);
    return text;
}

std::string Json::quote(const std::string &text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            } else {
                out.push_back(c);
            }
        }
    }
    return out + "\"";
}

Json &Json::field(const std::string &key, const std::string &value) {
    fields_.emplace_back(key, quote(value));
    return *this;
}
Json &Json::field(const std::string &key, const char *value) {
    return value != nullptr ? field(key, std::string(value)) : null_field(key);
}
Json &Json::field(const std::string &key, std::int64_t value) {
    fields_.emplace_back(key, std::to_string(value));
    return *this;
}
Json &Json::field(const std::string &key, std::uint64_t value) {
    fields_.emplace_back(key, std::to_string(value));
    return *this;
}
Json &Json::field(const std::string &key, double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4g", value);
    fields_.emplace_back(key, text);
    return *this;
}
Json &Json::field(const std::string &key, bool value) {
    fields_.emplace_back(key, value ? "true" : "false");
    return *this;
}
Json &Json::null_field(const std::string &key) {
    fields_.emplace_back(key, "null");
    return *this;
}
Json &Json::raw(const std::string &key, const std::string &json) {
    fields_.emplace_back(key, json);
    return *this;
}
Json &Json::array(const std::string &key, const std::vector<Json> &items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) out += (i ? ", " : "") + items[i].str();
    fields_.emplace_back(key, out + "]");
    return *this;
}
std::string Json::str() const {
    std::string out = "{";
    for (std::size_t i = 0; i < fields_.size(); ++i)
        out += (i ? ", " : "") + quote(fields_[i].first) + ": " + fields_[i].second;
    return out + "}";
}

std::string human_bytes(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1ull << 30));
    else if (bytes >= (1ull << 20)) std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / (1ull << 20));
    else std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    return text;
}

} // namespace portablekit::app
