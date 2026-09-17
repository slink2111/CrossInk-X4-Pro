#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;
class Page;

namespace EpubGrayscale {
constexpr int GRAYSCALE_STRIP_ROWS = 80;

using BaseDisplayFn = void (*)(void* context);

// Preserves the live BW buffer and existing controller synchronization. False
// leaves the caller responsible for its existing BW-snapshot fallback.
bool runTiledGrayscalePass(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                           bool foregroundBlack, bool needsTextGrayscale, bool needsImageGrayscale, uint8_t* scratch,
                           size_t scratchSize, bool asyncRefreshPending);

// Prerenders grayscale planes into memory buffers before calling baseDisplayFn,
// then immediately writes the grayscale planes to the display controller and
// activates grayscale. Eliminates visible two-pass flashes on screen.
bool runPrerenderedGrayscalePass(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                                 bool foregroundBlack, bool needsTextGrayscale, bool needsImageGrayscale,
                                 uint8_t* scratch, size_t scratchSize, BaseDisplayFn baseDisplayFn, void* context);
}  // namespace EpubGrayscale

