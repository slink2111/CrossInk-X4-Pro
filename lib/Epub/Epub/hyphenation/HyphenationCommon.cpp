#include "HyphenationCommon.h"

#include <Utf8.h>

namespace {

// Convert Latin uppercase letters (ASCII plus Latin-1 supplement) to lowercase
uint32_t toLowerLatinImpl(const uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') {
    return cp - 'A' + 'a';
  }
  if ((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00DE)) {
    return cp + 0x20;
  }

  // Latin Extended-A (U+0100..U+017E): uppercase letters are paired with
  // lowercase at cp+1. Two sub-ranges have different alignment:
  //   U+0100..U+0137: uppercase on EVEN codepoints
  //   U+0139..U+0148: uppercase on ODD codepoints
  //   U+014A..U+0177: uppercase on EVEN codepoints
  //   U+0179..U+017E: uppercase on ODD codepoints
  // Covers Polish (Ą/ą, Ć/ć, Ę/ę, Ł/ł, Ń/ń, Ś/ś, Ź/ź, Ż/ż), Czech, Hungarian, Turkish, etc.
  if ((cp >= 0x0100 && cp <= 0x0137 && (cp % 2 == 0)) || (cp >= 0x0139 && cp <= 0x0148 && (cp % 2 == 1)) ||
      (cp >= 0x014A && cp <= 0x0177 && (cp % 2 == 0)) || (cp >= 0x0179 && cp <= 0x017E && (cp % 2 == 1))) {
    return cp + 1;
  }

  switch (cp) {
    case 0x0178:      // Ÿ
      return 0x00FF;  // ÿ
    case 0x1E9E:      // ẞ
      return 0x00DF;  // ß
    default:
      return cp;
  }
}

// Convert Cyrillic uppercase letters to lowercase
// Cyrillic uppercase range 0x0410-0x042F maps to lowercase by adding 0x20
// Special case: Cyrillic capital IO (0x0401) maps to lowercase io (0x0451)
uint32_t toLowerCyrillicImpl(const uint32_t cp) {
  if (cp >= 0x0410 && cp <= 0x042F) {
    return cp + 0x20;
  }
  if (cp == 0x0401) {
    return 0x0451;
  }
  return cp;
}

// Convert Greek uppercase letters (monotonic & polytonic) to lowercase
uint32_t toLowerGreekImpl(const uint32_t cp) {
  // Basic Greek capitals: Α (0x0391) to Ω (0x03A9) except unassigned 0x03A2
  if (cp >= 0x0391 && cp <= 0x03A1) {
    return cp + 0x20;
  }
  if (cp >= 0x03A3 && cp <= 0x03A9) {
    return cp + 0x20;  // Note: Σ maps to σ (0x03C3)
  }
  switch (cp) {
    case 0x0386: return 0x03AC;  // Ά -> ά
    case 0x0388: return 0x03AD;  // Έ -> έ
    case 0x0389: return 0x03AE;  // Ή -> ή
    case 0x038A: return 0x03AF;  // Ί -> ί
    case 0x038C: return 0x03CC;  // Ό -> ό
    case 0x038E: return 0x03CD;  // Ύ -> ύ
    case 0x038F: return 0x03CE;  // Ώ -> ώ
    case 0x03AA: return 0x03CA;  // Ϊ -> ϊ
    case 0x03AB: return 0x03CB;  // Ϋ -> ϋ
    case 0x03FA: return 0x03FB;  // Ϻ -> ϻ
    default: break;
  }
  if ((cp >= 0x0370 && cp <= 0x0373 && (cp % 2 == 0)) ||
      (cp >= 0x0376 && cp <= 0x0377 && (cp % 2 == 0)) ||
      (cp >= 0x03D8 && cp <= 0x03EE && (cp % 2 == 0))) {
    return cp + 1;
  }
  if (cp == 0x037F) return 0x03F3;
  if (cp == 0x03CF) return 0x03D7;
  if (cp == 0x03F4) return 0x03B8;
  if (cp == 0x03F7) return 0x03F8;
  if (cp == 0x03F9) return 0x03F2;
  if (cp >= 0x03FD && cp <= 0x03FF) return cp - 130;

  // Polytonic Greek capitals (0x1F00..0x1FFF)
  if ((cp >= 0x1F08 && cp <= 0x1F0F) || (cp >= 0x1F18 && cp <= 0x1F1D) ||
      (cp >= 0x1F28 && cp <= 0x1F2F) || (cp >= 0x1F38 && cp <= 0x1F3F) ||
      (cp >= 0x1F48 && cp <= 0x1F4D) || (cp >= 0x1F59 && cp <= 0x1F5F && (cp % 2 == 1)) ||
      (cp >= 0x1F68 && cp <= 0x1F6F) || (cp >= 0x1F88 && cp <= 0x1F8F) ||
      (cp >= 0x1F98 && cp <= 0x1F9F) || (cp >= 0x1FA8 && cp <= 0x1FAF) ||
      (cp >= 0x1FB8 && cp <= 0x1FB9) || (cp >= 0x1FD8 && cp <= 0x1FD9) ||
      (cp >= 0x1FE8 && cp <= 0x1FE9)) {
    return cp - 8;
  }
  if (cp >= 0x1FBA && cp <= 0x1FBB) return cp - 0x4A;
  if (cp == 0x1FBC) return 0x1FB3;
  if (cp >= 0x1FC8 && cp <= 0x1FCB) return cp - 0x56;
  if (cp == 0x1FCC) return 0x1FC3;
  if (cp >= 0x1FDA && cp <= 0x1FDB) return cp - 0x64;
  if (cp >= 0x1FEA && cp <= 0x1FEB) return cp - 0x70;
  if (cp == 0x1FEC) return 0x1FE5;
  if (cp >= 0x1FF8 && cp <= 0x1FF9) return cp - 0x80;
  if (cp >= 0x1FFA && cp <= 0x1FFB) return cp - 0x7E;
  if (cp == 0x1FFC) return 0x1FF3;

  return cp;
}

}  // namespace

