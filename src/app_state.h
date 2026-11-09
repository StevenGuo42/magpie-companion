#pragma once

#include <stdint.h>

enum class ConnectionState : uint8_t {
  kDisconnected,
  kSyncing,
  kLinked,
  kStale,
  kWps,
};

constexpr uint8_t kMaxProviders = 6;
constexpr uint8_t kMaxWindows = 4;
constexpr uint8_t kTrendPoints = 48;
constexpr uint8_t kNoReading = 0xFF;

struct QuotaWindow {
  char name[24];
  uint8_t remaining_percent;
  // Percent left at kTrendPoints even steps from the cycle's start to its
  // reset; kNoReading where there's no reading (yet).
  uint8_t trend[kTrendPoints];
  uint64_t resets_at;  // Unix seconds, 0 if unknown
};

struct ProviderQuota {
  char name[24];
  uint8_t window_count;
  QuotaWindow windows[kMaxWindows];
};

struct AppState {
  uint8_t remaining_percent = 0;
  uint64_t tokens_today = 0;
  uint32_t calls_today = 0;
  uint32_t errors_today = 0;
  uint16_t success_permille = 1000;
  uint32_t average_ttft_ms = 0;
  char latest_model[32] = "-";
  uint16_t latest_status = 0;
  uint32_t latest_latency_ms = 0;
  bool latest_failed = false;
  uint16_t session_count = 0;
  uint32_t total_calls = 0;
  uint64_t total_input = 0;
  uint64_t total_output = 0;
  uint64_t total_cache_read = 0;
  uint64_t total_reasoning = 0;
  uint64_t total_cost_micros = 0;
  uint16_t cache_hit_permille = 0;
  char top_model[24] = "-";
  uint32_t top_model_calls = 0;
  uint8_t provider_count = 0;
  ProviderQuota providers[kMaxProviders] = {};

  uint64_t unix_time = 0;
  int16_t utc_offset_minutes = 0;
  ConnectionState connection = ConnectionState::kDisconnected;
  bool time_valid = false;
  bool data_valid = false;
};

// Arduino-only so the metrics test builds on a desktop compiler.
#ifdef ARDUINO
#include <Arduino.h>

String formatCompactTokens(uint64_t tokens);
String formatCountdown(uint32_t seconds);
String formatDateTime(const AppState& state, uint32_t elapsed_seconds = 0);
const char* connectionLabel(ConnectionState state);
#endif
