#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <VectorFontSupport.h>

#if CROSSPOINT_VECTOR_FONTS
#include <TtfEpdFont.h>
#include <esp_heap_caps.h>
#include "ReaderFontSizes.h"
#endif

#include <cstdio>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace {

#if CROSSPOINT_VECTOR_FONTS
// Stable, non-zero renderer font id for a vector family at a size (FNV-1a of
// name + size). 0 is the "not found" sentinel, so bump collisions to 1.
int computeTtfFontId(const char* familyName, uint8_t pointSize) {
  uint32_t hash = 2166136261u;
  for (const char* p = familyName; p && *p; ++p) {
    hash ^= static_cast<uint8_t>(*p);
    hash *= 16777619u;
  }
  hash ^= pointSize;
  hash *= 16777619u;
  hash ^= 0x54544600u;  // "TTF\0" salt to avoid colliding with cpfont ids
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}
#endif  // CROSSPOINT_VECTOR_FONTS

struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};

constexpr UiFontSize kUiFontSizes[] = {
    {SMALL_FONT_ID, 8},
    {UI_10_FONT_ID, 10},
    {UI_12_FONT_ID, 12},
};

enum class FontFileSelection : uint8_t { Closest, Exact };

// This is a cold setup path, not a render loop. The 320-byte stack footprint
// (path and filename) replace a heap-allocated whole-font catalog
// during every dictionary swap, avoiding persistent fragmentation on the C3.
bool findInstalledFontFile(const char* familyName, const uint8_t targetPointSize, const FontFileSelection selection,
                           char* path, const size_t pathSize, uint8_t& selectedPointSize) {
  if (!familyName || familyName[0] == '\0' || !path || pathSize == 0) return false;

  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) return false;
  const int directoryLength = std::snprintf(path, pathSize, "%s/%s", root, familyName);
  if (directoryLength <= 0 || static_cast<size_t>(directoryLength) >= pathSize) return false;

  HalFile dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;

  uint8_t closestSize = 0;
  uint8_t closestDiff = UINT8_MAX;
  char filename[128] = {};
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0) continue;

    if (selection == FontFileSelection::Closest) {
      const uint8_t diff = pointSize > targetPointSize ? pointSize - targetPointSize : targetPointSize - pointSize;
      if (closestDiff == UINT8_MAX || diff < closestDiff || (diff == closestDiff && pointSize < closestSize)) {
        closestSize = pointSize;
        closestDiff = diff;
      }
    }
  }
  dir.close();

  if (selection == FontFileSelection::Closest) {
    selectedPointSize = closestSize;
  } else if (selection == FontFileSelection::Exact) {
    selectedPointSize = targetPointSize;
  }

  if (selectedPointSize == 0) return false;
  // Scan once more to preserve the exact file name rather than assuming the
  // file base name matches the directory name.
  dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0 ||
        pointSize != selectedPointSize) {
      continue;
    }
    const int pathLength = std::snprintf(path, pathSize, "%s/%s/%s", root, familyName, filename);
    dir.close();
    return pathLength > 0 && static_cast<size_t>(pathLength) < pathSize;
  }
  dir.close();
  return false;
}

}  // namespace

SdCardFontSystem::SdCardFontSystem() = default;
SdCardFontSystem::~SdCardFontSystem() = default;

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  if (SETTINGS.sdFontFamilyName[0] == '\0') {
    LOG_DBG("SDFS", "SD font resolver ready; discovery deferred until requested");
    return;
  }

  ensureLoaded(renderer);
  releaseRegistry();
}

