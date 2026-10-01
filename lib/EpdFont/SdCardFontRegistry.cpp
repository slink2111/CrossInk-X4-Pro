#include "SdCardFontRegistry.h"

#if CROSSPOINT_VECTOR_FONTS
#include <FtFont.h>
#endif
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>  // strcasecmp, strncasecmp

#include <algorithm>
#include <cstdlib>
#include <cstring>

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  if (vector) {
    for (const auto& f : files) {
      if (f.style == style) return &f;
    }
    return files.empty() ? nullptr : &files[0];
  }
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
}

const SdCardFontFileInfo* SdCardFontFamilyInfo::findNearestSize(const uint8_t pointSize, const uint8_t style) const {
  if (vector) {
    for (const auto& f : files) {
      if (f.style == style) return &f;
    }
    return files.empty() ? nullptr : &files[0];
  }
  const SdCardFontFileInfo* best = nullptr;
  uint8_t bestDelta = 255;
  for (const auto& f : files) {
    if (f.style != style) continue;
    const uint8_t delta = f.pointSize > pointSize ? f.pointSize - pointSize : pointSize - f.pointSize;
    if (!best || delta < bestDelta || (delta == bestDelta && f.pointSize < best->pointSize)) {
      best = &f;
      bestDelta = delta;
    }
  }
  return best;
}

bool SdCardFontFamilyInfo::hasSize(uint8_t size) const {
  if (vector) {
    return size >= 8 && size <= 22;
  }
  for (const auto& f : files) {
    if (f.pointSize == size) return true;
  }
  return false;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  if (vector) {
    return {8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
  }
  std::vector<uint8_t> sizes;
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  // Use an ends-with check rather than strstr() so that in-progress downloads
  // like "Foo_14.cpfont.tmp" or backups like "Foo_14.cpfont~" aren't accepted.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;
  const char* ext = filename + nameLen - kExtLen;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  style = 0;
  return true;
}

#if CROSSPOINT_VECTOR_FONTS

bool SdCardFontRegistry::parseVectorFontName(const char* filename, size_t& baseLen) {
  static constexpr const char* kExts[] = {".ttf", ".otf", ".ttc"};
  const size_t nameLen = strlen(filename);
  for (const char* ext : kExts) {
    const size_t extLen = strlen(ext);
    if (nameLen <= extLen) continue;
    const char* tail = filename + nameLen - extLen;
    if (strcasecmp(tail, ext) == 0) {
      baseLen = nameLen - extLen;
      return baseLen > 0 && baseLen <= 127;
    }
  }
  return false;
}

uint8_t SdCardFontRegistry::parseVectorStyle(const char* baseName, size_t baseLen) {
  // Case-insensitive token scan. "bold" (incl. semibold/demibold) → bold bit;
  // "italic"/"oblique" → italic bit. Anything else is regular.
  bool bold = false;
  bool ital = false;
  const size_t n = baseLen;
  for (size_t i = 0; i < n; ++i) {
    if ((n - i) >= 4 && strncasecmp(baseName + i, "bold", 4) == 0) bold = true;
    if ((n - i) >= 6 && strncasecmp(baseName + i, "italic", 6) == 0) ital = true;
    if ((n - i) >= 7 && strncasecmp(baseName + i, "oblique", 7) == 0) ital = true;
  }
  return static_cast<uint8_t>((bold ? 1 : 0) | (ital ? 2 : 0));
}

// FtFont::ReadFn over a HalFile (absolute-offset reads; count 0 is a seek probe).
unsigned long SdCardFontRegistry::halFileRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                              const unsigned long count) {
  auto* f = static_cast<HalFile*>(ctx);
  if (f == nullptr || !*f) return 0;
  if (!f->seek(static_cast<size_t>(offset))) return 0;
  if (count == 0) return 0;
  const int n = f->read(buffer, count);
  return n < 0 ? 0 : static_cast<unsigned long>(n);
}

