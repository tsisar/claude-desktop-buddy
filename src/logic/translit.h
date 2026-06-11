#pragma once
#include <stdint.h>
#include <stddef.h>

// Pure, Arduino-free logic — host-testable via `pio test -e native`.
//
// Sanitise incoming UTF-8 into the built-in 6x8 font's character set:
// strip control bytes, pass ASCII through, romanise Cyrillic, map common
// punctuation, and fall back to '?' for anything else (emoji / CJK). The
// panel has no working Cyrillic glyph font, so romanised Latin is the
// readable option. RU + UA coverage.

inline const char* cyrillicTranslit(uint32_t cp) {
  switch (cp) {
    // Russian uppercase A..Я
    case 0x0410: return "A";    case 0x0411: return "B";
    case 0x0412: return "V";    case 0x0413: return "H";   // Г → H (UA) / G (RU)
    case 0x0414: return "D";    case 0x0415: return "E";
    case 0x0416: return "Zh";   case 0x0417: return "Z";
    case 0x0418: return "I";    case 0x0419: return "Y";
    case 0x041A: return "K";    case 0x041B: return "L";
    case 0x041C: return "M";    case 0x041D: return "N";
    case 0x041E: return "O";    case 0x041F: return "P";
    case 0x0420: return "R";    case 0x0421: return "S";
    case 0x0422: return "T";    case 0x0423: return "U";
    case 0x0424: return "F";    case 0x0425: return "Kh";
    case 0x0426: return "Ts";   case 0x0427: return "Ch";
    case 0x0428: return "Sh";   case 0x0429: return "Shch";
    case 0x042A: return "'";    case 0x042B: return "Y";
    case 0x042C: return "'";    case 0x042D: return "E";
    case 0x042E: return "Yu";   case 0x042F: return "Ya";
    // Lowercase
    case 0x0430: return "a";    case 0x0431: return "b";
    case 0x0432: return "v";    case 0x0433: return "h";
    case 0x0434: return "d";    case 0x0435: return "e";
    case 0x0436: return "zh";   case 0x0437: return "z";
    case 0x0438: return "i";    case 0x0439: return "y";
    case 0x043A: return "k";    case 0x043B: return "l";
    case 0x043C: return "m";    case 0x043D: return "n";
    case 0x043E: return "o";    case 0x043F: return "p";
    case 0x0440: return "r";    case 0x0441: return "s";
    case 0x0442: return "t";    case 0x0443: return "u";
    case 0x0444: return "f";    case 0x0445: return "kh";
    case 0x0446: return "ts";   case 0x0447: return "ch";
    case 0x0448: return "sh";   case 0x0449: return "shch";
    case 0x044A: return "'";    case 0x044B: return "y";
    case 0x044C: return "'";    case 0x044D: return "e";
    case 0x044E: return "yu";   case 0x044F: return "ya";
    // Ukrainian-specific
    case 0x0404: return "Ye";   case 0x0454: return "ye";
    case 0x0406: return "I";    case 0x0456: return "i";
    case 0x0407: return "Yi";   case 0x0457: return "yi";
    case 0x0490: return "G";    case 0x0491: return "g";
    // Yo (used in RU)
    case 0x0401: return "Yo";   case 0x0451: return "yo";
    default:     return nullptr;
  }
}

// The "ascii" name predates the transliteration and is kept to avoid
// churning every call site.
inline void asciiCopy(char* dst, size_t dstLen, const char* src) {
  if (!dstLen) return;
  size_t j = 0, i = 0;
  while (src[i] && j + 1 < dstLen) {
    unsigned char c = (unsigned char)src[i];
    if (c < 0x20) { i++; continue; }                       // control byte
    if (c < 0x80) { dst[j++] = (char)c; i++; continue; }   // ASCII passthrough
    // Decode one UTF-8 multibyte sequence into a codepoint.
    uint32_t cp; int n;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else { i++; continue; }                                // stray continuation
    i++;
    while (n-- > 0 && ((unsigned char)src[i] & 0xC0) == 0x80) {
      cp = (cp << 6) | ((unsigned char)src[i] & 0x3F); i++;
    }
    const char* t = cyrillicTranslit(cp);                  // Cyrillic → Latin
    if (!t) switch (cp) {                                  // common punctuation
      case 0x2014: case 0x2013: t = "-";   break;          // em / en dash
      case 0x2018: case 0x2019: t = "'";   break;          // curly single quote
      case 0x201C: case 0x201D: t = "\"";  break;          // curly double quote
      case 0x2026:              t = "...";  break;          // ellipsis
      default: break;
    }
    if (t) { for (; *t && j + 1 < dstLen; t++) dst[j++] = *t; }
    else if (j + 1 < dstLen) dst[j++] = '?';               // unknown (emoji/CJK)
  }
  dst[j] = 0;
}