uint32_t toLowerLatin(const uint32_t cp) { return toLowerLatinImpl(cp); }

uint32_t toLowerCyrillic(const uint32_t cp) { return toLowerCyrillicImpl(cp); }

uint32_t toLowerGreek(const uint32_t cp) { return toLowerGreekImpl(cp); }

bool isLatinLetter(const uint32_t cp) {
  if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
    return true;
  }

  if (((cp >= 0x00C0 && cp <= 0x00D6) || (cp >= 0x00D8 && cp <= 0x00F6) || (cp >= 0x00F8 && cp <= 0x00FF)) &&
      cp != 0x00D7 && cp != 0x00F7) {
    return true;
  }

  // Latin Extended-A (U+0100..U+017F): Polish, Czech, Hungarian, Turkish, etc.
  if (cp >= 0x0100 && cp <= 0x017F) {
    return true;
  }

  switch (cp) {
    case 0x0152:  // Œ
    case 0x0153:  // œ
    case 0x0178:  // Ÿ
    case 0x1E9E:  // ẞ
      return true;
    default:
      return false;
  }
}

bool isCyrillicLetter(const uint32_t cp) { return (cp >= 0x0400 && cp <= 0x052F); }

bool isGreekLetter(const uint32_t cp) {
  if (cp >= 0x0370 && cp <= 0x03FF) {
    if (cp == 0x0374 || cp == 0x0375 || cp == 0x037E || cp == 0x0384 || cp == 0x0385 || cp == 0x0387) {
      return false;
    }
    return true;
  }
  if (cp >= 0x1F00 && cp <= 0x1FFF) {
    if ((cp >= 0x1FBD && cp <= 0x1FC1) || (cp >= 0x1FCD && cp <= 0x1FCF) ||
        (cp >= 0x1FDD && cp <= 0x1FDF) || (cp >= 0x1FED && cp <= 0x1FEF) ||
        (cp >= 0x1FFD && cp <= 0x1FFE)) {
      return false;
    }
    return true;
  }
  return false;
}

bool isAlphabetic(const uint32_t cp) {
  return isLatinLetter(cp) || isCyrillicLetter(cp) || isGreekLetter(cp);
}

bool isPunctuation(const uint32_t cp) {
  switch (cp) {
    case '-':
    case '.':
    case ',':
    case '!':
    case '?':
    case ';':
    case ':':
    case '"':
    case '\'':
    case ')':
    case '(':
    case 0x00AB:  // «
    case 0x00BB:  // »
    case 0x2018:  // ‘
    case 0x2019:  // ’
    case 0x201A:  // ‚
    case 0x201C:  // “
    case 0x201D:  // ”
    case 0x201E:  // „
    case 0x00A0:  // no-break space
    case '{':
    case '}':
    case '[':
    case ']':
    case '/':
    case 0x2039:  // ‹
    case 0x203A:  // ›
    case 0x2026:  // …
      return true;
    default:
      return false;
  }
}

bool isAsciiDigit(const uint32_t cp) { return cp >= '0' && cp <= '9'; }

bool isApostrophe(const uint32_t cp) {
  switch (cp) {
    case '\'':
    case 0x2018:  // left single quotation mark
    case 0x2019:  // right single quotation mark
      return true;
    default:
      return false;
  }
}

