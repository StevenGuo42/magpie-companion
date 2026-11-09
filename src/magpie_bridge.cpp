#include "magpie_bridge.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_wps.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "magpie_metrics.h"
#include "secrets.h"

namespace {

constexpr uint32_t kRefreshMs = 15 * 1000;
constexpr uint32_t kWifiRetryMs = 1000;
// Magpie may wait up to 12 s on vendors before answering /quotas.
constexpr uint16_t kHttpTimeoutMs = 20 * 1000;
constexpr int kInvalidJson = -100;
constexpr time_t kClockSetAfter = 1700000000;

QueueHandle_t state_queue = nullptr;
QueueHandle_t message_queue = nullptr;
TaskHandle_t poll_task = nullptr;
volatile bool poll_failed = false;
volatile bool wps_active = false;
int last_failure = 0;

// The quota history holds every reading of the past days: kept out of
// internal RAM.
struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void* pointer) override {
    heap_caps_free(pointer);
  }
  void* reallocate(void* pointer, size_t size) override {
    return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM);
  }
};

void post(const char* text) {
  NetMessage message{};
  snprintf(message.text, sizeof(message.text), "%s", text);
  xQueueOverwrite(message_queue, &message);
}

// The network joined last, kept in flash by the Wi-Fi driver.
bool hasSavedNetwork() {
  wifi_config_t config{};
  return esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK &&
         config.sta.ssid[0] != '\0';
}

void stopWps() {
  esp_wifi_wps_disable();
  wps_active = false;
  WiFi.setAutoReconnect(true);
}

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t) {
  switch (event) {
    case ARDUINO_EVENT_WPS_ER_SUCCESS:
      stopWps();
      Serial.println("[net] WPS paired");
      post("WPS PAIRED");
      WiFi.begin();  // joins the network WPS just saved
      break;
    case ARDUINO_EVENT_WPS_ER_FAILED:
    case ARDUINO_EVENT_WPS_ER_TIMEOUT:
    case ARDUINO_EVENT_WPS_ER_PBC_OVERLAP:
      stopWps();
      Serial.printf("[net] WPS ended without pairing (event %d)\n", event);
      post(event == ARDUINO_EVENT_WPS_ER_PBC_OVERLAP
               ? "WPS: TWO ROUTERS IN WPS MODE"
               : "WPS FAILED: HOLD BOOT TO RETRY");
      if (hasSavedNetwork()) {
        WiFi.begin();
      }
      break;
    default:
      break;
  }
}

