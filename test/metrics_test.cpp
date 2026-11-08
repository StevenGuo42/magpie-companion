// Desktop check of the snapshot math (a port of the upstream companion's
// test_metrics.py). Run after `pio run` has fetched ArduinoJson:
//   g++ -std=c++17 -I src -I .pio/libdeps/esp32-s3-touch-amoled-216/ArduinoJson/src
//       test/metrics_test.cpp src/magpie_metrics.cpp -o metrics_test && ./metrics_test
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "magpie_metrics.h"

int main() {
  JsonDocument quotas;
  JsonDocument usage;
  JsonDocument requests;
  deserializeJson(quotas, R"({"data":[
    {"provider":"claude","name":"Claude Code","windows":[
      {"name":"5 hours","remaining":100},
      {"name":"7 days · Fable","remaining":80}]},
    {"provider":"codex","windows":[
      {"name":"5 hours","remaining":42.4,"resetsAt":"2027-01-15T10:00:00Z"},
      {"name":"Limit","remaining":0,"unlimited":true},
      {"name":"Credits","remaining":0,"text":true},
      {"name":"Wallet","remaining":0,"balance":12.5}]},
    {"provider":"openrouter","name":"OpenRouter","windows":[]}]})");
  deserializeJson(usage, R"({
    "series":[
      {"input":10,"output":2,"cache_read":3},
      {"input":100,"output":20,"reasoning":5,"cache_read":30,"calls":7,
       "errors":2,"timed":5,"ttft_ms":6250}],
    "calls":120,"input":1000,"output":200,"cache_read":3000,"reasoning":50,
    "cost":5000.25,"sessionCount":12,
    "models":[{"name":"gpt-5.6-sol","calls":40},{"name":"opus","calls":80},
              {"model":"tie","calls":80}]})");
  deserializeJson(requests,
                  R"({"rows":[{"served":"","model":"gpt-5.6-sol","status":200,"ms":9876}]})");

  AppState state;
  buildAppState(quotas, usage, requests, state);
  assert(state.remaining_percent == 42);
  assert(state.tokens_today == 155);
  assert(state.calls_today == 7);
  assert(state.errors_today == 2);
  assert(state.success_permille == 714);
  assert(state.average_ttft_ms == 1250);
  assert(strcmp(state.latest_model, "gpt-5.6-sol") == 0);
  assert(state.latest_status == 200);
  assert(state.latest_latency_ms == 9876);
  assert(!state.latest_failed);
  assert(state.session_count == 12);
  assert(state.total_calls == 120);
  assert(state.total_cost_micros == 5000250000ULL);  // past uint32 micros
  assert(state.cache_hit_permille == 750);
  assert(strcmp(state.top_model, "opus") == 0);  // first of a tie, as max()
  assert(state.top_model_calls == 80);

  // Usage page: accounts with a percentage window only, ASCII names.
  assert(state.provider_count == 2);
  assert(strcmp(state.providers[0].name, "Claude Code") == 0);
  assert(state.providers[0].window_count == 1);
  assert(strcmp(state.providers[0].windows[0].name, "7 days - Fable") == 0);
  assert(state.providers[0].windows[0].resets_at == 0);
  assert(strcmp(state.providers[1].name, "codex") == 0);  // no name: its id
  assert(state.providers[1].window_count == 1);
  assert(state.providers[1].windows[0].remaining_percent == 42);
  assert(state.providers[1].windows[0].resets_at == 1800007200ULL);

  assert(unixSeconds("2026-11-09T07:00:00.123456789+08:00") == 1794178800ULL);
  assert(unixSeconds("2024-02-29T23:59:59-05:30") == 1709270999ULL);
  assert(unixSeconds("2026-11-09 07:00:00") == 0);
  assert(unixSeconds("") == 0);

  JsonDocument empty;
  AppState blank;
  buildAppState(empty, empty, empty, blank);
  assert(blank.remaining_percent == 100);
  assert(blank.tokens_today == 0);
  assert(blank.success_permille == 1000);
  assert(strcmp(blank.latest_model, "-") == 0);
  assert(blank.latest_failed);
  assert(strcmp(blank.top_model, "-") == 0);
  assert(blank.provider_count == 0);

  puts("metrics_test: ok");
  return 0;
}