void SdCardFontSystem::persistSettingsChange() const {
  if (settingsPersistenceCallback_) {
    settingsPersistenceCallback_(settingsPersistenceContext_);
  } else {
    SETTINGS.saveToFile();
  }
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  const bool registryWasDirty = registryDirty_.load(std::memory_order_acquire);
  const char* wantedFamily = SETTINGS.sdFontFamilyName;

#if CROSSPOINT_VECTOR_FONTS
  // Vector (.ttf/.otf) family selected: route through the FreeInkFont path and
  // drop any pre-rasterized (.cpfont) font that was loaded.
  if (wantedFamily[0] != '\0') {
    ensureRegistry();
    const auto* wantedFam = registry_.findFamily(wantedFamily);
    if (wantedFam && wantedFam->vector) {
      if (!manager_.currentFamilyName().empty()) {
        manager_.unloadAll(renderer);
        loadedFontPointSize_ = 0;
      }
      loadTtfFamily(*wantedFam, renderer, registryWasDirty);
      return;
    }
  }
  // Not on a vector family — ensure any previously-loaded TTF font is released
  // before the pre-rasterized/built-in path below takes over.
  if (!ttfFamily_.empty()) unloadTtf(renderer);
#endif

  const std::string& currentFamily = manager_.currentFamilyName();
  uint8_t targetPointSize = SETTINGS.getSdFontTargetPointSize();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
      loadedFontPointSize_ = 0;
    }
    return;
  }

  if (!registryWasDirty && currentFamily == wantedFamily && loadedFontPointSize_ == targetPointSize &&
      SETTINGS.legacySdFontSizeStep == UINT8_MAX) {
    return;
  }

  ensureRegistry();

  const auto* family = registry_.findFamily(wantedFamily);
  if (family && SETTINGS.legacySdFontSizeStep != UINT8_MAX) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) {
      const uint8_t step = std::min<uint8_t>(SETTINGS.legacySdFontSizeStep, sizes.size() - 1);
      targetPointSize = sizes[step];
      SETTINGS.readerFontPointSize = targetPointSize;
      SETTINGS.legacySdFontSizeStep = UINT8_MAX;
      persistSettingsChange();
      LOG_INF("SDFS", "Migrated SD font size to %u pt", targetPointSize);
    }
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      persistSettingsChange();
      return;
    }
    const auto* wantedFile = family->findClosestFile(targetPointSize);
    uint8_t wantedPt = wantedFile ? wantedFile->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (target %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            targetPointSize, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  if (family) {
    if (manager_.loadFamilyClosest(*family, renderer, targetPointSize)) {
      loadedFontPointSize_ = targetPointSize;
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
      persistSettingsChange();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
    persistSettingsChange();
  }
}

void SdCardFontSystem::releaseLoadedFont(GfxRenderer& renderer) {
#if CROSSPOINT_VECTOR_FONTS
  unloadTtf(renderer);
#endif
  if (manager_.currentFamilyName().empty()) return;

  const std::string familyName = manager_.currentFamilyName();
  (void)familyName;
  manager_.unloadAll(renderer);
  loadedFontPointSize_ = 0;
  LOG_DBG("SDFS", "Released SD card font before low-memory operation: %s", familyName.c_str());
}

void SdCardFontSystem::ensureRegistry() {
  const bool dirty = registryDirty_.exchange(false, std::memory_order_acq_rel);
  if (registryLoaded_ && !dirty) return;
  if (dirty) LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
  registry_.discover();
  if (registry_.lastDiscoveryFailed()) {
    LOG_ERR("SDFS", "SD font registry scan ran out of memory (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    registryDirty_.store(true, std::memory_order_release);
    return;
  }
  registryLoaded_ = true;
}

void SdCardFontSystem::releaseRegistry() {
  if (!registryLoaded_) return;
  LOG_DBG("SDFS", "Releasing SD font catalog (%d families)", registry_.getFamilyCount());
  registry_.clear();
  registryLoaded_ = false;
}

void SdCardFontSystem::releaseForNetwork(GfxRenderer& renderer) {
  releaseLoadedFont(renderer);
  releaseRegistry();
  registryDirty_.store(true, std::memory_order_release);
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(familyName));
  if (readerIt == renderer.getFontMap().end()) return;

  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  for (const auto& ui : kUiFontSizes) {
    const int sdFontId = manager_.loadFamilyExtraSize(*family, renderer, ui.pointSize);
    if (sdFontId != 0) {
      renderer.setFallbackFont(ui.fontId, sdFontId);
    } else {
      LOG_DBG("SDFS", "No %u pt SD glyphs for UI fallback in %s", ui.pointSize, familyName.c_str());
    }
  }
}

void SdCardFontSystem::setupUiFallbacksDirect(GfxRenderer& renderer, const char* familyName) {
  if (!familyName || familyName[0] == '\0') return;

  const auto readerIt = renderer.getFontMap().find(manager_.getFontId(manager_.currentFamilyName()));
  if (readerIt == renderer.getFontMap().end()) return;

  static constexpr uint32_t kCjkProbes[] = {0x4E00, 0x3042, 0x30A2, 0xAC00};
  bool hasCjk = false;
  for (const uint32_t cp : kCjkProbes) {
    if (readerIt->second.hasCodepoint(cp)) {
      hasCjk = true;
      break;
    }
  }
  if (!hasCjk) return;

  for (const auto& ui : kUiFontSizes) {
    char path[160] = {};
    uint8_t pointSize = 0;
    if (!findInstalledFontFile(familyName, ui.pointSize, FontFileSelection::Exact, path, sizeof(path), pointSize)) {
      continue;
    }
    const int sdFontId = manager_.loadFamilyExtraFile(path, familyName, pointSize, renderer);
    if (sdFontId != 0) renderer.setFallbackFont(ui.fontId, sdFontId);
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
#if CROSSPOINT_VECTOR_FONTS
  if (ttfDictFontId_ != 0 && familyName && ttfDictFamily_ == familyName) return ttfDictFontId_;
  if (ttfFontId_ != 0 && familyName && ttfFamily_ == familyName) return ttfFontId_;
#endif
  return manager_.getFontId(familyName);
}

bool SdCardFontSystem::changeReaderFontSize(const bool larger, const FontSizeStepMode mode) {
  refreshIfDirty();

  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
#if CROSSPOINT_VECTOR_FONTS
      if (family->vector) {
        return changeReaderFontSizeStep(VECTOR_READER_POINT_SIZES, std::size(VECTOR_READER_POINT_SIZES),
                                        SETTINGS.readerFontPointSize, larger, mode);
      }
#endif
      const auto sizes = family->availableSizes();
      if (changeReaderFontSizeStep(sizes.data(), sizes.size(), SETTINGS.readerFontPointSize, larger, mode)) return true;
      if (sizes.size() > 0) return false;
    }
  }

  return SETTINGS.changeReaderFontSize(larger, mode);
}

