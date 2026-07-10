#include "SleepArtCache.h"

#include <Logging.h>

#include "CrossPointSettings.h"

namespace {
constexpr uint32_t SLEEP_ART_MAGIC = 0x53415254;  // "SART"
constexpr uint8_t SLEEP_ART_VERSION = 1;

// Field order avoids internal padding; read/written as a whole from an
// aligned stack object (no packed-member access, see RISC-V alignment rules).
struct SleepArtHeader {
  uint32_t magic;  // 0 until Writer::commit() seals the entry
  uint32_t srcSize;
  uint32_t srcFingerprint;
  uint16_t panelW;
  uint16_t panelH;
  uint8_t version;
  uint8_t planeCount;  // 1 = BW only, 3 = BW + gray LSB + gray MSB
  uint8_t coverMode;
  uint8_t coverFilter;
};
static_assert(sizeof(SleepArtHeader) == 20, "unexpected padding in cache header");
}  // namespace

SleepArtCache::Key SleepArtCache::keyFor(HalFile& srcFile, const std::string& srcPath) {
  Key key;
  key.srcSize = static_cast<uint32_t>(srcFile.fileSize());

  // Sample a block from the middle of the file: BMPs of identical dimensions
  // have identical sizes, so the content hash is what detects an edited image.
  uint8_t sample[256];
  uint32_t hash = 0;
  if (srcFile.seekSet(key.srcSize / 2)) {
    const int n = srcFile.read(sample, sizeof(sample));
    for (int i = 0; i < n; i++) {
      hash = hash * 31u + sample[i];
    }
  }
  srcFile.seekSet(0);
  key.srcFingerprint = hash;

  // "/dir/img.bmp" -> "/dir/.img.bmp.sart"
  const size_t slash = srcPath.find_last_of('/');
  const size_t nameStart = (slash == std::string::npos) ? 0 : slash + 1;
  key.cachePath = srcPath.substr(0, nameStart) + "." + srcPath.substr(nameStart) + ".sart";
  return key;
}

bool SleepArtCache::tryRender(const GfxRenderer& renderer, const Key& key) {
  HalFile file;
  if (!Storage.openFileForRead("SLC", key.cachePath, file)) {
    return false;
  }

  SleepArtHeader header{};
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    return false;
  }

  const size_t bufferSize = renderer.getBufferSize();
  const bool valid =
      header.magic == SLEEP_ART_MAGIC && header.version == SLEEP_ART_VERSION && header.srcSize == key.srcSize &&
      header.srcFingerprint == key.srcFingerprint && static_cast<int>(header.panelW) == renderer.getScreenWidth() &&
      static_cast<int>(header.panelH) == renderer.getScreenHeight() &&
      header.coverMode == SETTINGS.sleepScreenCoverMode && header.coverFilter == SETTINGS.sleepScreenCoverFilter &&
      (header.planeCount == 1 || header.planeCount == 3) &&
      file.fileSize() == sizeof(header) + static_cast<size_t>(header.planeCount) * bufferSize;
  if (!valid) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (file.read(frameBuffer, bufferSize) != static_cast<int>(bufferSize)) {
    // Nothing displayed yet: clean miss, the normal render path repaints.
    return false;
  }

  LOG_DBG("SLC", "Sleep art cache hit: %s (%u planes)", key.cachePath.c_str(), header.planeCount);

  if (header.planeCount == 1) {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return true;
  }

  // Mirror of the grayscale pipeline tail in renderBitmapSleepScreen, minus
  // the per-plane source decodes.
  renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);
  if (file.read(frameBuffer, bufferSize) == static_cast<int>(bufferSize)) {
    renderer.copyGrayscaleLsbBuffers();
    if (file.read(frameBuffer, bufferSize) == static_cast<int>(bufferSize)) {
      renderer.copyGrayscaleMsbBuffers();
      renderer.displayGrayBuffer();
    }
  }
  // An IO failure mid-gray still leaves the BW art displayed — count as a hit
  // (the panel is in a sane state; the gray nudge is polish).
  return true;
}

void SleepArtCache::Writer::begin(const GfxRenderer& renderer, const Key& key, const uint8_t newPlaneCount) {
  active = false;
  if (newPlaneCount != 1 && newPlaneCount != 3) {
    return;
  }
  if (!Storage.openFileForWrite("SLC", key.cachePath, file)) {
    return;
  }

  cachePath = key.cachePath;
  srcSize = key.srcSize;
  srcFingerprint = key.srcFingerprint;
  panelW = static_cast<uint16_t>(renderer.getScreenWidth());
  panelH = static_cast<uint16_t>(renderer.getScreenHeight());
  planeCount = newPlaneCount;
  planesStored = 0;

  // Placeholder header (magic 0): commit() seals it, so an interrupted write
  // is rejected as a miss instead of replaying half an image.
  SleepArtHeader header{};
  if (file.write(&header, sizeof(header)) != sizeof(header)) {
    abandon();
    return;
  }
  active = true;
}

void SleepArtCache::Writer::storePlane(const GfxRenderer& renderer) {
  if (!active || planesStored >= planeCount) {
    return;
  }
  const size_t bufferSize = renderer.getBufferSize();
  if (file.write(renderer.getFrameBuffer(), bufferSize) != bufferSize) {
    abandon();
    return;
  }
  planesStored++;
}

void SleepArtCache::Writer::commit() {
  if (!active) {
    return;
  }
  active = false;
  if (planesStored != planeCount) {
    abandon();
    return;
  }

  SleepArtHeader header{};
  header.magic = SLEEP_ART_MAGIC;
  header.version = SLEEP_ART_VERSION;
  header.srcSize = srcSize;
  header.srcFingerprint = srcFingerprint;
  header.panelW = panelW;
  header.panelH = panelH;
  header.planeCount = planeCount;
  header.coverMode = SETTINGS.sleepScreenCoverMode;
  header.coverFilter = SETTINGS.sleepScreenCoverFilter;
  if (!file.seekSet(0) || file.write(&header, sizeof(header)) != sizeof(header)) {
    abandon();
    return;
  }
  file.close();
  LOG_DBG("SLC", "Sleep art cached: %s (%u planes)", cachePath.c_str(), planeCount);
}

void SleepArtCache::Writer::abandon() {
  active = false;
  file.close();  // must close before remove (DESTRUCTOR_CLOSES_FILE contract)
  Storage.remove(cachePath.c_str());
}
