#pragma once
// FitsHeaderString.h
// Assemble a FITS header STRING from the 80-character card images a backend
// hands over (``/v1/files/header`` returns exactly astropy's ``str(card)``).
//
// libwcs parses a header as one flat buffer of fixed 80-column cards terminated
// by END - it does not take a list. The conversion is three rules (pad, truncate,
// terminate), all of which fail silently when wrong: a short card shifts every
// keyword after it, a missing END makes the parser read past the buffer. Pure
// string work, kept out of AstroUtils so it can be tested without libwcs.

#include <string>
#include <vector>

namespace visivo {

/// The fixed FITS card width.
inline constexpr std::size_t kFitsCardLength = 80;

/// One buffer of 80-column cards ending with a padded END card. Cards are
/// space-padded, and anything longer than 80 characters is truncated - a card
/// image is 80 columns by definition, and letting a long one through would
/// misalign everything after it.
inline std::string buildFitsHeaderString(const std::vector<std::string> &cards)
{
    std::string header;
    header.reserve((cards.size() + 1) * kFitsCardLength);
    for (const std::string &card : cards) {
        std::string padded = card.substr(0, kFitsCardLength);
        padded.resize(kFitsCardLength, ' ');
        header += padded;
    }
    std::string end = "END";
    end.resize(kFitsCardLength, ' ');
    header += end;
    return header;
}

}   // namespace visivo
