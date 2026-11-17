#pragma once

#include "app_state.h"

enum class UiAction : uint8_t {
  kRefresh,
  kReconnect,
  kNextProvider,
  kRotate,
  kToggleUsageView,
};

using UiActionHandler = void (*)(UiAction action);

void uiCreate(UiActionHandler action_handler);
// provider: index into state.providers shown on the Usage page; usage_trends:
// that page charts each window's cycle instead of a bar of what's left.
void uiUpdate(const AppState& state, uint32_t elapsed_seconds,
              uint8_t provider, bool usage_trends);
// percent: 0-100, or -1 with no battery (then only a plug is shown).
void uiUpdateBattery(int percent, bool plugged_in);
void uiCyclePage();
void uiShowQuickActions();
void uiShowToast(const char* text);
// AMOLED burn-in guard: moves everything to the next of 9 spots, 3 px apart.
void uiShiftPixels();
bool uiModalVisible();
