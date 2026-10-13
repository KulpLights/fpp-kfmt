#pragma once

// UTF-8 -> RDS character set (IEC 62106 / EN 50067 Annex E, code table E.1 "G0"), one byte per
// character. FPP hands plugins UTF-8 (TagLib, settings, mpc); an RDS receiver reads each byte as
// a code from table E.1, so a raw UTF-8 "é" (C3 A9) shows as two wrong characters and pushes every
// RT+ start/length after it out by one.
//
// - Printable ASCII 0x20-0x7E is passed through unchanged. Table E.1 differs from ASCII at 0x24
//   ("¤", with "$" at 0xAB), 0x5E, 0x60 and 0x7E, but receivers commonly treat this range as ASCII,
//   and this is what the plugin has always sent.
// - Characters in the upper half of table E.1 map to their code (é -> 0x82).
// - Typographic punctuation and accented letters the table lacks fold to an ASCII stand-in
//   (curly quotes -> ' ", dashes -> -, ł -> l). Other letters become '?', other symbols and
//   emoji are dropped.
// - Bytes that are not valid UTF-8 are taken as Latin-1.
//
// Every character produces at most one byte, so a converted string's length is its on-air
// length - RT+ positions and the 8/64-character limits can be computed on it directly.
//
// No function-local statics: in an inline function they become STB_GNU_UNIQUE symbols, which
// stop the plugin library from ever being unloaded. Namespace-scope constexpr data has none.
//
// Upper half of table E.1 from redsea (src/text/rdsstring.cc), plus the spec spellings it renders
// differently (0x8D ß, 0x9D ğ, 0xA4 Ğ). Shared verbatim between fpp-kfmt and fpp-vastfmt.

#include <cstdint>
#include <string>
#include <vector>