uint8_t SdCardFontSystem::resolveLegacySizeStep(const char* familyName, const uint8_t sizeStep) {
  ensureRegistry();
  const auto* family = familyName ? registry_.findFamily(familyName) : nullptr;
  if (family) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) return sizes[std::min<uint8_t>(sizeStep, sizes.size() - 1)];
  }
  return CrossPointSettings::getSdFontRangePointSize(SETTINGS.sdFontSizeRange, sizeStep);
}

DictionaryFontActivation SdCardFontSystem::activateDictionaryFont(GfxRenderer& renderer, const char* familyName,
                                                                  uint8_t targetPointSize) {
  if (!familyName || familyName[0] == '\0') {
    if (SETTINGS.dictionarySdFontFamilyName[0] != '\0') {
      familyName = SETTINGS.dictionarySdFontFamilyName;
    } else if (SETTINGS.sdFontFamilyName[0] != '\0') {
      familyName = SETTINGS.sdFontFamilyName;
    }
  }
  if (!familyName || familyName[0] == '\0') {
    return {restoreReaderFont(renderer), false};
  }

  ensureRegistry();
  const auto* family = registry_.findFamily(familyName);
  if (!family) {
    LOG_DBG("SDFS", "Dictionary font not found on card: %s", familyName);
    const char* globalFamilyName = SETTINGS.dictionarySdFontFamilyName;
    if (globalFamilyName[0] != '\0' && std::strcmp(familyName, globalFamilyName) != 0) {
      LOG_DBG("SDFS", "Using global dictionary font while per-book font is unavailable: %s", globalFamilyName);
      return activateDictionaryFont(renderer, globalFamilyName, SETTINGS.dictionaryFontPointSize);
    }
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_reader_fallback");
    return {readerFontId, false};
  }

#if CROSSPOINT_VECTOR_FONTS
  if (family->vector) {
    if (targetPointSize == 0) {
      targetPointSize = (ttf_ && ttfPointSize_ != 0) ? ttfPointSize_ : SETTINGS.readerFontPointSize;
      if (targetPointSize == 0) targetPointSize = 14;
    }
    targetPointSize = snapToNearestPointSize(VECTOR_READER_POINT_SIZES, std::size(VECTOR_READER_POINT_SIZES), targetPointSize);

    // Reuse reader's active TTF font if family and size match
    if (ttf_ && ttfFamily_ == family->name && ttfPointSize_ == targetPointSize) {
      if (ttfDict_) {
        renderer.unregisterTtfFont(ttfDictFontId_);
        renderer.removeFont(ttfDictFontId_);
        ttfDict_.reset();
        ttfDictFontId_ = 0;
        ttfDictFamily_.clear();
        ttfDictPointSize_ = 0;
      }
      LOG_DBG("SDFS", "Dictionary font reusing reader TTF font %s @ %upt (id %d)", family->name.c_str(), targetPointSize, ttfFontId_);
      return {ttfFontId_, true};
    }

    if (ttfDict_ && ttfDictFamily_ == family->name && ttfDictPointSize_ == targetPointSize) {
      return {ttfDictFontId_, true};
    }

    if (ttfDict_) {
      renderer.unregisterTtfFont(ttfDictFontId_);
      renderer.removeFont(ttfDictFontId_);
      ttfDict_.reset();
      ttfDictFontId_ = 0;
      ttfDictFamily_.clear();
      ttfDictPointSize_ = 0;
    }

    const bool havePsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
    const size_t cacheBytes = havePsram ? 512 * 1024 : 32 * 1024;
    const uint16_t maxGlyphs = havePsram ? 2048 : 512;

    ttfDict_ = makeUniqueNoThrow<TtfEpdFont>();
    if (!ttfDict_) {
      LOG_ERR("SDFS", "OOM: TtfEpdFont for dictionary");
      return {restoreReaderFont(renderer), false};
    }

    if (ttf_ && ttfFamily_ == family->name && ttfSources_[0].present) {
      addTtfSources(*ttfDict_);
    } else {
      for (const auto& file : family->files) {
        const uint8_t role = file.style < 4 ? file.style : 0;
        openTtfSource(role, file.path);
      }
      if (!ttfSources_[0].present) {
        LOG_ERR("SDFS", "Dictionary TTF: regular file failed to open: %s", family->name.c_str());
        ttfDict_.reset();
        return {restoreReaderFont(renderer), false};
      }
      addTtfSources(*ttfDict_);
    }

    if (!ttfDict_->load(targetPointSize, /*twoBit=*/true, cacheBytes, maxGlyphs)) {
      LOG_ERR("SDFS", "Failed to load dictionary TTF face @ %upt", targetPointSize);
      ttfDict_.reset();
      return {restoreReaderFont(renderer), false};
    }
    ttfDict_->build(" ");
    ttfDictFontId_ = computeTtfFontId((family->name + "\x01dict").c_str(), targetPointSize);
    renderer.insertFont(ttfDictFontId_, ttfDict_->family());
    renderer.registerTtfFont(ttfDictFontId_, ttfDict_.get());
    ttfDictFamily_ = family->name;
    ttfDictPointSize_ = targetPointSize;
    LOG_DBG("SDFS", "Activated dictionary TTF font %s @ %upt (id %d)", family->name.c_str(), targetPointSize, ttfDictFontId_);
    return {ttfDictFontId_, true};
  }
#endif

  MemoryBudget::logHeapShape("dict.font_before_activate");
  char path[160] = {};
  uint8_t selectedPointSize = 0;
  if (targetPointSize == 0) {
    targetPointSize = manager_.currentPointSize() != 0
                          ? manager_.currentPointSize()
                          : CrossPointSettings::getReaderFontPointSize(SETTINGS.getEffectiveReaderFontSize());
  }
  if (!findInstalledFontFile(familyName, targetPointSize, FontFileSelection::Closest, path, sizeof(path),
                             selectedPointSize)) {
    LOG_DBG("SDFS", "Dictionary font not found on card: %s", familyName);
    const char* globalFamilyName = SETTINGS.dictionarySdFontFamilyName;
    if (globalFamilyName[0] != '\0' && std::strcmp(familyName, globalFamilyName) != 0) {
      LOG_DBG("SDFS", "Using global dictionary font while per-book font is unavailable: %s", globalFamilyName);
      return activateDictionaryFont(renderer, globalFamilyName, SETTINGS.dictionaryFontPointSize);
    }
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_reader_fallback");
    return {readerFontId, false};
  }

  if (manager_.currentFamilyName() == familyName && manager_.currentPointSize() == selectedPointSize) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    MemoryBudget::logHeapShape("dict.font_reused_reader");
    return {fontId, true};
  }

  const int activeReaderFontId = SETTINGS.getReaderFontId();
  const auto beforeCacheRelease = MemoryBudget::snapshot();
  if (renderer.releaseSdCardFontForLowMemory(activeReaderFontId)) {
    const auto afterCacheRelease = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader SD-font caches before dictionary swap: free=%u->%u maxAlloc=%u->%u",
            beforeCacheRelease.freeHeap, afterCacheRelease.freeHeap, beforeCacheRelease.maxAllocHeap,
            afterCacheRelease.maxAllocHeap);
  }

  auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    const auto beforeReaderUnload = heap;
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    heap = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader font before dictionary swap retry: free=%u->%u maxAlloc=%u->%u",
            beforeReaderUnload.freeHeap, heap.freeHeap, beforeReaderUnload.maxAllocHeap, heap.maxAllocHeap);
  }
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    LOG_ERR("SDFS", "Low heap for dictionary font swap (%u free, %u max alloc, need %u/%u); using reader font",
            heap.freeHeap, heap.maxAllocHeap, MemoryBudget::DICTIONARY_SD_FONT_MIN_FREE,
            MemoryBudget::DICTIONARY_SD_FONT_MIN_MAX_ALLOC);
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_heap_fallback");
    return {readerFontId, false};
  }

  if (!manager_.currentFamilyName().empty()) {
    manager_.unloadAll(renderer);
  }
  loadedFontPointSize_ = 0;

  if (manager_.loadFamilyFile(path, familyName, selectedPointSize, renderer)) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    LOG_DBG("SDFS", "Activated dictionary font %s at %u pt", familyName, manager_.currentPointSize());
    MemoryBudget::logHeapShape("dict.font_after_activate");
    return {fontId, true};
  }

  LOG_ERR("SDFS", "Failed to load dictionary font %s; restoring reader font", familyName);
  const int readerFontId = restoreReaderFont(renderer);
  MemoryBudget::logHeapShape("dict.font_reader_fallback");
  return {readerFontId, false};
}

