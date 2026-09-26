#pragma once

// Small text formats the app keeps its state in: key=value files (a game's
// record, a corpus's status, the keys file) and JSON for the command line's
// --json output. Nothing here needs a library.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace portablekit::app {

using KeyValues = std::map<std::string, std::string>;

// "key = value" lines; '#' starts a comment. Unreadable file: empty map.
[[nodiscard]] KeyValues read_key_values(const std::filesystem::path &path);
// Written to a temporary file and renamed, so a reader never sees half of it.
bool write_key_values(const std::filesystem::path &path, const KeyValues &values, const std::string &header = {});

[[nodiscard]] std::string to_hex(const std::uint8_t *bytes, std::size_t size);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> from_hex(const std::string &text);
[[nodiscard]] std::string hex32_text(std::uint32_t value);
[[nodiscard]] std::string trim(const std::string &text);
[[nodiscard]] std::string lower(std::string text);

// A JSON object built field by field, for --json output.
class Json {
public:
    Json &field(const std::string &key, const std::string &value);
    Json &field(const std::string &key, const char *value);
    Json &field(const std::string &key, std::int64_t value);
    Json &field(const std::string &key, std::uint64_t value);
    Json &field(const std::string &key, int value) { return field(key, static_cast<std::int64_t>(value)); }
    Json &field(const std::string &key, unsigned value) { return field(key, static_cast<std::uint64_t>(value)); }
    Json &field(const std::string &key, double value);
    Json &field(const std::string &key, bool value);
    Json &null_field(const std::string &key);
    Json &raw(const std::string &key, const std::string &json);
    Json &array(const std::string &key, const std::vector<Json> &items);
    [[nodiscard]] std::string str() const;

    static std::string quote(const std::string &text);

private:
    std::vector<std::pair<std::string, std::string>> fields_;
};

[[nodiscard]] std::string human_bytes(std::uint64_t bytes);

} // namespace portablekit::app
