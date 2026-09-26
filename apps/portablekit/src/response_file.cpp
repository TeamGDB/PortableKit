#include "response_file.hpp"

#include <fstream>

namespace portablekit::app {

std::string response_file_text(const std::vector<std::string> &arguments) {
    std::string text;
    for (const std::string &argument : arguments) {
        text += '"';
        for (const char c : argument) {
            if (c == '\\') text += '/';
            else if (c == '"') text += "\\\"";
            else text += c;
        }
        text += "\"\n";
    }
    return text;
}

std::vector<std::string> with_response_file(const std::vector<std::string> &command, std::size_t keep,
                                            const std::filesystem::path &path) {
    if (command.size() <= keep) return command;
    const std::vector<std::string> rest(command.begin() + static_cast<std::ptrdiff_t>(keep), command.end());
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) return command;
        file << response_file_text(rest);
        if (!file) return command;
    }
    std::vector<std::string> result(command.begin(), command.begin() + static_cast<std::ptrdiff_t>(keep));
    result.push_back("@" + path.string());
    return result;
}

std::size_t command_line_length(const std::vector<std::string> &command) {
    std::size_t length = 0u;
    for (const std::string &argument : command) length += argument.size() + 3u;  // quotes and a space
    return length;
}

} // namespace portablekit::app
