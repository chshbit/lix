#include "escape-string.hh"
#include "ansicolor.hh"
#include <gtest/gtest.h>

namespace nix {

TEST(EscapeString, simple) {
  auto escaped = escapeString("puppy");
  ASSERT_EQ(escaped, "\"puppy\"");
}

TEST(EscapeString, escaping) {
  auto escaped = escapeString("\n\r\t \" \\ ${ooga booga}");
  ASSERT_EQ(escaped, "\"\\n\\r\\t \\\" \\\\ \\${ooga booga}\"");
}

TEST(EscapeString, maxLength) {
  auto escaped = escapeString("puppy", {.maxLength = 5});
  ASSERT_EQ(escaped, "\"puppy\"");

  escaped = escapeString("puppy doggy", {.maxLength = 5});
  ASSERT_EQ(escaped, "\"puppy\" «6 bytes elided»");
}

TEST(EscapeString, ansiColors) {
  auto escaped = escapeString("puppy doggy", {.maxLength = 5, .ansiColors = true});
  ASSERT_EQ(escaped, ANSI_MAGENTA "\"puppy\" " ANSI_FAINT "«6 bytes elided»" ANSI_NORMAL);
}

TEST(EscapeString, escapeNonPrinting) {
  auto escaped = escapeString("puppy\u0005doggy", {.escapeNonPrinting = true});
  ASSERT_EQ(escaped, "\"puppy\\x05doggy\"");
}

} // namespace nix
