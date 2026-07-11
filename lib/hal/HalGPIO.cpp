#include <BoardConfig.h>
#include <HalGPIO.h>
#include <Logging.h>
#include <PowerManager.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <XteinkDetect.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

// Global HalGPIO instance
HalGPIO gpio;

namespace X3GPIO {

bool readI2CReg16LE(uint8_t addr, uint8_t reg, uint16_t* outValue) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(addr, static_cast<uint8_t>(2), static_cast<uint8_t>(true)) < 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  const uint8_t lo = Wire.read();
  const uint8_t hi = Wire.read();
  *outValue = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

bool readBQ27220CurrentMA(int16_t* outCurrent) {
  uint16_t raw = 0;
  if (!readI2CReg16LE(I2C_ADDR_BQ27220, BQ27220_CUR_REG, &raw)) {
    return false;
  }
  *outCurrent = static_cast<int16_t>(raw);
  return true;
}

}  // namespace X3GPIO

namespace {
constexpr char HW_NAMESPACE[] = "cphw";
constexpr char NVS_KEY_DEV_OVERRIDE[] = "dev_ovr";  // 0=auto, 1=x4, 2=x3
constexpr char NVS_KEY_DEV_CACHED[] = "dev_det";    // 0=unknown, 1=x4, 2=x3

enum class NvsDeviceValue : uint8_t { Unknown = 0, X4 = 1, X3 = 2 };

NvsDeviceValue readNvsDeviceValue(const char* key, NvsDeviceValue defaultValue) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, true)) {
    return defaultValue;
  }
  const uint8_t raw = prefs.getUChar(key, static_cast<uint8_t>(defaultValue));
  prefs.end();
  if (raw > static_cast<uint8_t>(NvsDeviceValue::X3)) {
    return defaultValue;
  }
  return static_cast<NvsDeviceValue>(raw);
}

void writeNvsDeviceValue(const char* key, NvsDeviceValue value) {
  Preferences prefs;
  if (!prefs.begin(HW_NAMESPACE, false)) {
    return;
  }
  prefs.putUChar(key, static_cast<uint8_t>(value));
  prefs.end();
}

HalGPIO::DeviceType nvsToDeviceType(NvsDeviceValue value) {
  return value == NvsDeviceValue::X3 ? HalGPIO::DeviceType::X3 : HalGPIO::DeviceType::X4;
}

HalGPIO::DeviceType detectDeviceTypeWithFingerprint() {
  // Explicit override for recovery/support:
  // 0 = auto, 1 = force X4, 2 = force X3
  const NvsDeviceValue overrideValue = readNvsDeviceValue(NVS_KEY_DEV_OVERRIDE, NvsDeviceValue::Unknown);
  if (overrideValue == NvsDeviceValue::X3 || overrideValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Device override active: %s", overrideValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(overrideValue);
  }

  const NvsDeviceValue cachedValue = readNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::Unknown);
  if (cachedValue == NvsDeviceValue::X3 || cachedValue == NvsDeviceValue::X4) {
    LOG_INF("HW", "Using cached device type: %s", cachedValue == NvsDeviceValue::X3 ? "X3" : "X4");
    return nvsToDeviceType(cachedValue);
  }

  // No cache yet: use FreeInk's canonical two-pass X3 fingerprint and persist
  // only confirmed results. Inconclusive probes deliberately remain uncached.
  uint8_t score1 = 0;
  uint8_t score2 = 0;
  const freeink::XteinkVerdict verdict = freeink::detectXteinkVerdict(&score1, &score2);
  LOG_INF("HW", "Xteink probe scores: pass1=%u pass2=%u verdict=%u", score1, score2, static_cast<unsigned>(verdict));

  if (verdict == freeink::XteinkVerdict::X3Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X3);
    return HalGPIO::DeviceType::X3;
  }

  if (verdict == freeink::XteinkVerdict::X4Confirmed) {
    writeNvsDeviceValue(NVS_KEY_DEV_CACHED, NvsDeviceValue::X4);
    return HalGPIO::DeviceType::X4;
  }

  // Conservative fallback for first boot with inconclusive probes.
  return HalGPIO::DeviceType::X4;
}

}  // namespace

void HalGPIO::begin() {
#if FREEINK_MCU_C3
  _deviceType = detectDeviceTypeWithFingerprint();
  BoardConfig::selectDevice(deviceIsX3() ? BoardConfig::Board::XteinkX3 : BoardConfig::Board::XteinkX4);

  // Resolve the per-batch controller before SPI owns the display pins. FreeInk
  // checks the OEM hw_calib/screenType value first, then falls back to its
  // two-pass display-bus probe. X3's facade keys panel selection off the sibling
  // board profile, so preserve a detected UC8279 through setDisplayX3().
  freeink::applyXteinkDisplayController();
  if (deviceIsX3() && BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8279) {
    BoardConfig::selectDevice(BoardConfig::Board::XteinkX3Uc8279);
  }

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);

  if (deviceIsX4()) {
    pinMode(BAT_GPIO0, INPUT);
    pinMode(UART0_RXD, INPUT);
  }
