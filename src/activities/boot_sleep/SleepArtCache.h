#pragma once
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <string>

// Caches the display-ready sleep-art planes on SD so repeat sleeps with the
// same image skip the expensive source decode. The grayscale pipeline streams
// and re-decodes the source BMP once per plane (BW base + gray LSB + gray MSB
// — single-framebuffer constraint, and no RAM to hold the decoded image);
// measured on X3 that is ~2.8 s of a ~4.2 s sleep entry. A cache hit replays
// the finished planes with three 48 KB reads instead.
//
// The cache entry is a hidden sidecar next to its source image
// ("/dir/img.bmp" -> "/dir/.img.bmp.sart"), so every wallpaper in a random
// /.sleep rotation gets its own entry, deleting an image orphans only its own
// sidecar, and no eviction bookkeeping is needed. The dot prefix keeps
// sidecars out of the wallpaper scanner, which skips dotfiles.
//
// Entries are best-effort and self-invalidating: the header echoes the source
// size, a sampled content fingerprint (same-dimension BMPs have identical
// file sizes, so size alone cannot detect an edited image), the pipeline
// settings, and the panel geometry. Any mismatch is a miss and the entry is
// rewritten after the normal render.
class SleepArtCache {
 public:
  struct Key {
    std::string cachePath;  // hidden sidecar next to the source image
    uint32_t srcSize = 0;
    uint32_t srcFingerprint = 0;
  };

  // Build the key for an open source image. Reads a small sample block for
  // the fingerprint and restores the file position to 0, so Bitmap parsing
  // can proceed unaffected on a miss.
  static Key keyFor(HalFile& srcFile, const std::string& srcPath);

  // Blit + display the cached planes. Returns false without touching the
  // panel on any miss (no entry, stale source, settings/geometry mismatch).
  static bool tryRender(const GfxRenderer& renderer, const Key& key);

  // Persists planes as the render pipeline produces them: begin() once,
  // storePlane() with the framebuffer after each plane (BW, then optionally
  // gray LSB / MSB), commit() at the end. All failures silently deactivate
  // the writer and remove the partial entry — sleeping must never break on a
  // cache problem. The header is sealed (magic written) only in commit(), so
  // an interrupted write is rejected by tryRender().
  class Writer {
   public:
    void begin(const GfxRenderer& renderer, const Key& key, uint8_t planeCount);
    void storePlane(const GfxRenderer& renderer);
    void commit();

   private:
    void abandon();

    HalFile file;
    std::string cachePath;
    uint32_t srcSize = 0;
    uint32_t srcFingerprint = 0;
    uint16_t panelW = 0;
    uint16_t panelH = 0;
    uint8_t planeCount = 0;
    uint8_t planesStored = 0;
    bool active = false;
  };
};