bool isExplicitHyphen(const uint32_t cp) {
  switch (cp) {
    case '-':
    case 0x00AD:  // soft hyphen
    case 0x058A:  // Armenian hyphen
    case 0x2010:  // hyphen
    case 0x2011:  // non-breaking hyphen
    case 0x2012:  // figure dash
    case 0x2013:  // en dash
    case 0x2014:  // em dash
    case 0x2015:  // horizontal bar
    case 0x2043:  // hyphen bullet
    case 0x207B:  // superscript minus
    case 0x208B:  // subscript minus
    case 0x2212:  // minus sign
    case 0x2E17:  // double oblique hyphen
    case 0x2E3A:  // two-em dash
    case 0x2E3B:  // three-em dash
    case 0xFE58:  // small em dash
    case 0xFE63:  // small hyphen-minus
    case 0xFF0D:  // fullwidth hyphen-minus
    case 0x005F:  // Underscore
    case 0x2026:  // Ellipsis
      return true;
    default:
      return false;
  }
}

bool isSoftHyphen(const uint32_t cp) { return cp == 0x00AD; }

void trimSurroundingPunctuationAndFootnote(std::vector<CodepointInfo>& cps) {
  if (cps.empty()) {
    return;
  }

  // Remove trailing footnote references like [12], even if punctuation trails after the closing bracket.
  if (cps.size() >= 3) {
    int end = static_cast<int>(cps.size()) - 1;
    while (end >= 0 && isPunctuation(cps[end].value)) {
      --end;
    }
    int pos = end;
    if (pos >= 0 && isAsciiDigit(cps[pos].value)) {
      while (pos >= 0 && isAsciiDigit(cps[pos].value)) {
        --pos;
      }
      if (pos >= 0 && cps[pos].value == '[' && end - pos > 1) {
        cps.erase(cps.begin() + pos, cps.end());
      }
    }
  }

  while (!cps.empty() && isPunctuation(cps.front().value)) {
    cps.erase(cps.begin());
  }
  while (!cps.empty() && isPunctuation(cps.back().value)) {
    cps.pop_back();
  }
}

