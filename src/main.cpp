#include <Arduino.h>
#include "board_config.h"

#include <Arduino_GFX_Library.h>
#if __has_include(<TouchDrv.hpp>)
#include <TouchDrv.hpp>
#else
#include <TouchDrvCSTXXX.hpp>
#endif
#include <Wire.h>
#include <XPowersLib.h>
#include <Preferences.h>
#include <lvgl.h>

#include "app_state.h"
#include "magpie_bridge.h"
#include "ui.h"

namespace {

constexpr uint32_t kUiRefreshMs = 1000;
constexpr uint32_t kScreenOffLvglRefreshMs = 50;
constexpr uint32_t kIdleScreenTimeoutMs = 15 * 1000;
constexpr uint32_t kGlanceIntervalMs = 10 * 60 * 1000;
constexpr uint32_t kGlanceMs = 10 * 1000;
constexpr uint32_t kDataStaleMs = 60 * 1000;
constexpr uint32_t kButtonDebounceMs = 35;
constexpr uint32_t kCodexLongPressMs = 1000;
constexpr uint32_t kBootMaxShortPressMs = 800;
constexpr uint32_t kBootWpsMs = 3000;
constexpr size_t kDrawBufferRows = 40;

Arduino_DataBus* display_bus = new Arduino_ESP32QSPI(
    board::kLcdChipSelect, board::kLcdClock, board::kLcdSdio0,
    board::kLcdSdio1, board::kLcdSdio2, board::kLcdSdio3);

Arduino_CO5300* display = new Arduino_CO5300(
    display_bus, board::kLcdReset, 0, board::kScreenWidth,
    board::kScreenHeight, 0, 0, 0, 0);

TouchDrvCST92xx touch;
XPowersPMU power;

lv_disp_draw_buf_t draw_buffer;
lv_color_t* draw_buffer_a = nullptr;
lv_color_t* draw_buffer_b = nullptr;

int16_t touch_x[2] = {};
int16_t touch_y[2] = {};
volatile bool touch_pending = false;

AppState app_state;
uint32_t state_received_at_ms = 0;
uint32_t last_ui_refresh_ms = 0;
uint32_t last_lvgl_refresh_ms = 0;
uint32_t last_activity_ms = 0;
uint32_t screen_off_ms = 0;
bool screen_on = true;
bool power_ready = false;

// Kept across reboots: the screen's rotation (0-3, quarter turns) and the
// Usage page's account and view.
Preferences settings;
lv_disp_t* lvgl_display = nullptr;
char selected_provider[24] = "";
uint8_t provider_index = 0;
bool usage_trends = true;

struct ButtonState {
  int pin;
  bool stable_pressed = false;
  bool raw_pressed = false;
  bool long_press_sent = false;
  uint32_t raw_changed_at = 0;
  uint32_t pressed_at = 0;
};

ButtonState boot_button{board::kBootButton};
ButtonState codex_button{board::kCodexButton};

void setScreenOn(bool on) {
  if (screen_on == on) {
    return;
  }
  screen_on = on;
  if (!on) {
    screen_off_ms = millis();
  }
  if (on) {
    uiShiftPixels();  // drawn while dark, so the move isn't seen
    lv_refr_now(nullptr);
    display->displayOn();
    display->setBrightness(board::kDisplayBrightness);
  } else {
    display->setBrightness(0);
    display->displayOff();
  }
}

void IRAM_ATTR onTouchInterrupt() {
  touch_pending = true;
}

bool consumeTouchInterrupt() {
  noInterrupts();
  const bool pending = touch_pending;
  touch_pending = false;
  interrupts();
  return pending;
}

void updateTouchWake() {
  if (screen_on || !consumeTouchInterrupt()) {
    return;
  }

  touch.getPoint(touch_x, touch_y, touch.getSupportTouchPoint());
  Serial.println("[display] wake by touch");
  setScreenOn(true);
  last_activity_ms = millis();
  // The waking finger may still be down: it mustn't tap what's under it.
  lv_indev_wait_release(lv_indev_get_next(nullptr));
}

void displayFlush(lv_disp_drv_t* driver, const lv_area_t* area,
                  lv_color_t* pixels) {
  const uint32_t width = area->x2 - area->x1 + 1;
  const uint32_t height = area->y2 - area->y1 + 1;

#if LV_COLOR_16_SWAP
  display->draw16bitBeRGBBitmap(area->x1, area->y1,
                                reinterpret_cast<uint16_t*>(&pixels->full),
                                width, height);
#else
  display->draw16bitRGBBitmap(area->x1, area->y1,
                              reinterpret_cast<uint16_t*>(&pixels->full),
                              width, height);
#endif

  lv_disp_flush_ready(driver);
}

void roundDisplayArea(lv_disp_drv_t*, lv_area_t* area) {
  if (area->x1 % 2 != 0) {
    --area->x1;
  }
  if (area->y1 % 2 != 0) {
    --area->y1;
  }
  if (area->x2 % 2 == 0) {
    ++area->x2;
  }
  if (area->y2 % 2 == 0) {
    ++area->y2;
  }
}

void readTouch(lv_indev_drv_t*, lv_indev_data_t* data) {
  // A touch on the dark screen is left to updateTouchWake: it only wakes.
  if (!screen_on) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  consumeTouchInterrupt();
  const uint8_t points =
      touch.getPoint(touch_x, touch_y, touch.getSupportTouchPoint());
  if (points == 0) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  last_activity_ms = millis();
  data->state = LV_INDEV_STATE_PR;
  data->point.x = touch_x[0];
  data->point.y = touch_y[0];
}

// The Usage page's account, found again by its saved name. The current one
// is kept while it still has that name, so accounts sharing a name cycle.
uint8_t selectedProvider() {
  if (provider_index < app_state.provider_count &&
      strcmp(app_state.providers[provider_index].name, selected_provider) == 0) {
    return provider_index;
  }
  for (uint8_t i = 0; i < app_state.provider_count; ++i) {
    if (strcmp(app_state.providers[i].name, selected_provider) == 0) {
      return provider_index = i;
    }
  }
  return provider_index = 0;
}

void handleUiAction(UiAction action) {
  switch (action) {
    case UiAction::kNextProvider:
      if (app_state.provider_count > 1) {
        provider_index = (selectedProvider() + 1) % app_state.provider_count;
        strlcpy(selected_provider, app_state.providers[provider_index].name,
                sizeof(selected_provider));
        settings.putString("provider", selected_provider);
        last_ui_refresh_ms = millis() - kUiRefreshMs;  // redraw now
      }
      return;
    case UiAction::kToggleUsageView:
      usage_trends = !usage_trends;
      settings.putBool("trends", usage_trends);
      last_ui_refresh_ms = millis() - kUiRefreshMs;  // redraw now
      return;
    case UiAction::kRotate: {
      const uint8_t rotation = (settings.getUChar("rotation", 0) + 1) % 4;
      settings.putUChar("rotation", rotation);
      lv_disp_set_rotation(lvgl_display, static_cast<lv_disp_rot_t>(rotation));
      return;
    }
    case UiAction::kRefresh:
      netRequestRefresh();
      uiShowToast("REFRESHING");
      return;
    case UiAction::kReconnect:
      netReconnect();
      uiShowToast("RECONNECTING");
      return;
  }
}

bool initPower() {
  if (!power.begin(Wire, AXP2101_SLAVE_ADDRESS, board::kI2cSda,
                   board::kI2cScl)) {
    Serial.println("[pmu] AXP2101 not found");
    return false;
  }

  power.setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
  power.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);  // the PMU's shortest
  power.enableLongPressShutdown();
  power.setLongPressPowerOFF();
  power.setChargeTargetVoltage(3);
  power.enableBattDetection();
  power.enableBattVoltageMeasure();
  Serial.println("[pmu] long-press shutdown configured: 4 seconds");
  return true;
}

