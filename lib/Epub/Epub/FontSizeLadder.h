#pragma once

#include <cstdint>

// FontSizeLadder — the body font's sibling sizes, used to snap an effective block
// font size to a REAL pre-rendered font instead of nearest-neighbor glyph scaling
// (concept from CidVonHighwind/microreader's BitmapFontSet).
struct FontSizeLadder {
  static constexpr int kMaxRungs = 9;

  struct Rung {
    int32_t fontId = 0;    // font-id
    uint16_t sizePct = 0;  // rung size as percent of the body font (body = 100)
  };

  Rung rungs[kMaxRungs] = {};
  int8_t count = 0;

  void addRung(const int32_t fontId, const uint16_t sizePct) {
    if (count >= kMaxRungs || fontId == 0 || sizePct == 0) return;
    rungs[count].fontId = fontId;
    rungs[count].sizePct = sizePct;
    ++count;
  }

  static constexpr float kResidualDeadZone = 0.03f;

  struct Resolved {
    int32_t fontId = 0;
    float residual = 1.0f;
  };

  Resolved resolve(const float desiredPct) const {
    Resolved r = resolveExact(desiredPct);
    if (r.residual > 1.0f - kResidualDeadZone && r.residual < 1.0f + kResidualDeadZone) {
      r.residual = 1.0f;
    }
    return r;
  }

 private:
  Resolved resolveExact(const float desiredPct) const {
    Resolved r;
    r.residual = desiredPct / 100.0f;
    if (count == 0 || desiredPct <= 0.0f) return r;

    int best = -1;
    float bestDiff = 0.0f;
    for (int i = 0; i < count; ++i) {
      const float diff =
          (rungs[i].sizePct > desiredPct) ? (rungs[i].sizePct - desiredPct) : (desiredPct - rungs[i].sizePct);
      if (best < 0 || diff < bestDiff) {
        best = i;
        bestDiff = diff;
      }
    }
    if (best < 0 || rungs[best].sizePct == 100) return r;

    r.fontId = rungs[best].fontId;
    r.residual = desiredPct / static_cast<float>(rungs[best].sizePct);
    return r;
  }
};