namespace rds_charset {

// Unicode code point for RDS codes 0x80-0xFF (0 = unused).
constexpr char32_t kUpper[128] = {
    0x00E1, 0x00E0, 0x00E9, 0x00E8, 0x00ED, 0x00EC, 0x00F3, 0x00F2, 0x00FA, 0x00F9, 0x00D1, 0x00C7, 0x015E, 0x03B2, 0x00A1, 0x0132,  // 0x80  á à é è í ì ó ò ú ù Ñ Ç Ş β ¡ Ĳ
    0x00E2, 0x00E4, 0x00EA, 0x00EB, 0x00EE, 0x00EF, 0x00F4, 0x00F6, 0x00FB, 0x00FC, 0x00F1, 0x00E7, 0x015F, 0x01E7, 0x0131, 0x0133,  // 0x90  â ä ê ë î ï ô ö û ü ñ ç ş ǧ ı ĳ
    0x00AA, 0x03B1, 0x00A9, 0x2030, 0x01E6, 0x011B, 0x0148, 0x0151, 0x03C0, 0x20AC, 0x00A3, 0x0024, 0x2190, 0x2191, 0x2192, 0x2193,  // 0xA0  ª α © ‰ Ǧ ě ň ő π € £ $ ← ↑ → ↓
    0x00BA, 0x00B9, 0x00B2, 0x00B3, 0x00B1, 0x0130, 0x0144, 0x0171, 0x00B5, 0x00BF, 0x00F7, 0x00B0, 0x00BC, 0x00BD, 0x00BE, 0x00A7,  // 0xB0  º ¹ ² ³ ± İ ń ű µ ¿ ÷ ° ¼ ½ ¾ §
    0x00C1, 0x00C0, 0x00C9, 0x00C8, 0x00CD, 0x00CC, 0x00D3, 0x00D2, 0x00DA, 0x00D9, 0x0158, 0x010C, 0x0160, 0x017D, 0x00D0, 0x013F,  // 0xC0  Á À É È Í Ì Ó Ò Ú Ù Ř Č Š Ž Ð Ŀ
    0x00C2, 0x00C4, 0x00CA, 0x00CB, 0x00CE, 0x00CF, 0x00D4, 0x00D6, 0x00DB, 0x00DC, 0x0159, 0x010D, 0x0161, 0x017E, 0x0111, 0x0140,  // 0xD0  Â Ä Ê Ë Î Ï Ô Ö Û Ü ř č š ž đ ŀ
    0x00C3, 0x00C5, 0x00C6, 0x0152, 0x0177, 0x00DD, 0x00D5, 0x00D8, 0x00DE, 0x014A, 0x0154, 0x0106, 0x015A, 0x0179, 0x0166, 0x00F0,  // 0xE0  Ã Å Æ Œ ŷ Ý Õ Ø Þ Ŋ Ŕ Ć Ś Ź Ŧ ð
    0x00E3, 0x00E5, 0x00E6, 0x0153, 0x0175, 0x00FD, 0x00F5, 0x00F8, 0x00FE, 0x014B, 0x0155, 0x0107, 0x015B, 0x017A, 0x0167, 0x0000,  // 0xF0  ã å æ œ ŵ ý õ ø þ ŋ ŕ ć ś ź ŧ ·
};

constexpr char32_t kSpecAlias[][2] = {{0x00DF, 0x8D}, {0x011F, 0x9D}, {0x011E, 0xA4}};

struct Fold {
    char32_t cp;
    char     ascii;
};
// Generated from Unicode decompositions (Latin-1 Supplement to Latin Extended-B) plus punctuation.
constexpr Fold kFolds[] = {
    {0x00A0, ' '}, {0x00A2, 'c'}, {0x00A5, 'Y'}, {0x00AB, '"'}, {0x00B4, '\''}, {0x00B7, '.'}, {0x00BB, '"'}, {0x00D7, 'x'},
    {0x00FF, 'y'}, {0x0100, 'A'}, {0x0101, 'a'}, {0x0102, 'A'}, {0x0103, 'a'}, {0x0104, 'A'}, {0x0105, 'a'}, {0x0108, 'C'},
    {0x0109, 'c'}, {0x010A, 'C'}, {0x010B, 'c'}, {0x010E, 'D'}, {0x010F, 'd'}, {0x0112, 'E'}, {0x0113, 'e'}, {0x0114, 'E'},
    {0x0115, 'e'}, {0x0116, 'E'}, {0x0117, 'e'}, {0x0118, 'E'}, {0x0119, 'e'}, {0x011A, 'E'}, {0x011C, 'G'}, {0x011D, 'g'},
    {0x0120, 'G'}, {0x0121, 'g'}, {0x0122, 'G'}, {0x0123, 'g'}, {0x0124, 'H'}, {0x0125, 'h'}, {0x0128, 'I'}, {0x0129, 'i'},
    {0x012A, 'I'}, {0x012B, 'i'}, {0x012C, 'I'}, {0x012D, 'i'}, {0x012E, 'I'}, {0x012F, 'i'}, {0x0134, 'J'}, {0x0135, 'j'},
    {0x0136, 'K'}, {0x0137, 'k'}, {0x0139, 'L'}, {0x013A, 'l'}, {0x013B, 'L'}, {0x013C, 'l'}, {0x013D, 'L'}, {0x013E, 'l'},
    {0x0143, 'N'}, {0x0145, 'N'}, {0x0146, 'n'}, {0x0147, 'N'}, {0x014C, 'O'}, {0x014D, 'o'}, {0x014E, 'O'}, {0x014F, 'o'},
    {0x0150, 'O'}, {0x0156, 'R'}, {0x0157, 'r'}, {0x015C, 'S'}, {0x015D, 's'}, {0x0162, 'T'}, {0x0163, 't'}, {0x0164, 'T'},
    {0x0165, 't'}, {0x0168, 'U'}, {0x0169, 'u'}, {0x016A, 'U'}, {0x016B, 'u'}, {0x016C, 'U'}, {0x016D, 'u'}, {0x016E, 'U'},
    {0x016F, 'u'}, {0x0170, 'U'}, {0x0172, 'U'}, {0x0173, 'u'}, {0x0174, 'W'}, {0x0176, 'Y'}, {0x0178, 'Y'}, {0x017B, 'Z'},
    {0x017C, 'z'}, {0x01A0, 'O'}, {0x01A1, 'o'}, {0x01AF, 'U'}, {0x01B0, 'u'}, {0x01CD, 'A'}, {0x01CE, 'a'}, {0x01CF, 'I'},
    {0x01D0, 'i'}, {0x01D1, 'O'}, {0x01D2, 'o'}, {0x01D3, 'U'}, {0x01D4, 'u'}, {0x01D5, 'U'}, {0x01D6, 'u'}, {0x01D7, 'U'},
    {0x01D8, 'u'}, {0x01D9, 'U'}, {0x01DA, 'u'}, {0x01DB, 'U'}, {0x01DC, 'u'}, {0x01DE, 'A'}, {0x01DF, 'a'}, {0x01E0, 'A'},
    {0x01E1, 'a'}, {0x01E8, 'K'}, {0x01E9, 'k'}, {0x01EA, 'O'}, {0x01EB, 'o'}, {0x01EC, 'O'}, {0x01ED, 'o'}, {0x01F0, 'j'},
    {0x01F4, 'G'}, {0x01F5, 'g'}, {0x01F8, 'N'}, {0x01F9, 'n'}, {0x01FA, 'A'}, {0x01FB, 'a'}, {0x0200, 'A'}, {0x0201, 'a'},
    {0x0202, 'A'}, {0x0203, 'a'}, {0x0204, 'E'}, {0x0205, 'e'}, {0x0206, 'E'}, {0x0207, 'e'}, {0x0208, 'I'}, {0x0209, 'i'},
    {0x020A, 'I'}, {0x020B, 'i'}, {0x020C, 'O'}, {0x020D, 'o'}, {0x020E, 'O'}, {0x020F, 'o'}, {0x0210, 'R'}, {0x0211, 'r'},
    {0x0212, 'R'}, {0x0213, 'r'}, {0x0214, 'U'}, {0x0215, 'u'}, {0x0216, 'U'}, {0x0217, 'u'}, {0x0218, 'S'}, {0x0219, 's'},
    {0x021A, 'T'}, {0x021B, 't'}, {0x021E, 'H'}, {0x021F, 'h'}, {0x0226, 'A'}, {0x0227, 'a'}, {0x0228, 'E'}, {0x0229, 'e'},
    {0x022A, 'O'}, {0x022B, 'o'}, {0x022C, 'O'}, {0x022D, 'o'}, {0x022E, 'O'}, {0x022F, 'o'}, {0x0230, 'O'}, {0x0231, 'o'},
    {0x0232, 'Y'}, {0x0233, 'y'}, {0x2010, '-'}, {0x2011, '-'}, {0x2012, '-'}, {0x2013, '-'}, {0x2014, '-'}, {0x2018, '\''},
    {0x2019, '\''}, {0x201A, '\''}, {0x201B, '\''}, {0x201C, '"'}, {0x201D, '"'}, {0x201E, '"'}, {0x201F, '"'}, {0x2022, '*'},
    {0x2026, '.'}, {0x2032, '\''}, {0x2033, '"'}, {0x2212, '-'},
};

inline int rdsCode(char32_t cp) {
    if (cp >= 0x20 && cp <= 0x7E) {
        return (int)cp;
    }
    for (auto &a : kSpecAlias) {
        if (a[0] == cp) return (int)a[1];
    }
    for (int i = 0; i < 128; i++) {
        if (kUpper[i] == cp && cp != 0) return 0x80 + i;
    }
    for (auto &f : kFolds) {
        if (f.cp == cp) return (unsigned char)f.ascii;
    }
    if (cp < 0x20 || cp == 0x7F) return ' ';   // controls: keep the spacing
    if (cp < 0x2000) return '?';               // a letter we cannot show
    return -1;                                 // symbol / emoji: drop
}

// Decodes one UTF-8 character starting at s[i]; advances i. Invalid input is read as Latin-1.
inline char32_t nextCodePoint(const std::string &s, size_t &i) {
    unsigned char c = (unsigned char)s[i];
    int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len > 1 && i + len <= s.size()) {
        char32_t cp = c & (0x7F >> len);
        bool ok = true;
        for (int k = 1; k < len; k++) {
            unsigned char cc = (unsigned char)s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (ok && cp >= (len == 2 ? 0x80u : len == 3 ? 0x800u : 0x10000u)) {
            i += len;
            return cp;
        }
    }
    i += 1;
    return c;
}

} // namespace rds_charset

