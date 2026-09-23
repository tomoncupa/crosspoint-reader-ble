#pragma once

// The ESP32-C3 has one radio: Bluetooth and WiFi cannot both be on. Any screen that
// brings WiFi up calls this first, so a remote left connected does not break WiFi.
void releaseRadioForWifi();