void SdCardFontRegistry::refineVectorStyles(const char* dirPath, std::vector<SdCardFontFileInfo>& files) {
  using freeink::font::FtFont;
  struct Candidate {
    size_t index;  // into files
    uint16_t weight;
    bool italic;
  };
  std::vector<Candidate> cands;
  cands.reserve(files.size());
  for (size_t i = 0; i < files.size(); ++i) {
    Candidate c{i, static_cast<uint16_t>((files[i].style & 1) ? 700 : 400), (files[i].style & 2) != 0};
    HalFile f = Storage.open(files[i].path.c_str());
    if (f && !f.isDirectory()) {
      FtFont::FaceInfo face;
      if (FtFont::inspectStream(&halFileRead, &f, static_cast<unsigned long>(f.size()), face) ==
          FtFont::InspectResult::Ok) {
        c.weight = face.weight;
        c.italic = face.italic;
      }
    }
    cands.push_back(c);
  }

  const auto pick = [&](const bool italic, const int target, const Candidate* exclude) -> const Candidate* {
    const Candidate* best = nullptr;
    for (const auto& c : cands) {
      if (c.italic != italic || &c == exclude) continue;
      if (!best) {
        best = &c;
        continue;
      }
      const int dc = std::abs(static_cast<int>(c.weight) - target);
      const int db = std::abs(static_cast<int>(best->weight) - target);
      if (dc < db || (dc == db && (c.weight < best->weight ||
                                   (c.weight == best->weight && files[c.index].path < files[best->index].path)))) {
        best = &c;
      }
    }
    return best;
  };

  const Candidate* regular = pick(false, 400, nullptr);
  if (!regular) {
    regular = pick(true, 400, nullptr);
    if (regular) LOG_DBG("SDREG", "No upright face in %s — promoting %s", dirPath, files[regular->index].path.c_str());
    if (!regular) return;  // no usable files at all
  }
  const Candidate* bold = pick(false, 700, regular);
  if (bold && bold->weight <= regular->weight) bold = nullptr;
  const Candidate* italic = regular->italic ? nullptr : pick(true, 400, nullptr);
  const Candidate* boldItalic = pick(true, 700, italic ? italic : regular);
  if (boldItalic && italic && boldItalic->weight <= italic->weight) boldItalic = nullptr;
  if (boldItalic && !boldItalic->italic) boldItalic = nullptr;

  std::vector<SdCardFontFileInfo> selected;
  selected.reserve(4);
  const auto add = [&](const Candidate* c, const uint8_t role) {
    if (!c) return;
    SdCardFontFileInfo info = files[c->index];
    info.style = role;
    selected.push_back(std::move(info));
  };
  add(regular, 0);
  add(bold, 1);
  add(italic, 2);
  add(boldItalic, 3);
  if (selected.size() < files.size()) {
    LOG_DBG("SDREG", "%s: %u of %u faces selected by weight", dirPath, static_cast<unsigned>(selected.size()),
            static_cast<unsigned>(files.size()));
  }
  files = std::move(selected);
}

#endif  // CROSSPOINT_VECTOR_FONTS

bool SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir.allocationFailed()) LOG_ERR("SDREG", "Out of memory opening font directory: %s", dirPath);
    return !dir.allocationFailed();
  }

  std::vector<SdCardFontFileInfo> cpfontFiles;
  std::vector<SdCardFontFileInfo> vectorFiles;

  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();

    // Skip macOS resource fork files (._*) and other hidden files
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

    uint8_t size, style;
    if (parseFilename(nameBuffer, size, style)) {
      bool duplicate = false;
      for (const auto& existing : cpfontFiles) {
        if (existing.pointSize == size && existing.style == style) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        LOG_ERR("SDREG", "Duplicate font %s in %s — skipping", nameBuffer, dirPath);
        continue;
      }
      SdCardFontFileInfo info;
      info.path = std::string(dirPath) + "/" + nameBuffer;
      info.pointSize = size;
      info.style = style;
      cpfontFiles.push_back(std::move(info));
      continue;
    }

#if CROSSPOINT_VECTOR_FONTS
    size_t baseLen = 0;
    if (parseVectorFontName(nameBuffer, baseLen)) {
      SdCardFontFileInfo info;
      info.path = std::string(dirPath) + "/" + nameBuffer;
      info.pointSize = 0;  // size-free
      info.style = parseVectorStyle(nameBuffer, baseLen);
      vectorFiles.push_back(std::move(info));
    }
#endif
  }

  if (dir.allocationFailed()) {
    LOG_ERR("SDREG", "Out of memory scanning font directory: %s", dirPath);
    return false;
  }

  if (!cpfontFiles.empty()) {
    family.vector = false;
    family.files = std::move(cpfontFiles);
  }
#if CROSSPOINT_VECTOR_FONTS
  else if (!vectorFiles.empty()) {
    refineVectorStyles(dirPath, vectorFiles);
    family.vector = true;
    family.files = std::move(vectorFiles);
  }
#endif

  return true;
}

