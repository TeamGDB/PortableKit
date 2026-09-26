#pragma once

// Long command lines through a response file. Windows refuses to start a
// process whose command line is longer than 32767 characters, and a large
// game's link lists hundreds of objects by full path: PSP2i (NPJH50332),
// 385 units, failed there with error 206. Compilers and linkers of every
// toolchain the app drives (clang, clang-cl, lld) read "@file" as the
// arguments in that file.

#include <filesystem>
#include <string>
#include <vector>

namespace portablekit::app {

// The file's text: one argument a line, each in double quotes, with
// backslashes written as forward slashes (every argument with a backslash
// is a Windows path, which accepts either) so that the GNU and the Windows
// rules for reading response files give the same arguments.
[[nodiscard]] std::string response_file_text(const std::vector<std::string> &arguments);

// `command` with everything after the first `keep` arguments (the program,
// usually) moved into a response file at `path`, written here. Returns the
// command unchanged if the file cannot be written.
[[nodiscard]] std::vector<std::string> with_response_file(const std::vector<std::string> &command, std::size_t keep,
                                                          const std::filesystem::path &path);

// The length of the command line the arguments make, spaces and quotes
// included, as a program starting them would build it.
[[nodiscard]] std::size_t command_line_length(const std::vector<std::string> &command);

} // namespace portablekit::app
