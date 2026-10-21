#include "DictationText.h"

namespace GameEngine
{
namespace
{
constexpr const char* kWordSeparators = " \n";
}

size_t CountDictationWords(const std::string& text)
{
    size_t words = 0;
    for (size_t at = text.find_first_not_of(kWordSeparators); at != std::string::npos;
         at = text.find_first_not_of(kWordSeparators, text.find_first_of(kWordSeparators, at)))
        ++words;
    return words;
}

std::string DictationTextAfterWords(const std::string& transcript, size_t skippedWords)
{
    size_t at = transcript.find_first_not_of(kWordSeparators);
    for (size_t word = 0; word < skippedWords && at != std::string::npos; ++word)
        at = transcript.find_first_not_of(kWordSeparators, transcript.find_first_of(kWordSeparators, at));
    return at == std::string::npos ? std::string{} : transcript.substr(at);
}

} // namespace GameEngine
