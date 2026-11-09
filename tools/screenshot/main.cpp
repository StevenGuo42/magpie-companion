// Renders each page of the real UI (src/ui.cpp) with sample data and writes
// it as raw RGB565 (480 x 480, little-endian) to <out_dir>/<page>.raw.
// Build and convert with tools/screenshot/build.sh.

#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include "app_state.h"
#include "ui.h"

namespace {

constexpr int kSize = 480;
uint32_t now_ms = 0;
uint16_t frame[kSize * kSize];

void flush(lv_disp_drv_t* driver, const lv_area_t* area, lv_color_t* pixels) {
  const int width = area->x2 - area->x1 + 1;
  for (int y = area->y1; y <= area->y2; ++y) {
    memcpy(&frame[y * kSize + area->x1], &pixels[(y - area->y1) * width],
           width * sizeof(uint16_t));
  }
  lv_disp_flush_ready(driver);
}

void run(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 5) {
    now_ms += 5;
    lv_timer_handler();
  }
}

// The trend runs from 100% at the cycle's start to left now, using up faster
// as it goes; the rest of the cycle is still to come.
void setWindow(QuotaWindow& window, const char* name, uint8_t left,
               uint64_t now, uint32_t resets_in, uint32_t cycle) {
  snprintf(window.name, sizeof(window.name), "%s", name);
  window.remaining_percent = left;
  window.resets_at = now + resets_in;
  const int current = (cycle - resets_in) * (kTrendPoints - 1) / cycle;
  for (int i = 0; i < kTrendPoints; ++i) {
    window.trend[i] = i > current ? kNoReading
                                  : 100 - (100 - left) * i * i / (current * current);
  }
}

// Made-up but plausible figures, so no one's real usage is published.
AppState sampleState() {
  AppState state;
  state.unix_time = 1794178800;  // 2026-11-08 23:00 UTC, 18:00 Eastern
  state.utc_offset_minutes = -300;
  state.time_valid = true;
  state.data_valid = true;
  state.connection = ConnectionState::kLinked;

  state.calls_today = 412;
  state.errors_today = 3;
  state.success_permille = 993;
  state.remaining_percent = 54;
  state.tokens_today = 48200000;
  state.average_ttft_ms = 1840;
  snprintf(state.latest_model, sizeof(state.latest_model), "claude-opus-5-5");
  state.latest_status = 200;
  state.latest_latency_ms = 5767;

  state.session_count = 38;
  state.total_calls = 9120;
  state.total_input = 2100000;
  state.total_output = 18400000;
  state.total_cache_read = 1100000000;
  state.total_reasoning = 3200000;
  state.total_cost_micros = 642180000;
  state.cache_hit_permille = 998;
  snprintf(state.top_model, sizeof(state.top_model), "claude-opus-5-5");
  state.top_model_calls = 4102;

  state.provider_count = 2;
  ProviderQuota& claude = state.providers[0];
  snprintf(claude.name, sizeof(claude.name), "Claude Code");
  claude.window_count = 3;
  constexpr uint32_t kFiveHours = 5 * 3600;
  constexpr uint32_t kWeek = 7 * 86400;
  const uint64_t now = state.unix_time;
  setWindow(claude.windows[0], "5 hours", 21, now, 2 * 3600 + 14 * 60, kFiveHours);
  setWindow(claude.windows[1], "7 days", 54, now, 2 * 86400 + 9 * 3600, kWeek);
  setWindow(claude.windows[2], "7 days - Fable", 8, now, 2 * 86400 + 9 * 3600, kWeek);
  ProviderQuota& codex = state.providers[1];
  snprintf(codex.name, sizeof(codex.name), "Codex");
  codex.window_count = 1;
  setWindow(codex.windows[0], "7 days", 91, now, 3 * 86400 + 5 * 3600, kWeek);
  return state;
}

bool save(const char* directory, const char* name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.raw", directory, name);
  FILE* file = fopen(path, "wb");
  if (file == nullptr) {
    perror(path);
    return false;
  }
  const bool ok = fwrite(frame, sizeof(frame), 1, file) == 1;
  fclose(file);
  return ok;
}

}  // namespace

extern "C" uint32_t millis(void) {
  return now_ms;
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <out_dir>\n", argv[0]);
    return 2;
  }

  lv_init();
  static lv_color_t buffer[kSize * kSize];
  static lv_disp_draw_buf_t draw_buffer;
  lv_disp_draw_buf_init(&draw_buffer, buffer, nullptr, kSize * kSize);
  static lv_disp_drv_t driver;
  lv_disp_drv_init(&driver);
  driver.hor_res = kSize;
  driver.ver_res = kSize;
  driver.flush_cb = flush;
  driver.draw_buf = &draw_buffer;
  lv_disp_drv_register(&driver);

  uiCreate([](UiAction) {});
  const AppState state = sampleState();
  uiUpdate(state, 0, 0, true);
  uiUpdateBattery(86, false);

  // The order of the pages in ui.cpp.
  const char* pages[] = {"overview", "usage", "requests", "sessions", "context"};
  for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
    if (i > 0) {
      uiCyclePage();
    }
    run(1000);  // let the page slide in
    if (!save(argv[1], pages[i])) {
      return 1;
    }
    if (strcmp(pages[i], "usage") == 0) {  // and its other view, then back
      uiUpdate(state, 0, 0, false);
      run(100);
      const bool saved = save(argv[1], "usage-bars");
      uiUpdate(state, 0, 0, true);
      if (!saved) {
        return 1;
      }
    }
  }
  return 0;
}