// shortcut: plain HTTP only; switch to NetworkClientSecure if Magpie serves TLS.
int getJson(const char* path, const JsonDocument& filter,
            JsonDocument& document) {
  HTTPClient http;
  http.setTimeout(kHttpTimeoutMs);
  // HTTP/1.0 keeps Go from chunking, so the body streams into ArduinoJson.
  http.useHTTP10(true);
  if (!http.begin(String(MAGPIE_URL) + path)) {
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  http.addHeader("Authorization", "Bearer " MAGPIE_GATEWAY_KEY);
  http.addHeader("Accept", "application/json");

  int status = http.GET();
  if (status == HTTP_CODE_OK &&
      deserializeJson(document, http.getStream(),
                      DeserializationOption::Filter(filter))) {
    status = kInvalidJson;
  }
  http.end();
  if (status != HTTP_CODE_OK) {
    Serial.printf("[net] GET %s failed: %d %s\n", path, status,
                  HTTPClient::errorToString(status).c_str());
  }
  return status;
}

void reportFailure(int status) {
  poll_failed = true;
  if (status == last_failure) {
    return;
  }
  last_failure = status;
  NetMessage message{};
  if (status > 0) {
    snprintf(message.text, sizeof(message.text), "MAGPIE HTTP %d", status);
  } else {
    snprintf(message.text, sizeof(message.text), "%s",
             status == kInvalidJson ? "MAGPIE SENT INVALID DATA"
                                    : "MAGPIE UNREACHABLE");
  }
  xQueueOverwrite(message_queue, &message);
}

void poll() {
  // Filters keep only what the display shows; the usage summary also lists
  // every session, account and key.
  static JsonDocument quotas_filter;
  static JsonDocument usage_filter;
  static JsonDocument requests_filter;
  static JsonDocument history_filter;
  if (quotas_filter.isNull()) {
    for (const char* key : {"name", "provider", "user"}) {
      quotas_filter["data"][0][key] = true;
    }
    for (const char* key : {"name", "remaining", "resetsAt", "unlimited",
                            "text", "balance"}) {
      quotas_filter["data"][0]["windows"][0][key] = true;
    }
    for (const char* key : {"input", "output", "reasoning", "cache_read",
                            "cache_write", "calls", "errors", "timed",
                            "ttft_ms"}) {
      usage_filter["series"][0][key] = true;
    }
    for (const char* key : {"input", "output", "reasoning", "cache_read",
                            "calls", "cost", "sessionCount"}) {
      usage_filter[key] = true;
    }
    for (const char* key : {"name", "model", "calls"}) {
      usage_filter["models"][0][key] = true;
    }
    for (const char* key : {"served", "model", "status", "ms", "err",
                            "error"}) {
      requests_filter["rows"][0][key] = true;
    }
    history_filter["data"][0]["provider"] = true;
    history_filter["data"][0]["user"] = true;
    history_filter["data"][0]["lines"][0]["name"] = true;
    for (const char* key : {"at", "left", "start", "resetsAt"}) {
      history_filter["data"][0]["lines"][0]["points"][0][key] = true;
    }
  }

  static SpiRamAllocator spi_ram;
  JsonDocument quotas;
  JsonDocument usage;
  JsonDocument requests;
  JsonDocument history(&spi_ram);
  int status = getJson("/v1/magpie/quotas", quotas_filter, quotas);
  if (status == HTTP_CODE_OK) {
    status = getJson("/v1/magpie/usage", usage_filter, usage);
  }
  if (status == HTTP_CODE_OK) {
    // via=through: gateway calls only; imported session-log rows have no status
    status = getJson("/v1/magpie/usage/requests?limit=1&via=through",
                     requests_filter,
                     requests);
  }
  if (status != HTTP_CODE_OK) {
    reportFailure(status);
    return;
  }
  // Optional: without it (an older Magpie) the Usage page has no trends.
  // shortcut: 8 days covers 7-day windows; longer ones start mid-cycle.
  if (getJson("/v1/magpie/quotas/history?days=8", history_filter, history) !=
      HTTP_CODE_OK) {
    history.clear();
  }

  AppState state;
  buildAppState(quotas, usage, requests, history, state);
  const time_t now = time(nullptr);
  state.time_valid = now > kClockSetAfter;
  if (state.time_valid) {
    struct tm local {};
    localtime_r(&now, &local);
    char zone[8];
    strftime(zone, sizeof(zone), "%z", &local);  // "+0800"
    const int hhmm = atoi(zone);
    state.unix_time = now;
    state.utc_offset_minutes = hhmm / 100 * 60 + hhmm % 100;
  }
  state.connection = ConnectionState::kLinked;
  state.data_valid = true;

  xQueueOverwrite(state_queue, &state);
  poll_failed = false;
  last_failure = 0;
  Serial.printf("[net] status: calls %lu, quota %u%%\n",
                static_cast<unsigned long>(state.calls_today),
                state.remaining_percent);
}

void pollLoop(void*) {
  while (true) {
    if (WiFi.status() != WL_CONNECTED) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kWifiRetryMs));
      continue;
    }
    poll();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kRefreshMs));
  }
}

}  // namespace

bool netBegin() {
  state_queue = xQueueCreate(1, sizeof(AppState));
  message_queue = xQueueCreate(1, sizeof(NetMessage));
  if (state_queue == nullptr || message_queue == nullptr) {
    Serial.println("[net] failed to allocate queues");
    return false;
  }

  WiFi.onEvent(onWifiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (hasSavedNetwork()) {
    WiFi.begin();
  } else if (netStartWps()) {
    post("WPS: PRESS THE ROUTER'S WPS BUTTON");
  }
  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");

  // Room for an AppState (~2.5 KB with trends) on top of the HTTP and JSON work.
  if (xTaskCreate(pollLoop, "magpie_poll", 12288, nullptr, 1, &poll_task) !=
      pdPASS) {
    Serial.println("[net] failed to start poll task");
    return false;
  }
  Serial.printf("[net] polling %s\n", MAGPIE_URL);
  return true;
}

bool netStartWps() {
  if (wps_active) {
    return true;
  }
  // The core's reconnects on disconnect would cut into the WPS exchange.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  esp_wps_config_t config = WPS_CONFIG_INIT_DEFAULT(WPS_TYPE_PBC);
  if (esp_wifi_wps_enable(&config) != ESP_OK ||
      esp_wifi_wps_start(0) != ESP_OK) {
    Serial.println("[net] WPS failed to start");
    stopWps();
    return false;
  }
  wps_active = true;
  Serial.println("[net] WPS started; press the router's WPS button");
  return true;
}

bool netWpsActive() {
  return wps_active;
}

bool netConnected() {
  return WiFi.status() == WL_CONNECTED && !poll_failed;
}

void netRequestRefresh() {
  if (poll_task != nullptr) {
    xTaskNotifyGive(poll_task);
  }
}

void netReconnect() {
  WiFi.reconnect();
  netRequestRefresh();
}

bool netTakeState(AppState& state) {
  return state_queue != nullptr &&
         xQueueReceive(state_queue, &state, 0) == pdTRUE;
}

bool netTakeMessage(NetMessage& message) {
  return message_queue != nullptr &&
         xQueueReceive(message_queue, &message, 0) == pdTRUE;
}