std::vector<CodepointInfo> collectCodepoints(const std::string& word) {
  std::vector<CodepointInfo> cps;
  cps.reserve(word.size());

  const unsigned char* base = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* ptr = base;
  while (*ptr != 0) {
    const unsigned char* current = ptr;
    const uint32_t cp = utf8NextCodepoint(&ptr);
    // If this is a combining diacritic (e.g., U+0301 = acute) and there's
    // a previous base character that can be composed into a single
    // precomposed Unicode scalar (Latin-1 / Latin-Extended), do that
    // composition here. This provides lightweight NFC-like behavior for
    // common Western European diacritics (acute, grave, circumflex, tilde,
    // diaeresis, cedilla) without pulling in a full Unicode normalization
    // library.
    if (!cps.empty()) {
      uint32_t prev = cps.back().value;
      uint32_t composed = 0;
      switch (cp) {
        case 0x0300:  // grave
          switch (prev) {
            case 0x0041:
              composed = 0x00C0;
              break;  // A -> À
            case 0x0061:
              composed = 0x00E0;
              break;  // a -> à
            case 0x0045:
              composed = 0x00C8;
              break;  // E -> È
            case 0x0065:
              composed = 0x00E8;
              break;  // e -> è
            case 0x0049:
              composed = 0x00CC;
              break;  // I -> Ì
            case 0x0069:
              composed = 0x00EC;
              break;  // i -> ì
            case 0x004F:
              composed = 0x00D2;
              break;  // O -> Ò
            case 0x006F:
              composed = 0x00F2;
              break;  // o -> ò
            case 0x0055:
              composed = 0x00D9;
              break;  // U -> Ù
            case 0x0075:
              composed = 0x00F9;
              break;  // u -> ù
            default:
              break;
          }
          break;
        case 0x0301:  // acute
          switch (prev) {
            case 0x0041:
              composed = 0x00C1;
              break;  // A -> Á
            case 0x0061:
              composed = 0x00E1;
              break;  // a -> á
            case 0x0045:
              composed = 0x00C9;
              break;  // E -> É
            case 0x0065:
              composed = 0x00E9;
              break;  // e -> é
            case 0x0049:
              composed = 0x00CD;
              break;  // I -> Í
            case 0x0069:
              composed = 0x00ED;
              break;  // i -> í
            case 0x004F:
              composed = 0x00D3;
              break;  // O -> Ó
            case 0x006F:
              composed = 0x00F3;
              break;  // o -> ó
            case 0x0055:
              composed = 0x00DA;
              break;  // U -> Ú
            case 0x0075:
              composed = 0x00FA;
              break;  // u -> ú
            case 0x0059:
              composed = 0x00DD;
              break;  // Y -> Ý
            case 0x0079:
              composed = 0x00FD;
              break;  // y -> ý
            case 0x0043:
              composed = 0x0106;
              break;  // C -> Ć
            case 0x0063:
              composed = 0x0107;
              break;  // c -> ć
            case 0x004E:
              composed = 0x0143;
              break;  // N -> Ń
            case 0x006E:
              composed = 0x0144;
              break;  // n -> ń
            case 0x0053:
              composed = 0x015A;
              break;  // S -> Ś
            case 0x0073:
              composed = 0x015B;
              break;  // s -> ś
            case 0x005A:
              composed = 0x0179;
              break;  // Z -> Ź
            case 0x007A:
              composed = 0x017A;
              break;  // z -> ź
            default:
              break;
          }
          break;
        case 0x0302:  // circumflex
          switch (prev) {
            case 0x0041:
              composed = 0x00C2;
              break;  // A -> Â
            case 0x0061:
              composed = 0x00E2;
              break;  // a -> â
            case 0x0045:
              composed = 0x00CA;
              break;  // E -> Ê
            case 0x0065:
              composed = 0x00EA;
              break;  // e -> ê
            case 0x0049:
              composed = 0x00CE;
              break;  // I -> Î
            case 0x0069:
              composed = 0x00EE;
              break;  // i -> î
            case 0x004F:
              composed = 0x00D4;
              break;  // O -> Ô
            case 0x006F:
              composed = 0x00F4;
              break;  // o -> ô
            case 0x0055:
              composed = 0x00DB;
              break;  // U -> Û
            case 0x0075:
              composed = 0x00FB;
              break;  // u -> û
            default:
              break;
          }
          break;
        case 0x0303:  // tilde
          switch (prev) {
            case 0x0041:
              composed = 0x00C3;
              break;  // A -> Ã
            case 0x0061:
              composed = 0x00E3;
              break;  // a -> ã
            case 0x004E:
              composed = 0x00D1;
              break;  // N -> Ñ
            case 0x006E:
              composed = 0x00F1;
              break;  // n -> ñ
            default:
              break;
          }
          break;
        case 0x0308:  // diaeresis/umlaut
          switch (prev) {
            case 0x0041:
              composed = 0x00C4;
              break;  // A -> Ä
            case 0x0061:
              composed = 0x00E4;
              break;  // a -> ä
            case 0x0045:
              composed = 0x00CB;
              break;  // E -> Ë
            case 0x0065:
              composed = 0x00EB;
              break;  // e -> ë
            case 0x0049:
              composed = 0x00CF;
              break;  // I -> Ï
            case 0x0069:
              composed = 0x00EF;
              break;  // i -> ï
            case 0x004F:
              composed = 0x00D6;
              break;  // O -> Ö
            case 0x006F:
              composed = 0x00F6;
              break;  // o -> ö
            case 0x0055:
              composed = 0x00DC;
              break;  // U -> Ü
            case 0x0075:
              composed = 0x00FC;
              break;  // u -> ü
            case 0x0059:
              composed = 0x0178;
              break;  // Y -> Ÿ
            case 0x0079:
              composed = 0x00FF;
              break;  // y -> ÿ
            default:
              break;
          }
          break;
        case 0x0307:  // dot above (Polish Ż)
          switch (prev) {
            case 0x005A:
              composed = 0x017B;
              break;  // Z -> Ż
            case 0x007A:
              composed = 0x017C;
              break;  // z -> ż
            default:
              break;
          }
          break;
        case 0x0327:  // cedilla
          switch (prev) {
            case 0x0043:
              composed = 0x00C7;
              break;  // C -> Ç
            case 0x0063:
              composed = 0x00E7;
              break;  // c -> ç
            default:
              break;
          }
          break;
        case 0x0328:  // ogonek (Polish Ą, Ę)
          switch (prev) {
            case 0x0041:
              composed = 0x0104;
              break;  // A -> Ą
            case 0x0061:
              composed = 0x0105;
              break;  // a -> ą
            case 0x0045:
              composed = 0x0118;
              break;  // E -> Ę
            case 0x0065:
              composed = 0x0119;
              break;  // e -> ę
            default:
              break;
          }
          break;
        default:
          break;
      }

      if (composed != 0) {
        cps.back().value = composed;
        continue;  // skip pushing the combining mark itself
      }
    }

    cps.push_back({cp, static_cast<size_t>(current - base)});
  }

  return cps;
}
