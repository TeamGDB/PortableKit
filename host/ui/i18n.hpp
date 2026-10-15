#pragma once

#include <initializer_list>
#include <string>

// The interface's languages. Text is written in English everywhere and looked
// up here when it is drawn: the widgets translate what they are given, so a
// screen only has to build its own sentences from patterns (trf) where it
// puts names or numbers into them. Text with no translation stays English.
namespace portablekit::ui {

enum class Language { English, Portuguese };

// The player's choice (Settings::language), else the system's language.
[[nodiscard]] Language language();
// Cycles to the next language and saves the choice.
void next_language();
// The current language's own name: "English", "Português".
[[nodiscard]] const char *language_name();

// `english` in the current language.
[[nodiscard]] const char *tr(const char *english);
[[nodiscard]] std::string tr(const std::string &english);
// `pattern` in the current language, each {} replaced by the next argument.
[[nodiscard]] std::string trf(const char *pattern, std::initializer_list<std::string> args);

} // namespace portablekit::ui