// Scan a single root (e.g. "/.fonts") and append its families to `out`.
// Skips families whose names already exist in `out` (de-duplicates between
// the hidden and visible roots — first scan wins).
bool SdCardFontRegistry::scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out) {
  HalFile root = Storage.open(rootPath);
  if (!root) {
    if (root.allocationFailed()) {
      LOG_ERR("SDREG", "Out of memory opening font root: %s", rootPath);
      return false;
    }
    LOG_DBG("SDREG", "Fonts directory not found: %s", rootPath);
    return true;
  }
  if (!root.isDirectory()) {
    LOG_ERR("SDREG", "Fonts path is not a directory: %s", rootPath);
    return true;
  }

  char nameBuffer[128];
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();

      // Skip hidden/system directories inside the root (macOS ._*, .Trashes, etc.)
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

      // De-dup by family name across roots.
      bool exists = false;
      for (const auto& fam : out) {
        if (fam.name == nameBuffer) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      SdCardFontFamilyInfo family;
      family.name = nameBuffer;
      std::string subDirPath = std::string(rootPath) + "/" + nameBuffer;
      if (!SdCardFontRegistry::scanDirectory(subDirPath.c_str(), family)) return false;

      if (!family.files.empty()) {
        out.push_back(std::move(family));
        LOG_DBG("SDREG", "Found family: %s (%d files) in %s", out.back().name.c_str(),
                static_cast<int>(out.back().files.size()), rootPath);
      }
    } else {
#if CROSSPOINT_VECTOR_FONTS
      // Loose TrueType/OpenType file directly under the root (e.g.
      // /fonts/Bookerly.ttf). Rendered at any size via the FreeInkFont engine.
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
      size_t baseLen = 0;
      if (!parseVectorFontName(nameBuffer, baseLen)) continue;

      std::string famName(nameBuffer, baseLen);  // filename without extension
      bool exists = false;
      for (const auto& fam : out) {
        if (fam.name == famName) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      SdCardFontFamilyInfo family;
      family.name = famName;
      family.vector = true;
      SdCardFontFileInfo info;
      info.path = std::string(rootPath) + "/" + nameBuffer;
      info.pointSize = 0;  // size-free
      info.style = 0;
      family.files.push_back(std::move(info));
      out.push_back(std::move(family));
      LOG_DBG("SDREG", "Found vector font: %s in %s", famName.c_str(), rootPath);
#else
      entry.close();
#endif  // CROSSPOINT_VECTOR_FONTS
    }
  }

  if (root.allocationFailed()) {
    LOG_ERR("SDREG", "Out of memory scanning font root: %s", rootPath);
    return false;
  }
  return true;
}

bool SdCardFontRegistry::discover() {
  discoveryFailed_ = false;
  families_.clear();
  families_.reserve(16);

  // Hidden root is scanned first so it wins on name collisions, matching the
  // sleep-folder pattern (/.sleep preferred over /sleep).
  char hiddenRoot[16];
  char visibleRoot[16];
  const char* hiddenPath = FsHelpers::resolveRootDirectoryIgnoreCase(FONTS_DIR_HIDDEN, hiddenRoot, sizeof(hiddenRoot))
                               ? hiddenRoot
                               : FONTS_DIR_HIDDEN;
  const char* visiblePath =
      FsHelpers::resolveRootDirectoryIgnoreCase(FONTS_DIR_VISIBLE, visibleRoot, sizeof(visibleRoot))
          ? visibleRoot
          : FONTS_DIR_VISIBLE;
  if (!scanRoot(hiddenPath, families_) || !scanRoot(visiblePath, families_)) {
    discoveryFailed_ = true;
    clear();
    LOG_ERR("SDREG", "Font discovery stopped after an out-of-memory directory scan");
    return false;
  }

  // Sort families alphabetically
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });

  // Cap at MAX_SD_FAMILIES
  if (static_cast<int>(families_.size()) > MAX_SD_FAMILIES) {
    families_.resize(MAX_SD_FAMILIES);
  }

  LOG_DBG("SDREG", "Discovery complete: %d families", static_cast<int>(families_.size()));
  return !families_.empty();
}

void SdCardFontRegistry::clear() { std::vector<SdCardFontFamilyInfo>().swap(families_); }

const char* SdCardFontRegistry::findFamilyRoot(const char* familyName) {
  if (!familyName || !*familyName) return nullptr;
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_HIDDEN, familyName);
  if (Storage.exists(path)) return FONTS_DIR_HIDDEN;
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_VISIBLE, familyName);
  if (Storage.exists(path)) return FONTS_DIR_VISIBLE;
  return nullptr;
}

const char* SdCardFontRegistry::defaultWriteRoot() {
  bool hiddenExists = Storage.exists(FONTS_DIR_HIDDEN);
  bool visibleExists = Storage.exists(FONTS_DIR_VISIBLE);
  if (hiddenExists) return FONTS_DIR_HIDDEN;
  if (visibleExists) return FONTS_DIR_VISIBLE;
  return FONTS_DIR_HIDDEN;
}

const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& f : families_) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

int SdCardFontRegistry::getFamilyIndex(const std::string& name) const {
  for (int i = 0; i < static_cast<int>(families_.size()); i++) {
    if (families_[i].name == name) return i;
  }
  return -1;
}
