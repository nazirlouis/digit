#pragma once
#include <cctype>
namespace speech_phrases {
struct Phrase { int command; const char* text; };
inline constexpr Phrase phrases[] = {
  {8, "COME SAY HELLO"},
  {9, "WHAT IS THIS"}, {9, "WHAT'S THIS"},
  {14, "FIND A WAY OUT"},
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