bool initDisplay() {
  if (!display->begin()) {
    Serial.println("[display] initialization failed");
    return false;
  }
  display_bus->writeC8D8(0x36, 0xA0);
  display->fillScreen(RGB565_BLACK);
  display->setBrightness(board::kDisplayBrightness);
  return true;
}

bool initTouch() {
  touch.setPins(board::kTouchReset, board::kTouchInterrupt);
  if (!touch.begin(Wire, CST92XX_SLAVE_ADDRESS, board::kI2cSda,
                   board::kI2cScl)) {
    Serial.println("[touch] CST9217 not found");
    return false;
  }

  touch.setMaxCoordinates(board::kScreenWidth, board::kScreenHeight);
  touch.setSwapXY(true);
  touch.setMirrorXY(true, false);
  pinMode(board::kTouchInterrupt, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(board::kTouchInterrupt),
                  onTouchInterrupt, FALLING);
  Serial.printf("[touch] model: %s\n", touch.getModelName());
  return true;
}

bool initLvgl() {
  lv_init();

  constexpr size_t pixel_count = board::kScreenWidth * kDrawBufferRows;
  draw_buffer_a = static_cast<lv_color_t*>(
      heap_caps_malloc(pixel_count * sizeof(lv_color_t),
                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  draw_buffer_b = static_cast<lv_color_t*>(
      heap_caps_malloc(pixel_count * sizeof(lv_color_t),
                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

  if (draw_buffer_a == nullptr) {
    Serial.println("[lvgl] failed to allocate draw buffer");
    return false;
  }

  lv_disp_draw_buf_init(&draw_buffer, draw_buffer_a, draw_buffer_b,
                        pixel_count);

  static lv_disp_drv_t display_driver;
  lv_disp_drv_init(&display_driver);
  display_driver.hor_res = board::kScreenWidth;
  display_driver.ver_res = board::kScreenHeight;
  display_driver.flush_cb = displayFlush;
  display_driver.rounder_cb = roundDisplayArea;
  display_driver.draw_buf = &draw_buffer;
  // LVGL rotates the image and the touch points; the panel stays as set up.
  display_driver.sw_rotate = 1;
  display_driver.rotated =
      static_cast<lv_disp_rot_t>(settings.getUChar("rotation", 0) % 4);
  lvgl_display = lv_disp_drv_register(&display_driver);

  static lv_indev_drv_t touch_driver;
  lv_indev_drv_init(&touch_driver);
  touch_driver.type = LV_INDEV_TYPE_POINTER;
  touch_driver.read_cb = readTouch;
  lv_indev_drv_register(&touch_driver);

  uiCreate(handleUiAction);
  state_received_at_ms = millis();
  uiUpdate(app_state, 0, 0, usage_trends);
  return true;
}

template <typename ShortPress, typename LongPress>
void updateButton(ButtonState& button, uint32_t long_press_ms,
                  ShortPress short_press, LongPress long_press) {
  const uint32_t now = millis();
  const bool pressed = digitalRead(button.pin) == LOW;

  if (pressed != button.raw_pressed) {
    button.raw_pressed = pressed;
    button.raw_changed_at = now;
  }

  if (now - button.raw_changed_at < kButtonDebounceMs ||
      button.stable_pressed == button.raw_pressed) {
    if (button.stable_pressed && !button.long_press_sent &&
        now - button.pressed_at >= long_press_ms) {
      button.long_press_sent = true;
      long_press();
    }
    return;
  }

  button.stable_pressed = button.raw_pressed;
  if (button.stable_pressed) {
    button.pressed_at = now;
    button.long_press_sent = false;
    return;
  }

  if (!button.long_press_sent) {
    short_press(now - button.pressed_at);
  }
}

void updateButtons() {
  updateButton(
      boot_button, kBootWpsMs,
      [](uint32_t held_ms) {
        if (held_ms <= kBootMaxShortPressMs) {
          setScreenOn(!screen_on);
          if (screen_on) {
            last_activity_ms = millis();
          }
        }
      },
      []() {
        setScreenOn(true);
        last_activity_ms = millis();
        uiShowToast(netStartWps() ? "WPS: PRESS THE ROUTER'S WPS BUTTON"
                                  : "WPS FAILED TO START");
      });

  updateButton(
      codex_button, kCodexLongPressMs,
      [](uint32_t) {
        last_activity_ms = millis();
        const bool was_screen_on = screen_on;
        setScreenOn(true);
        if (was_screen_on && !uiModalVisible()) {
          uiCyclePage();
        }
      },
      []() {
        last_activity_ms = millis();
        setScreenOn(true);
        uiShowQuickActions();
      });
}

void refreshUi() {
  if (!screen_on) {
    return;
  }

  const uint32_t now = millis();
  if (now - last_ui_refresh_ms < kUiRefreshMs) {
    return;
  }
  last_ui_refresh_ms = now;
  const uint32_t state_age_seconds = (now - state_received_at_ms) / 1000UL;
  uiUpdate(app_state, state_age_seconds, selectedProvider(), usage_trends);
  if (power_ready) {
    uiUpdateBattery(power.getBatteryPercent(), power.isCharging());
  }
}

void updateNet() {
  static AppState received_state;  // ~2.5 KB: off the loop task's stack
  if (netTakeState(received_state)) {
    app_state = received_state;
    state_received_at_ms = millis();
  }

  if (netWpsActive()) {
    app_state.connection = ConnectionState::kWps;
  } else if (!netConnected()) {
    app_state.connection = ConnectionState::kDisconnected;
  } else if (!app_state.data_valid) {
    app_state.connection = ConnectionState::kSyncing;
  } else if (millis() - state_received_at_ms >= kDataStaleMs) {
    app_state.connection = ConnectionState::kStale;
  } else {
    app_state.connection = ConnectionState::kLinked;
  }

  NetMessage message;
  if (netTakeMessage(message)) {
    uiShowToast(message.text);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nMagpie Usage Display starting");

  pinMode(board::kBootButton, INPUT_PULLUP);
  pinMode(board::kCodexButton, INPUT_PULLUP);
  Wire.begin(board::kI2cSda, board::kI2cScl);

  settings.begin("display");
  settings.getString("provider", selected_provider, sizeof(selected_provider));
  usage_trends = settings.getBool("trends", true);
  power_ready = initPower();
  const bool display_ok = initDisplay();
  const bool touch_ok = initTouch();

  if (!display_ok || !initLvgl()) {
    Serial.println("[fatal] display stack initialization failed");
    while (true) {
      delay(1000);
    }
  }

  if (!power_ready) {
    uiShowToast("PMU OFFLINE");
  } else if (!touch_ok) {
    uiShowToast("TOUCH OFFLINE");
  }

  if (!netBegin()) {
    uiShowToast("WIFI INIT FAILED");
  }

  lv_mem_monitor_t lvgl_memory;
  lv_mem_monitor(&lvgl_memory);
  Serial.printf("[mem] heap free %lu, LVGL free %lu (largest %lu)\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(lvgl_memory.free_size),
                static_cast<unsigned long>(lvgl_memory.free_biggest_size));
  Serial.println("Magpie Usage Display ready");
  last_activity_ms = millis();
}

void loop() {
  updateButtons();
  updateTouchWake();
  updateNet();
  refreshUi();

  const uint32_t lvgl_now = millis();
  if (screen_on ||
      lvgl_now - last_lvgl_refresh_ms >= kScreenOffLvglRefreshMs) {
    last_lvgl_refresh_ms = lvgl_now;
    lv_timer_handler();
  }

  const uint32_t now = millis();
  // Stays on during WPS so its status is visible from the router.
  if (screen_on && !netWpsActive() &&
      now - last_activity_ms >= kIdleScreenTimeoutMs) {
    setScreenOn(false);
  }

  // A 10-second glance every 10 minutes while asleep; the idle timeout
  // above turns it off again.
  if (!screen_on && now - screen_off_ms >= kGlanceIntervalMs) {
    setScreenOn(true);
    last_activity_ms = now - (kIdleScreenTimeoutMs - kGlanceMs);
  }

  delay(5);
}
