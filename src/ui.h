#pragma once

#include "app_state.h"

enum class UiAction : uint8_t {
  kRefresh,
  kReconnect,
  kNextProvider,
  kRotate,
};

using UiActionHandler = void (*)(UiAction action);

void uiCreate(UiActionHandler action_handler);
// provider: index into state.providers shown on the Usage page.
void uiUpdate(const AppState& state, uint32_t elapsed_seconds,
              uint8_t provider);
void uiUpdateBattery(int percent, bool charging);
void uiCyclePage();
void uiShowQuickActions();
void uiShowToast(const char* text);
// AMOLED burn-in guard: moves everything to the next of 9 spots, 3 px apart.
void uiShiftPixels();
bool uiModalVisible();