int SdCardFontSystem::restoreReaderFont(GfxRenderer& renderer) {
#if CROSSPOINT_VECTOR_FONTS
  if (ttfDict_) {
    renderer.unregisterTtfFont(ttfDictFontId_);
    renderer.removeFont(ttfDictFontId_);
    ttfDict_.reset();
    ttfDictFontId_ = 0;
    ttfDictFamily_.clear();
    ttfDictPointSize_ = 0;
  }
  if (ttf_ && ttfFontId_ != 0) {
    return ttfFontId_;
  }
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    ensureRegistry();
    const auto* fam = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (fam && fam->vector) {
      loadTtfFamily(*fam, renderer, false);
      if (ttfFontId_ != 0) return ttfFontId_;
    }
  }
#endif

  const char* familyName = SETTINGS.sdFontFamilyName;
  if (!familyName || familyName[0] == '\0') {
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return SETTINGS.getBuiltInReaderFontId();
  }

  char path[160] = {};
  uint8_t selectedPointSize = 0;
  if (!findInstalledFontFile(familyName, SETTINGS.getSdFontTargetPointSize(), FontFileSelection::Closest, path,
                             sizeof(path), selectedPointSize)) {
    LOG_ERR("SDFS", "Reader font unavailable while restoring: %s", familyName);
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return SETTINGS.getBuiltInReaderFontId();
  }

  if (manager_.currentFamilyName() != familyName || manager_.currentPointSize() != selectedPointSize) {
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    if (!manager_.loadFamilyFile(path, familyName, selectedPointSize, renderer)) {
      LOG_ERR("SDFS", "Failed to restore reader font: %s", familyName);
      MemoryBudget::logHeapShape("dict.font_after_restore");
      return SETTINGS.getBuiltInReaderFontId();
    }
    loadedFontPointSize_ = SETTINGS.getSdFontTargetPointSize();
    setupUiFallbacksDirect(renderer, familyName);
  }

  const int fontId = manager_.getFontId(manager_.currentFamilyName());
  MemoryBudget::logHeapShape("dict.font_after_restore");
  return fontId != 0 ? fontId : SETTINGS.getBuiltInReaderFontId();
}

