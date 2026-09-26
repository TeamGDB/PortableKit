// The desktop app's response files: a link of hundreds of objects by full
// path goes through "@file", and the file reads back as the same arguments.

#include "response_file.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char *what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++g_failures;
}

// Reads a response file the way GNU tools do: quoted arguments, a backslash
// escaping the next character.
std::vector<std::string> read_gnu(const std::string &text) {
    std::vector<std::string> arguments;
    std::string current;
    bool quoted = false, any = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            current += text[++i];
            any = true;
        } else if (c == '"') {
            quoted = !quoted;
            any = true;
        } else if (!quoted && (c == ' ' || c == '\n' || c == '\t' || c == '\r')) {
            if (any) arguments.push_back(current);
            current.clear();
            any = false;
        } else {
            current += c;
            any = true;
        }
    }
    if (any) arguments.push_back(current);
    return arguments;
}

} // namespace

int main() {
    using namespace portablekit::app;
    namespace fs = std::filesystem;

    std::vector<std::string> command = {"C:\\PortableKit\\toolchain\\bin\\clang++.exe", "-shared", "-o",
                                        "C:\\Users\\Someone\\Desktop\\PortableKit\\data\\cache\\GAME\\lib.dll.part"};
    for (int i = 0; i < 400; ++i)
        command.push_back("C:\\Users\\Someone\\Desktop\\PortableKit\\data\\cache\\NPJH50332-3e376f74\\69f683e076b23b09-O0"
                          "\\objects\\generated_unit_" + std::to_string(i) + ".o");
    command.push_back("-DNAME=\"quoted value\"");
    check(command_line_length(command) > 32767u, "four hundred objects by full path are past Windows' limit");

    const fs::path file = fs::temp_directory_path() / "portablekit_response_file_test.rsp";
    const auto short_command = with_response_file(command, 1u, file);
    check(short_command.size() == 2u && short_command[0] == command[0] && short_command[1] == "@" + file.string(),
          "the program stays, everything else becomes @file");
    check(command_line_length(short_command) < 1000u, "and the command line is short");

    std::stringstream text;
    {
        // Closed before the file is removed: Windows does not remove an open file.
        std::ifstream in(file, std::ios::binary);
        text << in.rdbuf();
    }
    const auto read = read_gnu(text.str());
    check(read.size() == command.size() - 1u, "the file holds every other argument");
    bool same = read.size() == command.size() - 1u;
    for (std::size_t i = 0; same && i < read.size(); ++i) {
        std::string expected = command[i + 1u];
        for (char &c : expected)
            if (c == '\\') c = '/';
        same = read[i] == expected;
    }
    check(same, "and reads back as the same arguments, with forward slashes");
    check(text.str().find('\\') == std::string::npos || text.str().find("\\\"") != std::string::npos,
          "no backslash is left but the quote escapes");
    std::error_code ignored;
    fs::remove(file, ignored);

    const auto unchanged = with_response_file({"clang++"}, 1u, file);
    check(unchanged.size() == 1u && !fs::exists(file), "a command with nothing to move is left alone");

    if (g_failures != 0) std::printf("%d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
