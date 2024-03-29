#include <iomanip>
#include <ostream>
#include <sstream>

#include "ansicolor.hh"
#include "escape-char.hh"
#include "english.hh"
#include "escape-string.hh"
#include "print-elided.hh"

namespace nix {

std::ostream &
escapeString(std::ostream & output, std::string_view string, EscapeStringOptions options)
{
    size_t charsPrinted = 0;
    if (options.ansiColors) {
        output << ANSI_MAGENTA;
    }
    output << "\"";
    for (auto i = string.begin(); i != string.end(); ++i) {
        if (charsPrinted >= options.maxLength) {
            output << "\" ";
            printElided(
                output, string.length() - charsPrinted, "byte", "bytes", options.ansiColors
            );
            return output;
        }

        if (*i == '\"' || *i == '\\') {
            output << "\\" << *i;
        } else if (*i == '\n') {
            output << "\\n";
        } else if (*i == '\r') {
            output << "\\r";
        } else if (*i == '\t') {
            output << "\\t";
        } else if (*i == '$' && *(i + 1) == '{') {
            output << "\\" << *i;
        } else if (options.escapeNonPrinting && !isprint(*i)) {
            output << MaybeHexEscapedChar{*i};
        } else {
            output << *i;
        }
        charsPrinted++;
    }
    output << "\"";
    if (options.ansiColors) {
        output << ANSI_NORMAL;
    }
    return output;
}

std::string escapeString(std::string_view s, EscapeStringOptions options)
{
    std::ostringstream output;
    escapeString(output, s, options);
    return output.str();
}

}; // namespace nix