#else
  _deviceType = DeviceType::X4;
#endif
  inputMgr.begin();
}

void HalGPIO::update() {
  inputMgr.update();
  const bool connected = isUsbConnected();
  usbStateChanged = (connected != lastUsbConnected);
  lastUsbConnected = connected;
}

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const { return inputMgr.isPressed(buttonIndex); }

bool HalGPIO::wasPressed(uint8_t buttonIndex) const { return inputMgr.wasPressed(buttonIndex); }

bool HalGPIO::wasAnyPressed() const { return inputMgr.wasAnyPressed(); }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const { return inputMgr.wasReleased(buttonIndex); }

bool HalGPIO::wasAnyReleased() const { return inputMgr.wasAnyReleased(); }

unsigned long HalGPIO::getHeldTime() const { return inputMgr.getHeldTime(); }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return inputMgr.getPowerButtonHeldTime(); }

bool HalGPIO::hasTouch() const { return inputMgr.hasTouch(); }

bool HalGPIO::wasTouchTap(float& nx, float& ny) const { return inputMgr.wasTouchTap(nx, ny); }

bool HalGPIO::wasTouchDown(float& nx, float& ny) const { return inputMgr.wasTouchPressedAt(nx, ny); }

bool HalGPIO::isTouchTapCandidate(float& nx, float& ny, unsigned long& heldMs) const {
  return inputMgr.isTouchTapCandidate(nx, ny, heldMs);
}

bool HalGPIO::isTouchHeldAt(float& nx, float& ny) const { return inputMgr.isTouchHeldAt(nx, ny); }

unsigned long HalGPIO::lastTouchHeldMs() const { return inputMgr.lastTouchHeldMs(); }

bool HalGPIO::wasSwipe(float& nxStart, float& nyStart, float& nxEnd, float& nyEnd) const {
  return inputMgr.wasSwipe(nxStart, nyStart, nxEnd, nyEnd);
}

bool HalGPIO::wasTouchActivity() const { return inputMgr.wasTouchActivity(); }

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(const bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}

bool HalGPIO::isXteinkDevice() const {
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279 ||
         BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4;
}

bool HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  // Boards without a power button (or M5Paper's latch circuit) cannot verify a
  // hold; treat the wake as valid.
  if (BoardConfig::ACTIVE.input.power < 0) {
    return true;
  }
#if defined(FREEINK_DEVICE_M5PAPER) && FREEINK_DEVICE_M5PAPER
  return true;
#endif
  if (shortPressAllowed) {
    // Fast path - no duration check needed
    return true;
  }
  // TODO: Intermittent edge case remains: a single tap followed by another single tap
  // can still power on the device. Tighten wake debounce/state handling here.

  // Calibrate: subtract boot time already elapsed, assuming button held since boot.
  const unsigned long calibration = millis();
  const unsigned long calibratedDuration = (calibration < requiredDurationMs) ? (requiredDurationMs - calibration) : 1;

  const auto start = millis();
  inputMgr.update();
  // inputMgr.isPressed() may take up to ~500ms to return correct state
  while (!inputMgr.isPressed(BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputMgr.update();
  }
  if (inputMgr.isPressed(BTN_POWER)) {
    do {
      delay(10);
      inputMgr.update();
    } while (inputMgr.isPressed(BTN_POWER) && inputMgr.getPowerButtonHeldTime() < calibratedDuration);
    if (inputMgr.getPowerButtonHeldTime() < calibratedDuration) {
      return false;
    }
  } else {
    return false;
  }
  return true;
}

// Compile-time kill switch for the power-wake latch. A control build with
// -DPOWER_WAKE_LATCH=0 never installs the GPIO ISR service at all — the
// attribution experiment for the slice-sleep wedge (see
// docs/slice-wedge-findings.md, candidate #1). Costs boot-cancel and the
// sleep-entry wake latch; never ship with 0.
#ifndef POWER_WAKE_LATCH
#define POWER_WAKE_LATCH 1
#endif