#if CROSSPOINT_VECTOR_FONTS

void SdCardFontSystem::freeTtfSources() {
  for (auto& s : ttfSources_) {
    s.bytes.clear();
    freeink::font::PsramVector<uint8_t>().swap(s.bytes);
    if (s.file) s.file.close();
    s.streamed = false;
    s.size = 0;
    s.present = false;
  }
}

void SdCardFontSystem::unloadTtf(GfxRenderer& renderer) {
  if (ttfDict_) {
    renderer.unregisterTtfFont(ttfDictFontId_);
    renderer.removeFont(ttfDictFontId_);
    ttfDict_.reset();
    ttfDictFontId_ = 0;
    ttfDictFamily_.clear();
    ttfDictPointSize_ = 0;
  }
  if (ttfFamily_.empty() && ttfFontId_ == 0 && ttfUiIds_.empty()) return;
  for (const int id : ttfUiIds_) {
    renderer.unregisterTtfFont(id);
    renderer.removeFont(id);
  }
  ttfUiIds_.clear();
  ttfUi_.clear();
  renderer.clearFallbackFonts();
  if (ttfFontId_ != 0) {
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);
  }
  ttf_.reset();
  freeTtfSources();
  ttfFamily_.clear();
  ttfFontId_ = 0;
  ttfPointSize_ = 0;
}