// UTF-8 text -> RDS bytes, one byte per character (see above).
inline std::string utf8ToRDS(const std::string &utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        int code = rds_charset::rdsCode(rds_charset::nextCodePoint(utf8, i));
        if (code >= 0) out += (char)code;
    }
    return out;
}

// Number of characters (code points) in a UTF-8 string, counted the way utf8ToRDS reads it.
inline size_t utf8Length(const std::string &utf8) {
    size_t n = 0;
    for (size_t i = 0; i < utf8.size(); n++) rds_charset::nextCodePoint(utf8, i);
    return n;
}

// The first `chars` characters of a UTF-8 string, never splitting a multi-byte character.
inline std::string utf8Prefix(const std::string &utf8, size_t chars) {
    size_t i = 0;
    for (size_t n = 0; n < chars && i < utf8.size(); n++) rds_charset::nextCodePoint(utf8, i);
    return utf8.substr(0, i);
}

// Splits UTF-8 text into PS-sized pieces of `width` characters, the last one space-padded.
// Splitting by bytes would cut a multi-byte character in half.
inline std::vector<std::string> utf8Fragments(const std::string &utf8, size_t width = 8) {
    std::vector<std::string> out;
    for (size_t pos = 0; pos < utf8.size();) {
        std::string piece = utf8Prefix(utf8.substr(pos), width);
        pos += piece.size();
        piece.append(width - utf8Length(piece), ' ');
        out.push_back(piece);
    }
    return out;
}
