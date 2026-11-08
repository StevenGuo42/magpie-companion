#pragma once

// Just enough of Arduino for ui.cpp, app_state.cpp and LVGL to build on a PC.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t millis(void);

#ifdef __cplusplus
}

// LVGL includes this inside extern "C" (via lv_conf.h).
extern "C++" {
#include <stdio.h>

#include <string>
#include <type_traits>

class String {
 public:
  String(const char* text = "") : text_(text) {}
  template <typename T, typename = std::enable_if_t<std::is_integral_v<T>>>
  String(T value) : text_(std::to_string(value)) {}
  String(double value, unsigned int decimals) {
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    text_ = buffer;
  }
  const char* c_str() const { return text_.c_str(); }
  friend String operator+(const String& a, const String& b) {
    String out;
    out.text_ = a.text_ + b.text_;
    return out;
  }

 private:
  std::string text_;
};
}
#endif