bool SdCardFontSystem::openTtfSource(const uint8_t style, const std::string& path) {
  if (style >= 4) return false;
  static constexpr size_t kResidentMax = 6 * 1024 * 1024;
  static constexpr size_t kInternalHeadroom = 96 * 1024;
  HalFile f = Storage.open(path.c_str());
  if (!f) {
    LOG_ERR("SDFS", "Failed to open TTF: %s", path.c_str());
    return false;
  }
  const size_t len = f.size();
  if (len == 0) {
    LOG_ERR("SDFS", "Empty TTF: %s", path.c_str());
    f.close();
    return false;
  }
  TtfSource& s = ttfSources_[style];
  bool resident = len <= kResidentMax;
  if (resident && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < len) {
    const size_t internalFree = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (internalFree < len + kInternalHeadroom) {
      LOG_DBG("SDFS", "TTF %s (%u KB) too large for DRAM (largest block %u KB), streaming", path.c_str(),
              static_cast<unsigned>(len / 1024), static_cast<unsigned>(internalFree / 1024));
      resident = false;
    }
  }
  if (resident) {
    s.bytes.resize(len);
    const int got = f.read(s.bytes.data(), len);
    f.close();
    if (static_cast<size_t>(got) != len) {
      LOG_ERR("SDFS", "Short read on TTF %s (%d/%u)", path.c_str(), got, static_cast<unsigned>(len));
      s.bytes.clear();
      return false;
    }
    s.streamed = false;
  } else {
    s.file = std::move(f);
    s.streamed = true;
    static constexpr size_t kStreamPrefix = 1024 * 1024;
    const size_t prefix = len < kStreamPrefix ? len : kStreamPrefix;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > prefix + 256 * 1024) {
      s.bytes.resize(prefix);
      if (s.file.seek(0) && static_cast<size_t>(s.file.read(s.bytes.data(), prefix)) == prefix) {
        LOG_DBG("SDFS", "Cached %u KB TTF prefix in PSRAM", static_cast<unsigned>(prefix / 1024));
      } else {
        s.bytes.clear();
      }
    }
    LOG_DBG("SDFS", "Streaming TTF %s (%u KB) from SD", path.c_str(), static_cast<unsigned>(len / 1024));
  }
  s.size = static_cast<unsigned long>(len);
  s.present = true;
  return true;
}

unsigned long SdCardFontSystem::prefixRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                           const unsigned long count) {
  auto* s = static_cast<TtfSource*>(ctx);
  const unsigned long cached = s->bytes.size();
  if (offset < cached) {
    const unsigned long fromCache = (offset + count <= cached) ? count : cached - offset;
    if (count == 0) return 0;
    memcpy(buffer, s->bytes.data() + offset, fromCache);
    if (fromCache == count) return count;
    return fromCache +
           SdCardFontRegistry::halFileRead(&s->file, offset + fromCache, buffer + fromCache, count - fromCache);
  }
  return SdCardFontRegistry::halFileRead(&s->file, offset, buffer, count);
}

void SdCardFontSystem::addTtfSources(TtfEpdFont& font) {
  for (uint8_t st = 0; st < 4; ++st) {
    TtfSource& s = ttfSources_[st];
    if (!s.present) continue;
    if (s.streamed) {
      font.addStreamSource(st, &SdCardFontSystem::prefixRead, &s, s.size);
    } else {
      font.addResidentSource(st, s.bytes.data(), static_cast<uint32_t>(s.bytes.size()));
    }
  }
}

