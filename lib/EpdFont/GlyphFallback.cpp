#include "GlyphFallback.h"

#include <cstddef>

namespace {

struct Fallback {
  uint16_t from;
  uint16_t to;
};

// Sorted by `from` for binary search.
constexpr Fallback FALLBACKS[] = {
    {0x00E6, 'a'},      // æ latin small letter ae
    {0x00F0, 'd'},      // ð latin small letter eth
    {0x014B, 'n'},      // ŋ eng
    {0x0153, 'e'},      // œ latin small ligature oe
    {0x0250, 'a'},      // ɐ turned a
    {0x0251, 'a'},      // ɑ script a
    {0x0252, 'a'},      // ɒ turned script a
    {0x0253, 'b'},      // ɓ b with hook
    {0x0254, 'o'},      // ɔ open o
    {0x0256, 'd'},      // ɖ d with tail
    {0x0257, 'd'},      // ɗ d with hook
    {0x0259, 'e'},      // ə schwa
    {0x025A, 'e'},      // ɚ schwa with hook
    {0x025B, 'e'},      // ɛ open e
    {0x025C, 'e'},      // ɜ reversed open e
    {0x025D, 'e'},      // ɝ reversed open e with hook
    {0x025E, 'e'},      // ɞ closed reversed open e
    {0x025F, 'j'},      // ɟ dotless j with stroke
    {0x0260, 'g'},      // ɠ g with hook
    {0x0261, 'g'},      // ɡ script g
    {0x0263, 'y'},      // ɣ latin small letter gamma
    {0x0265, 'y'},      // ɥ turned h
    {0x0266, 'h'},      // ɦ h with hook
    {0x0268, 'i'},      // ɨ i with stroke
    {0x026A, 'i'},      // ɪ small capital I
    {0x026B, 'l'},      // ɫ l with middle tilde
    {0x026C, 'l'},      // ɬ l with belt
    {0x026D, 'l'},      // ɭ l with retroflex hook
    {0x026F, 'w'},      // ɯ turned m
    {0x0270, 'w'},      // ɰ turned m with long leg
    {0x0271, 'm'},      // ɱ m with hook
    {0x0272, 'n'},      // ɲ n with left hook
    {0x0273, 'n'},      // ɳ n with retroflex hook
    {0x0274, 'n'},      // ɴ small capital N
    {0x0275, 'o'},      // ɵ barred o
    {0x0277, 'w'},      // ɷ closed omega
    {0x0279, 'r'},      // ɹ turned r
    {0x027A, 'r'},      // ɺ turned r with long leg
    {0x027B, 'r'},      // ɻ turned r with hook
    {0x027D, 'r'},      // ɽ r with tail
    {0x027E, 'r'},      // ɾ r with fishhook
    {0x0280, 'r'},      // ʀ small capital R
    {0x0281, 'r'},      // ʁ inverted small capital R
    {0x0282, 's'},      // ʂ s with hook
    {0x0283, 's'},      // ʃ esh
    {0x0288, 't'},      // ʈ t with retroflex hook
    {0x0289, 'u'},      // ʉ u bar
    {0x028A, 'u'},      // ʊ upsilon
    {0x028B, 'v'},      // ʋ v with hook
    {0x028C, 'v'},      // ʌ turned v
    {0x028D, 'w'},      // ʍ turned w
    {0x028E, 'y'},      // ʎ turned y
    {0x0290, 'z'},      // ʐ z with retroflex hook
    {0x0291, 'z'},      // ʑ z with curl
    {0x0292, 'z'},      // ʒ ezh
    {0x0294, '\''},     // ʔ glottal stop
    {0x0295, '\''},     // ʕ pharyngeal voiced fricative
    {0x029D, 'j'},      // ʝ j with crossed tail
    {0x029F, 'l'},      // ʟ small capital L
    {0x02A3, 'd'},      // ʣ dz digraph
    {0x02A4, 'j'},      // ʤ dezh digraph
    {0x02A5, 'd'},      // ʥ dz digraph with curl
    {0x02A6, 't'},      // ʦ ts digraph
    {0x02A7, 'c'},      // ʧ tesh digraph
    {0x02A8, 't'},      // ʨ tc digraph with curl
    {0x02B0, 'h'},      // ʰ modifier h
    {0x02B2, 'j'},      // ʲ modifier j
    {0x02B7, 'w'},      // ʷ modifier w
    {0x02BC, '\''},     // ʼ modifier apostrophe
    {0x02C8, '\''},     // ˈ primary stress
    {0x02CC, ','},      // ˌ secondary stress
    {0x02D0, ':'},      // ː length mark
    {0x02D1, ':'},      // ˑ half-length mark
    {0x02E1, 'l'},      // ˡ modifier l
    {0x039B, 'A'},      // Λ capital lambda
    {0x03B8, 'o'},      // θ theta (if font lacks Greek theta)
    {0x1E37, 'l'},      // ḷ
    {0x1E71, 't'},      // ṱ
    {0x2190, '<'},      // ←
    {0x2191, '^'},      // ↑
    {0x2192, '>'},      // →
    {0x2193, 'v'},      // ↓
    {0x21C6, '>'},      // ⇆
    {0x222B, 'f'},      // ∫
    {0x223C, '~'},      // ∼
    {0x2248, '~'},      // ≈
    {0x25AA, 0x2022},   // ▪
    {0x25AB, 0x2022},   // ▫
    {0x25C6, 0x2022},   // ◆
    {0x25CF, 0x2022},   // ●
};

constexpr size_t FALLBACK_COUNT = sizeof(FALLBACKS) / sizeof(FALLBACKS[0]);

constexpr bool fallbacksAreSorted() {
  for (size_t i = 1; i < FALLBACK_COUNT; i++) {
    if (FALLBACKS[i - 1].from >= FALLBACKS[i].from) return false;
  }
  return true;
}
static_assert(fallbacksAreSorted(), "FALLBACKS must be sorted by `from` and free of duplicates");

uint32_t polytonicGreekFallback(uint32_t cp) {
  if (cp < 0x1F00 || cp > 0x1FFE) return cp;
  if (cp <= 0x1F07) return 0x03B1;  // α
  if (cp <= 0x1F0F) return 0x0391;  // Α
  if (cp <= 0x1F15) return 0x03B5;  // ε
  if (cp >= 0x1F18 && cp <= 0x1F1D) return 0x0395;  // Ε
  if (cp >= 0x1F20 && cp <= 0x1F27) return 0x03B7;  // η
  if (cp >= 0x1F28 && cp <= 0x1F2F) return 0x0397;  // Η
  if (cp >= 0x1F30 && cp <= 0x1F37) return 0x03B9;  // ι
  if (cp >= 0x1F38 && cp <= 0x1F3F) return 0x0399;  // Ι
  if (cp >= 0x1F40 && cp <= 0x1F45) return 0x03BF;  // ο
  if (cp >= 0x1F48 && cp <= 0x1F4D) return 0x039F;  // Ο
  if (cp >= 0x1F50 && cp <= 0x1F57) return 0x03C5;  // υ
  if (cp == 0x1F59 || cp == 0x1F5B || cp == 0x1F5D || cp == 0x1F5F) return 0x03A5;  // Υ
  if (cp >= 0x1F60 && cp <= 0x1F67) return 0x03C9;  // ω
  if (cp >= 0x1F68 && cp <= 0x1F6F) return 0x03A9;  // Ω
  if (cp == 0x1F70 || cp == 0x1F71) return 0x03B1;  // ὰ, ά -> α
  if (cp == 0x1F72 || cp == 0x1F73) return 0x03B5;  // ὲ, έ -> ε
  if (cp == 0x1F74 || cp == 0x1F75) return 0x03B7;  // ὴ, ή -> η
  if (cp == 0x1F76 || cp == 0x1F77) return 0x03B9;  // ὶ, ί -> ι
  if (cp == 0x1F78 || cp == 0x1F79) return 0x03BF;  // ὸ, ό -> ο
  if (cp == 0x1F7A || cp == 0x1F7B) return 0x03C5;  // ὺ, ύ -> υ
  if (cp == 0x1F7C || cp == 0x1F7D) return 0x03C9;  // ὼ, ώ -> ω
  if (cp >= 0x1F80 && cp <= 0x1F87) return 0x03B1;
  if (cp >= 0x1F88 && cp <= 0x1F8F) return 0x0391;
  if (cp >= 0x1F90 && cp <= 0x1F97) return 0x03B7;
  if (cp >= 0x1F98 && cp <= 0x1F9F) return 0x0397;
  if (cp >= 0x1FA0 && cp <= 0x1FA7) return 0x03C9;
  if (cp >= 0x1FA8 && cp <= 0x1FAF) return 0x03A9;
  if (cp >= 0x1FB0 && cp <= 0x1FB7) return 0x03B1;
  if (cp >= 0x1FB8 && cp <= 0x1FBC) return 0x0391;
  if (cp >= 0x1FC2 && cp <= 0x1FC7) return 0x03B7;
  if (cp >= 0x1FC8 && cp <= 0x1FC9) return 0x0395;
  if (cp >= 0x1FCA && cp <= 0x1FCC) return 0x0397;
  if (cp >= 0x1FD0 && cp <= 0x1FD7) return 0x03B9;
  if (cp >= 0x1FD8 && cp <= 0x1FDB) return 0x0399;
  if ((cp >= 0x1FE0 && cp <= 0x1FE3) || cp == 0x1FE6 || cp == 0x1FE7) return 0x03C5;
  if (cp == 0x1FE4 || cp == 0x1FE5) return 0x03C1;  // ῤ, ῥ -> ρ
  if (cp >= 0x1FE8 && cp <= 0x1FEB) return 0x03A5;
  if (cp == 0x1FEC) return 0x03A1;  // Ῥ -> Ρ
  if (cp >= 0x1FF2 && cp <= 0x1FF7) return 0x03C9;
  if (cp >= 0x1FF8 && cp <= 0x1FF9) return 0x039F;
  if (cp >= 0x1FFA && cp <= 0x1FFC) return 0x03A9;
  return cp;
}

}  // namespace

uint32_t fallbackGlyphCodepoint(const uint32_t cp) {
  if (cp >= 0x1F00 && cp <= 0x1FFE) {
    return polytonicGreekFallback(cp);
  }
  if (cp < 0x00E6 || cp > 0x25CF) return cp;

  size_t lo = 0;
  size_t hi = FALLBACK_COUNT;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (FALLBACKS[mid].from < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < FALLBACK_COUNT && FALLBACKS[lo].from == cp) return FALLBACKS[lo].to;
  return cp;
}
