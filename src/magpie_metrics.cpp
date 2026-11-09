#include "magpie_metrics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace {

uint64_t count(JsonVariantConst value) {
  const int64_t number = value | static_cast<int64_t>(0);
  return number > 0 ? static_cast<uint64_t>(number) : 0;
}

uint64_t tokens(JsonVariantConst bucket) {
  uint64_t sum = 0;
  for (const char* key :
       {"input", "output", "reasoning", "cache_read", "cache_write"}) {
    sum += count(bucket[key]);
  }
  return sum;
}

bool hasText(JsonVariantConst value) {
  const char* text = value | "";
  return text[0] != '\0';
}

void copyName(char* destination, size_t size, JsonVariantConst first,
              JsonVariantConst second) {
  const char* name = hasText(first) ? first.as<const char*>()
                                    : (hasText(second) ? second.as<const char*>()
                                                       : "-");
  snprintf(destination, size, "%s", name);
}

uint16_t permille(uint64_t part, uint64_t whole) {
  return whole == 0 ? 0 : static_cast<uint16_t>((part * 1000 + whole / 2) / whole);
}

// The display's fonts are ASCII only: other characters ("·") become '-'.
void copyAscii(char* destination, size_t size, const char* text) {
  size_t length = 0;
  for (; *text != '\0' && length + 1 < size; ++text) {
    const unsigned char c = *text;
    if (c < 0x80) {
      destination[length++] = c;
    } else if ((c & 0xC0) == 0xC0) {  // first byte of a multibyte character
      destination[length++] = '-';
    }
  }
  destination[length] = '\0';
}

// Text-only and prepaid-balance windows carry no percentage.
bool hasPercent(JsonVariantConst window) {
  return !(window["unlimited"] | false) && !(window["text"] | false) &&
         window["balance"].isNull() && window["remaining"].is<double>();
}

uint8_t percent(double value) {
  return static_cast<uint8_t>(std::clamp<long>(lround(value), 0, 100));
}

// The readings of one account's window: same provider, account and name.
JsonArrayConst historyPoints(const JsonDocument& history,
                             JsonVariantConst provider, const char* window) {
  for (JsonVariantConst account : history["data"].as<JsonArrayConst>()) {
    if (strcmp(account["provider"] | "", provider["provider"] | "") != 0 ||
        strcmp(account["user"] | "", provider["user"] | "") != 0) {
      continue;
    }
    for (JsonVariantConst line : account["lines"].as<JsonArrayConst>()) {
      if (strcmp(line["name"] | "", window) == 0) {
        return line["points"];
      }
    }
  }
  return JsonArrayConst();
}

// Spreads the readings of the cycle that resets at resets_at over trend, each
// held until the next. Points come oldest first.
void fillTrend(JsonArrayConst points, uint64_t resets_at, uint8_t* trend) {
  std::fill(trend, trend + kTrendPoints, kNoReading);
  if (resets_at == 0) {
    return;
  }
  // A minute's slack: some vendors' reset times drift between reads.
  auto this_cycle = [resets_at](JsonVariantConst point) {
    const uint64_t reset = unixSeconds(point["resetsAt"] | "");
    return reset + 60 >= resets_at && reset <= resets_at + 60;
  };
  uint64_t start = 0;
  for (JsonVariantConst point : points) {
    if (this_cycle(point)) {
      start = unixSeconds(point["start"] | "");
    }
  }
  if (start == 0 || start >= resets_at) {
    return;
  }
  const uint64_t cycle = resets_at - start;
  // Magpie may first read a quota hours after it resets: a cycle is assumed
  // to start with all of it left, unless a reading at its start says otherwise.
  trend[0] = 100;
  int last = 0;
  for (JsonVariantConst point : points) {
    const uint64_t at = unixSeconds(point["at"] | "");
    // With the start drifting, the last reading of the cycle before can
    // fall after it: a reading that names its cycle must name this one.
    if (at < start || at > resets_at || !point["left"].is<double>() ||
        (!point["resetsAt"].isNull() && !this_cycle(point))) {
      continue;
    }
    const int index = static_cast<int>(
        ((at - start) * (kTrendPoints - 1) + cycle / 2) / cycle);
    for (int i = last + 1; i < index; ++i) {
      trend[i] = trend[last];
    }
    trend[index] = percent(point["left"].as<double>());
    last = std::max(last, index);
  }
}

}  // namespace

