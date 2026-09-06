#pragma once
#include <string>

#include "SleepArtCache.h"
#include "activities/Activity.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;

 private:
  // Paints the "Going to sleep" popup (once) before a slow render path: a
  // cache-miss art render or cover generation. Fast paths skip it — their own
  // single refresh lands sooner than the popup used to, and the popup's
  // full-frame refresh would otherwise be ~27% of a cache-hit sleep entry.
  void paintGoingToSleepPopupOnce() const;
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  // cacheKey non-null persists the rendered planes for later cache-hit sleeps.
  // preserveBackground skips the initial clear so transparent overlays keep the
  // existing framebuffer content.
  void renderBitmapSleepScreen(const Bitmap& bitmap, const SleepArtCache::Key* cacheKey = nullptr,
                               bool preserveBackground = false) const;
  bool renderSleepOverlayFile(HalFile& file, const char* pathForLog) const;
  bool renderTransparentOverlayPng(const std::string& path) const;
  bool renderSleepOverlayPath(const std::string& path) const;
  void renderLastScreenSleepScreen() const;
  void renderTransparentCustomSleepScreen() const;
  void renderBlankSleepScreen() const;

  bool fromTimeout = false;
  mutable bool sleepPopupPainted = false;
};
