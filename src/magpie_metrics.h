#pragma once

#include <ArduinoJson.h>

#include "app_state.h"

// Fills state's gateway fields from Magpie's /v1/magpie/quotas,
// /v1/magpie/usage, /v1/magpie/usage/requests?limit=1 and
// /v1/magpie/quotas/history responses (an empty history: no trends).
void buildAppState(const JsonDocument& quotas, const JsonDocument& usage,
                   const JsonDocument& requests, const JsonDocument& history,
                   AppState& state);

// An RFC 3339 time ("2026-11-09T07:00:00Z", "...+08:00") in Unix seconds;
// 0 if text isn't one.
uint64_t unixSeconds(const char* text);
