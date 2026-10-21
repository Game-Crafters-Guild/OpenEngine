#pragma once

#include <cstddef>
#include <string>

namespace GameEngine
{

/// Words separated by spaces or line breaks in a dictation transcript.
size_t CountDictationWords(const std::string& text);

/// The transcript from its (skippedWords + 1)th word on. A speech session's transcript is
/// cumulative and the recognizer revises a word in place without changing how many came
/// before it, so words already cleared from the prompt are skipped by count.
std::string DictationTextAfterWords(const std::string& transcript, size_t skippedWords);

} // namespace GameEngine
