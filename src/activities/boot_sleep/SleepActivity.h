#pragma once
#include "SleepArtCache.h"
#include "activities/Activity.h"

class Bitmap;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool fromTimeout = false)
      : Activity("Sleep", renderer, mappedInput), fromTimeout(fromTimeout) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  // cacheKey non-null persists the rendered planes for later cache-hit sleeps.
  void renderBitmapSleepScreen(const Bitmap& bitmap, const SleepArtCache::Key* cacheKey = nullptr) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;

  bool fromTimeout = false;
};