void SdCardFontSystem::setupTtfUiFallbacks(GfxRenderer& renderer) {
  if (ttfFamily_.empty()) return;
  static constexpr size_t kUiFallbackMinInternalHeap = 160 * 1024;
  for (const auto& ui : kUiFontSizes) {
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) == 0) {
      const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (internalFree < kUiFallbackMinInternalHeap) {
        LOG_DBG("SDFS", "Skipping TTF UI fallback @%upt (%u KB internal free)", ui.pointSize,
                static_cast<unsigned>(internalFree / 1024));
        continue;
      }
    }
    auto f = makeUniqueNoThrow<TtfEpdFont>();
    if (!f) {
      LOG_ERR("SDFS", "OOM: TtfEpdFont for UI fallback @%upt", ui.pointSize);
      continue;
    }
    addTtfSources(*f);
    const bool ok = f->load(ui.pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/16 * 1024, /*maxGlyphs=*/384);
    if (!ok) continue;
    LOG_DBG("SDFS", "TTF UI fallback @%upt loaded (heap free %u)", ui.pointSize, (unsigned)ESP.getFreeHeap());
    const int id = computeTtfFontId((ttfFamily_ + "\x01ui").c_str(), ui.pointSize);
    renderer.insertFont(id, f->family());
    renderer.registerTtfFont(id, f.get());
    renderer.setFallbackFont(ui.fontId, id);
    ttfUiIds_.push_back(id);
    ttfUi_.push_back(std::move(f));
  }
}

void SdCardFontSystem::loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                     const bool registryWasDirty) {
  SETTINGS.readerFontPointSize =
      snapToNearestPointSize(VECTOR_READER_POINT_SIZES, std::size(VECTOR_READER_POINT_SIZES), SETTINGS.readerFontPointSize);
  const uint8_t size = SETTINGS.readerFontPointSize;

  if (!registryWasDirty && ttf_ && ttfFamily_ == family.name && ttfPointSize_ == size) return;

  const bool havePsram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 0;
  const size_t cacheBytes = havePsram ? 1024 * 1024 : 32 * 1024;
  const uint16_t maxGlyphs = havePsram ? 4096 : 768;

  if (!registryWasDirty && ttf_ && ttfFamily_ == family.name) {
    renderer.unregisterTtfFont(ttfFontId_);
    renderer.removeFont(ttfFontId_);
    if (ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs)) {
      ttf_->build(" ");
      ttfFontId_ = computeTtfFontId(family.name.c_str(), size);
      renderer.insertFont(ttfFontId_, ttf_->family());
      renderer.registerTtfFont(ttfFontId_, ttf_.get());
      ttfPointSize_ = size;
      return;
    }
  }

  unloadTtf(renderer);

  if (family.files.empty()) {
    LOG_ERR("SDFS", "Vector family %s has no file", family.name.c_str());
    SETTINGS.sdFontFamilyName[0] = '\0';
    persistSettingsChange();
    return;
  }

  for (const auto& file : family.files) {
    const uint8_t role = file.style < 4 ? file.style : 0;
    if (ttfSources_[role].present) continue;
    openTtfSource(role, file.path);
  }
  if (!ttfSources_[0].present) {
    LOG_ERR("SDFS", "Vector family %s: regular file failed to open (keeping selection)", family.name.c_str());
    freeTtfSources();
    return;
  }

  ttf_ = makeUniqueNoThrow<TtfEpdFont>();
  if (!ttf_) {
    LOG_ERR("SDFS", "OOM: TtfEpdFont for %s", family.name.c_str());
    freeTtfSources();
    return;
  }
  addTtfSources(*ttf_);
  const bool ok = ttf_->load(size, /*twoBit=*/true, cacheBytes, maxGlyphs);
  if (!ok) {
    LOG_ERR("SDFS", "FreeInkFont could not parse %s (keeping selection)", family.name.c_str());
    ttf_.reset();
    freeTtfSources();
    return;
  }
  ttf_->build(" ");

  ttfFontId_ = computeTtfFontId(family.name.c_str(), size);
  renderer.insertFont(ttfFontId_, ttf_->family());
  renderer.registerTtfFont(ttfFontId_, ttf_.get());
  ttfFamily_ = family.name;
  ttfPointSize_ = size;
  LOG_DBG("SDFS", "Reader TTF face loaded (heap free %u, max block %u)", (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
  setupTtfUiFallbacks(renderer);
  LOG_DBG("SDFS", "Loaded TTF font: %s @ %upt (id %d)", family.name.c_str(), size, ttfFontId_);
}

#endif  // CROSSPOINT_VECTOR_FONTS