uint64_t unixSeconds(const char* text) {
  int year, month, day, hour, minute, second, consumed = 0;
  if (text == nullptr ||
      sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour,
             &minute, &second, &consumed) != 6) {
    return 0;
  }
  const char* zone = text + consumed;
  while (*zone == '.' || (*zone >= '0' && *zone <= '9')) {
    ++zone;  // fractional seconds
  }
  int64_t offset = 0;
  if (*zone == '+' || *zone == '-') {
    int zone_hours, zone_minutes;
    if (sscanf(zone + 1, "%2d:%2d", &zone_hours, &zone_minutes) != 2) {
      return 0;
    }
    offset = (zone_hours * 60 + zone_minutes) * 60 * (*zone == '-' ? -1 : 1);
  } else if (*zone != 'Z') {
    return 0;
  }
  // Days since 1970-01-01 (Howard Hinnant's days_from_civil).
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t year_of_era = year - era * 400;
  const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  const int64_t seconds = (era * 146097 + day_of_era - 719468) * 86400 +
                          hour * 3600 + minute * 60 + second - offset;
  return seconds > 0 ? static_cast<uint64_t>(seconds) : 0;
}

void buildAppState(const JsonDocument& quotas, const JsonDocument& usage,
                   const JsonDocument& requests, const JsonDocument& history,
                   AppState& state) {
  // The most constrained limited window across every account; 100 if none.
  // Accounts with a percentage window are also listed for the Usage page.
  double lowest = 100;
  state.provider_count = 0;
  for (JsonVariantConst provider : quotas["data"].as<JsonArrayConst>()) {
    // shortcut: accounts past kMaxProviders and windows past kMaxWindows are
    // left off the Usage page; raise the limits if Magpie lists more.
    ProviderQuota* entry = state.provider_count < kMaxProviders
                               ? &state.providers[state.provider_count]
                               : nullptr;
    uint8_t windows = 0;
    for (JsonVariantConst window : provider["windows"].as<JsonArrayConst>()) {
      if (!hasPercent(window)) {
        continue;
      }
      lowest = std::min(lowest, window["remaining"].as<double>());
      // An untouched 5-hour window says nothing; it's listed once used.
      const bool idle_five_hours =
          strcmp(window["name"] | "", "5 hours") == 0 &&
          percent(window["remaining"].as<double>()) == 100;
      if (entry != nullptr && windows < kMaxWindows && !idle_five_hours) {
        QuotaWindow& slot = entry->windows[windows++];
        copyAscii(slot.name, sizeof(slot.name), window["name"] | "-");
        slot.remaining_percent = percent(window["remaining"].as<double>());
        slot.resets_at = unixSeconds(window["resetsAt"] | "");
        fillTrend(historyPoints(history, provider, window["name"] | ""),
                  slot.resets_at, slot.trend);
      }
    }
    if (entry != nullptr && windows > 0) {
      copyAscii(entry->name, sizeof(entry->name),
                hasText(provider["name"]) ? provider["name"].as<const char*>()
                                          : provider["provider"] | "-");
      entry->window_count = windows;
      ++state.provider_count;
    }
  }
  state.remaining_percent = percent(lowest);

  // The default period is 30 days bucketed by day; the last bucket is today.
  JsonArrayConst series = usage["series"].as<JsonArrayConst>();
  JsonVariantConst today =
      series.size() > 0 ? series[series.size() - 1] : JsonVariantConst();
  state.tokens_today = tokens(today);
  state.calls_today = count(today["calls"]);
  state.errors_today =
      std::min<uint64_t>(state.calls_today, count(today["errors"]));
  state.success_permille =
      state.calls_today == 0
          ? 1000
          : permille(state.calls_today - state.errors_today, state.calls_today);
  const uint64_t timed_today = count(today["timed"]);
  state.average_ttft_ms =
      timed_today == 0 ? 0 : count(today["ttft_ms"]) / timed_today;

  JsonVariantConst latest = requests["rows"][0];
  copyName(state.latest_model, sizeof(state.latest_model), latest["served"],
           latest["model"]);
  state.latest_status = count(latest["status"]);
  state.latest_latency_ms = count(latest["ms"]);
  state.latest_failed = hasText(latest["err"]) || hasText(latest["error"]) ||
                        state.latest_status < 200 || state.latest_status >= 400;

  state.session_count = count(usage["sessionCount"]);
  state.total_calls = count(usage["calls"]);
  state.total_input = count(usage["input"]);
  state.total_output = count(usage["output"]);
  state.total_cache_read = count(usage["cache_read"]);
  state.total_reasoning = count(usage["reasoning"]);
  state.total_cost_micros = static_cast<uint64_t>(
      llround(std::max(0.0, usage["cost"] | 0.0) * 1000000.0));
  state.cache_hit_permille = permille(
      state.total_cache_read, state.total_input + state.total_cache_read);

  JsonVariantConst top_model;
  for (JsonVariantConst model : usage["models"].as<JsonArrayConst>()) {
    if (top_model.isNull() || count(model["calls"]) > count(top_model["calls"])) {
      top_model = model;
    }
  }
  copyName(state.top_model, sizeof(state.top_model), top_model["name"],
           top_model["model"]);
  state.top_model_calls = count(top_model["calls"]);
}
