#pragma once

// Host-only adapter for the Arduino surface used by Settings.cpp. The real
// ArduinoJson parser/serializer is included unchanged by the tests.
#include <stddef.h>
#include <stdint.h>
#include <cstring>
#include <string>

using String = std::string;
#define F(text) text
constexpr uint8_t LOW = 0;
constexpr uint8_t HIGH = 1;