namespace {
// Power-wake latch state. Written by the ISR, read by the task after
// detaching; single 32-bit-aligned loads/stores are atomic on this core.
// Non-const statics live in DRAM, as IRAM_ATTR code requires.
volatile uint32_t powerLatchPressMs = 0;           // millis() of the last pressed edge
volatile uint32_t powerLatchLongestContactMs = 0;  // longest completed press since arming
int8_t powerLatchPin = -1;
bool powerLatchActiveHigh = false;

constexpr uint32_t POWER_LATCH_MIN_CONTACT_MS = 30;

void IRAM_ATTR powerWakeLatchIsr(void*) {
  const bool pressed = (gpio_get_level(static_cast<gpio_num_t>(powerLatchPin)) != 0) == powerLatchActiveHigh;
  const uint32_t now = millis();
  if (pressed) {
    powerLatchPressMs = now;
  } else if (powerLatchPressMs != 0) {
    // A full press->release. Requiring the pressed edge to have been seen by
    // THIS ISR ignores the release of the gesture that opened the latch window
    // (its pressed edge predates the arm), and the contact-time floor ignores
    // its release bounce. The longest contact is kept (not the last) so a
    // qualifying hold isn't shadowed by trailing bounce blips.
    const uint32_t contact = now - powerLatchPressMs;
    if (contact >= POWER_LATCH_MIN_CONTACT_MS && contact > powerLatchLongestContactMs) {
      powerLatchLongestContactMs = contact;
    }
  }
}
}  // namespace

void HalGPIO::armPowerWakeLatch() {
#if !POWER_WAKE_LATCH
  return;  // wedge-attribution control build: no latch, no GPIO ISR service
#endif
  powerLatchPin = BoardConfig::ACTIVE.input.power;
  powerLatchActiveHigh = BoardConfig::ACTIVE.input.powerActiveHigh;
  if (powerLatchPin < 0) {
    return;
  }
  powerLatchPressMs = 0;
  powerLatchLongestContactMs = 0;
  // Raw IDF ISR lifecycle, NOT Arduino attachInterrupt(): consume uninstalls
  // the GPIO ISR service (its CPU interrupt must not exist outside the latch
  // windows — an asserted level-type wake interrupt feeding the shared
  // service with no per-pin handler re-enters forever; the slice-sleep wedge,
  // docs/slice-wedge-findings.md #1). Arduino cannot survive that uninstall:
  // its core tracks the install in a never-reset function-local static
  // (esp32-hal-gpio.c, interrupt_initialized), so a later attachInterrupt()
  // would skip the reinstall and this ISR would silently never fire again.
  // Flag 0 = same service config Arduino used here before. A press edge
  // landing while flash cache is suspended (SPIFFS/NVS write) is held in the
  // GPIO status register and serviced late, not lost.
  const auto pin = static_cast<gpio_num_t>(powerLatchPin);
  const esp_err_t err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    LOG_ERR("GPIO", "Wake latch ISR service install failed: %d", static_cast<int>(err));
    powerLatchPin = -1;
    return;
  }
  gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
  gpio_isr_handler_add(pin, powerWakeLatchIsr, nullptr);
}

uint32_t HalGPIO::consumePowerWakeLatch() {
  if (powerLatchPin < 0) {
    return 0;
  }
  const auto pin = static_cast<gpio_num_t>(powerLatchPin);
  gpio_isr_handler_remove(pin);
  gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
  // Free the service's CPU interrupt entirely (safe: this latch is the only
  // ISR-service user in the tree). The slice hook declines while the latch is
  // armed, so the service and the light-sleep level wakes never coexist.
  gpio_uninstall_isr_service();
  uint32_t longestContactMs = powerLatchLongestContactMs;
  // Also count a press that is still held right now (pressed edge latched,
  // no release yet, contact time already past the bounce floor).
  if (powerLatchPressMs != 0 && (gpio_get_level(static_cast<gpio_num_t>(powerLatchPin)) != 0) == powerLatchActiveHigh) {
    const uint32_t contact = millis() - powerLatchPressMs;
    if (contact >= POWER_LATCH_MIN_CONTACT_MS && contact > longestContactMs) {
      longestContactMs = contact;
    }
  }
  powerLatchPin = -1;
  return longestContactMs;
}

bool HalGPIO::isPowerWakeLatchArmed() const { return powerLatchPin >= 0; }

bool HalGPIO::isUsbConnected() const {
  if (deviceIsX3()) {
    // X3: infer USB/charging via BQ27220 Current() register (0x0C, signed mA).
    // Positive current means charging.
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      int16_t currentMa = 0;
      if (X3GPIO::readBQ27220CurrentMA(&currentMa)) {
        return currentMa > 0;
      }
      delay(2);
    }
    return false;
  }
  if (BoardConfig::ACTIVE.usbDetect < 0) {
    return false;
  }
  return digitalRead(BoardConfig::ACTIVE.usbDetect) == HIGH;
}

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  const bool usbConnected = isUsbConnected();

  if (resetReason == ESP_RST_DEEPSLEEP &&
      (wakeupCause == ESP_SLEEP_WAKEUP_GPIO || wakeupCause == ESP_SLEEP_WAKEUP_EXT1)) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && !usbConnected) {
    return WakeupReason::PowerButton;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_UNKNOWN && usbConnected) {
    return WakeupReason::AfterFlash;
  }
  if (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON && usbConnected) {
    return WakeupReason::AfterUSBPower;
  }
  return WakeupReason::Other;
}
