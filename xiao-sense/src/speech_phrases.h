#pragma once
#include <cctype>
namespace speech_phrases {
struct Phrase { int command; const char* text; };
inline constexpr Phrase phrases[] = {
  {1, "SAY HELLO"},
  {7, "STOP MOVING"}, {7, "STOP"},
};
inline const Phrase* find(int command, const char* text) {
  if (!text) return nullptr;
  for (const auto& phrase : phrases) {
    if (phrase.command != command) continue;
    const char* a = text;
    const char* b = phrase.text;
    while (*a && *b && std::tolower(static_cast<unsigned char>(*a)) == std::tolower(static_cast<unsigned char>(*b))) { ++a; ++b; }
    if (!*a && !*b) return &phrase;
  }
  return nullptr;
}
} // namespace speech_phrases
