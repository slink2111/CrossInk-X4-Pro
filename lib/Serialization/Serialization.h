#pragma once
#include <HalStorage.h>

#include <iostream>
#include <limits>

namespace serialization {
template <typename T>
static void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
static void writePod(FsFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
static bool tryWritePod(FsFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

template <typename T>
static void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
static void readPod(FsFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

template <typename T>
static bool tryReadPod(FsFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

static void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

static bool tryWriteString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
  return tryWritePod(file, len) && (len == 0 || file.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len);
}

constexpr size_t COPY_CHUNK_BYTES = 512;
[[maybe_unused]] static bool copyBytes(FsFile& in, FsFile& out, uint32_t bytes) {
  uint8_t chunk[COPY_CHUNK_BYTES];
  while (bytes > 0) {
    const size_t want = bytes < COPY_CHUNK_BYTES ? static_cast<size_t>(bytes) : COPY_CHUNK_BYTES;
    if (in.read(chunk, want) != static_cast<int>(want)) return false;
    if (out.write(chunk, want) != want) return false;
    bytes -= static_cast<uint32_t>(want);
  }
  return true;
}

constexpr uint32_t MAX_STRING_LENGTH = 4096;

[[maybe_unused]] static bool readString(std::istream& is, std::string& s) {
  uint32_t len = 0;
  readPod(is, len);
  if (!is) {
    s.clear();
    return false;
  }
  if (len > MAX_STRING_LENGTH) {
    is.seekg(len, std::ios::cur);  // skip payload to keep stream aligned
    s.clear();
    return false;
  }
  s.resize(len);
  is.read(&s[0], len);
  return static_cast<bool>(is);
}

[[maybe_unused]] static bool readString(FsFile& file, std::string& s) {
  uint32_t len = 0;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != sizeof(len)) {
    s.clear();
    return false;
  }
  if (len > MAX_STRING_LENGTH) {
    file.seekCur(static_cast<int64_t>(len));  // skip payload to keep file position aligned
    s.clear();
    return false;
  }
  s.resize(len);
  const int readLen = static_cast<int>(len);
  if (readLen > 0 && file.read(reinterpret_cast<uint8_t*>(&s[0]), readLen) != readLen) {
    s.clear();
    return false;
  }
  return true;
}

static bool tryReadString(FsFile& file, std::string& s) {
  uint32_t len = 0;
  if (!tryReadPod(file, len)) {
    s.clear();
    return false;
  }
  if (len > MAX_STRING_LENGTH || static_cast<size_t>(len) > s.max_size() ||
      len > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    s.clear();
    return false;
  }
  s.resize(len);
  const int readLen = static_cast<int>(len);
  if (readLen > 0 && file.read(reinterpret_cast<uint8_t*>(&s[0]), readLen) != readLen) {
    s.clear();
    return false;
  }
  return true;
}
}  // namespace serialization
