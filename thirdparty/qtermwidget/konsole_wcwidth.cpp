#include <QString>

#include "konsole_wcwidth.h"

// Windows does not provide wcwidth(). Implement the classic Markus Kuhn algorithm.
static int mk_wcwidth(wchar_t ucs) {
    if (ucs == 0) return 0;
    // combining characters (general category M* / zero-width)
    if (ucs < 0x0300 || ucs > 0x1090) {
        // Table of zero-width characters
        if ((ucs >= 0x0300 && ucs <= 0x036F) ||  // Combining Diacritical Marks
            (ucs >= 0x0483 && ucs <= 0x0486) ||  // Cyrillic combining marks
            (ucs >= 0x0591 && ucs <= 0x05BD) ||  // Hebrew combining marks
            (ucs >= 0x05BF && ucs <= 0x05BF) ||
            (ucs >= 0x05C1 && ucs <= 0x05C2) ||
            (ucs >= 0x05C4 && ucs <= 0x05C5) ||
            (ucs == 0x05BF) ||
            (ucs >= 0x0610 && ucs <= 0x061A) ||
            (ucs >= 0x064B && ucs <= 0x065E) ||
            (ucs >= 0x0670 && ucs <= 0x0670) ||
            (ucs >= 0x06D6 && ucs <= 0x06DC) ||
            (ucs >= 0x06DE && ucs <= 0x06E4) ||
            (ucs >= 0x06E7 && ucs <= 0x06E8) ||
            (ucs >= 0x06EA && ucs <= 0x06ED) ||
            (ucs >= 0x0711 && ucs <= 0x0711) ||
            (ucs >= 0x0730 && ucs <= 0x074A) ||
            (ucs >= 0x07A6 && ucs <= 0x07B0) ||
            (ucs >= 0x0901 && ucs <= 0x0902) ||
            (ucs >= 0x093C && ucs <= 0x093C) ||
            (ucs >= 0x0941 && ucs <= 0x0948) ||
            (ucs >= 0x094D && ucs <= 0x094D) ||
            (ucs >= 0x0951 && ucs <= 0x0954) ||
            (ucs >= 0x0962 && ucs <= 0x0963) ||
            (ucs >= 0x0981 && ucs <= 0x0981) ||
            (ucs >= 0x09BC && ucs <= 0x09BC) ||
            (ucs >= 0x09C1 && ucs <= 0x09C4) ||
            (ucs >= 0x09CD && ucs <= 0x09CD) ||
            (ucs >= 0x09E2 && ucs <= 0x09E3) ||
            (ucs >= 0x0A01 && ucs <= 0x0A02) ||
            (ucs >= 0x0A3C && ucs <= 0x0A3C) ||
            (ucs >= 0x0A41 && ucs <= 0x0A42) ||
            (ucs >= 0x0A47 && ucs <= 0x0A48) ||
            (ucs >= 0x0A4B && ucs <= 0x0A4D) ||
            (ucs >= 0x0A70 && ucs <= 0x0A71) ||
            (ucs >= 0x0A81 && ucs <= 0x0A82) ||
            (ucs >= 0x0ABC && ucs <= 0x0ABC) ||
            (ucs >= 0x0AC1 && ucs <= 0x0AC5) ||
            (ucs >= 0x0AC7 && ucs <= 0x0AC8) ||
            (ucs >= 0x0ACD && ucs <= 0x0ACD) ||
            (ucs >= 0x0AE2 && ucs <= 0x0AE3) ||
            (ucs >= 0x0B01 && ucs <= 0x0B01) ||
            (ucs >= 0x0B3C && ucs <= 0x0B3C) ||
            (ucs >= 0x0B3F && ucs <= 0x0B3F) ||
            (ucs >= 0x0B41 && ucs <= 0x0B43) ||
            (ucs >= 0x0B4D && ucs <= 0x0B4D) ||
            (ucs >= 0x0B56 && ucs <= 0x0B56) ||
            (ucs >= 0x0B82 && ucs <= 0x0B82) ||
            (ucs >= 0x0BC0 && ucs <= 0x0BC0) ||
            (ucs >= 0x0BCD && ucs <= 0x0BCD) ||
            (ucs >= 0x0C3E && ucs <= 0x0C40) ||
            (ucs >= 0x0C46 && ucs <= 0x0C48) ||
            (ucs >= 0x0C4A && ucs <= 0x0C4D) ||
            (ucs >= 0x0C55 && ucs <= 0x0C56) ||
            (ucs >= 0x0CBC && ucs <= 0x0CBC) ||
            (ucs >= 0x0CBF && ucs <= 0x0CBF) ||
            (ucs >= 0x0CC6 && ucs <= 0x0CC6) ||
            (ucs >= 0x0CCC && ucs <= 0x0CCD) ||
            (ucs >= 0x0CE2 && ucs <= 0x0CE3) ||
            (ucs >= 0x0D41 && ucs <= 0x0D43) ||
            (ucs >= 0x0D4D && ucs <= 0x0D4D) ||
            (ucs >= 0x0DCA && ucs <= 0x0DCA) ||
            (ucs >= 0x0DD2 && ucs <= 0x0DD4) ||
            (ucs >= 0x0DD6 && ucs <= 0x0DD6) ||
            (ucs >= 0x0E31 && ucs <= 0x0E31) ||
            (ucs >= 0x0E34 && ucs <= 0x0E3A) ||
            (ucs >= 0x0E47 && ucs <= 0x0E4E) ||
            (ucs >= 0x0EB1 && ucs <= 0x0EB1) ||
            (ucs >= 0x0EB4 && ucs <= 0x0EB9) ||
            (ucs >= 0x0EBB && ucs <= 0x0EBC) ||
            (ucs >= 0x0EC8 && ucs <= 0x0ECD) ||
            (ucs >= 0x0F18 && ucs <= 0x0F19) ||
            (ucs >= 0x0F35 && ucs <= 0x0F35) ||
            (ucs >= 0x0F37 && ucs <= 0x0F37) ||
            (ucs >= 0x0F39 && ucs <= 0x0F39) ||
            (ucs >= 0x0F71 && ucs <= 0x0F7E) ||
            (ucs >= 0x0F80 && ucs <= 0x0F84) ||
            (ucs >= 0x0F86 && ucs <= 0x0F87) ||
            (ucs >= 0x0F90 && ucs <= 0x0F97) ||
            (ucs >= 0x0F99 && ucs <= 0x0FBC) ||
            (ucs >= 0x0FC6 && ucs <= 0x0FC6) ||
            (ucs >= 0x102D && ucs <= 0x1030) ||
            (ucs >= 0x1032 && ucs <= 0x1032) ||
            (ucs >= 0x1036 && ucs <= 0x1037) ||
            (ucs >= 0x1039 && ucs <= 0x1039) ||
            (ucs >= 0x1058 && ucs <= 0x1059) ||
            (ucs >= 0x1160 && ucs <= 0x11FF) ||  // Hangul Jungseong/Jongseong
            (ucs >= 0x135F && ucs <= 0x135F) ||
            (ucs >= 0x1712 && ucs <= 0x1714) ||
            (ucs >= 0x1732 && ucs <= 0x1734) ||
            (ucs >= 0x1752 && ucs <= 0x1753) ||
            (ucs >= 0x1772 && ucs <= 0x1773) ||
            (ucs >= 0x17B4 && ucs <= 0x17B5) ||
            (ucs >= 0x17B7 && ucs <= 0x17BD) ||
            (ucs >= 0x17C6 && ucs <= 0x17C6) ||
            (ucs >= 0x17C9 && ucs <= 0x17D3) ||
            (ucs >= 0x17DD && ucs <= 0x17DD) ||
            (ucs >= 0x180B && ucs <= 0x180D) ||
            (ucs >= 0x18A9 && ucs <= 0x18A9) ||
            (ucs >= 0x1920 && ucs <= 0x1922) ||
            (ucs >= 0x1927 && ucs <= 0x1928) ||
            (ucs >= 0x1932 && ucs <= 0x1932) ||
            (ucs >= 0x1939 && ucs <= 0x193B) ||
            (ucs >= 0x1A17 && ucs <= 0x1A18) ||
            (ucs >= 0x1DC0 && ucs <= 0x1DFF) ||
            (ucs >= 0x200B && ucs <= 0x200F) ||  // ZW* / ZWNJ / ZWJ / LRM / RLM
            (ucs >= 0x2028 && ucs <= 0x2029) ||
            (ucs >= 0x202A && ucs <= 0x202E) ||
            (ucs >= 0x2060 && ucs <= 0x2063) ||
            (ucs >= 0x206A && ucs <= 0x206F) ||
            (ucs >= 0x20D0 && ucs <= 0x20EF) ||  // combining marks for symbols
            (ucs >= 0x302A && ucs <= 0x302F) ||
            (ucs >= 0x3099 && ucs <= 0x309A) ||
            (ucs >= 0xA806 && ucs <= 0xA806) ||
            (ucs >= 0xA80B && ucs <= 0xA80B) ||
            (ucs >= 0xA825 && ucs <= 0xA826) ||
            (ucs >= 0xFB1E && ucs <= 0xFB1E) ||
            (ucs >= 0xFE00 && ucs <= 0xFE0F) ||  // variation selectors
            (ucs >= 0xFE20 && ucs <= 0xFE23) ||
            (ucs >= 0xFEFF && ucs <= 0xFEFF) ||  // BOM
            (ucs >= 0xFFF9 && ucs <= 0xFFFB) ||
            (ucs >= 0x10A01 && ucs <= 0x10A03) ||
            (ucs >= 0x10A05 && ucs <= 0x10A06) ||
            (ucs >= 0x10A0C && ucs <= 0x10A0F) ||
            (ucs >= 0x10A38 && ucs <= 0x10A3A) ||
            (ucs >= 0x10A3F && ucs <= 0x10A3F) ||
            (ucs >= 0x1D167 && ucs <= 0x1D169) ||
            (ucs >= 0x1D173 && ucs <= 0x1D182) ||
            (ucs >= 0xE0100 && ucs <= 0xE01EF)) {
            return 0;
        }
    }
    // CJK
    if (ucs >= 0x1100 && (ucs <= 0x115F || ucs == 0x2329 || ucs == 0x232A ||
        (ucs >= 0x2E80 && ucs <= 0x2FFF) ||
        (ucs >= 0x3000 && ucs <= 0x303E) ||
        (ucs >= 0x3040 && ucs <= 0x33FF) ||
        (ucs >= 0x3400 && ucs <= 0x4DB5) ||
        (ucs >= 0x4E00 && ucs <= 0x9FBB) ||
        (ucs >= 0xA000 && ucs <= 0xA4CF) ||
        (ucs >= 0xAC00 && ucs <= 0xD7A3) ||
        (ucs >= 0xF900 && ucs <= 0xFAFF) ||
        (ucs >= 0xFE10 && ucs <= 0xFE19) ||
        (ucs >= 0xFE30 && ucs <= 0xFE6F) ||
        (ucs >= 0xFF01 && ucs <= 0xFF60) ||
        (ucs >= 0xFFE0 && ucs <= 0xFFE6) ||
        (ucs >= 0x20000 && ucs <= 0x2FFFD) ||
        (ucs >= 0x30000 && ucs <= 0x3FFFD))) {
        return 2;
    }
    return 1;
}

int konsole_wcwidth(wchar_t ucs)
{
    return mk_wcwidth(ucs);
}

int string_width( const std::wstring & wstr )
{
    int w = 0;
    for ( size_t i = 0; i < wstr.length(); ++i ) {
        w += konsole_wcwidth( wstr[ i ] );
    }
    return w;
}
