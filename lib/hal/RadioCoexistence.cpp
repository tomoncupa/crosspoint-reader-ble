#include "RadioCoexistence.h"

#include <Logging.h>

#include "BluetoothHIDManager.h"

void releaseRadioForWifi() {
  auto& bt = BluetoothHIDManager::getInstance();
  if (!bt.isEnabled()) {
    return;
  }
  LOG_INF("BT", "Turning Bluetooth off so WiFi can use the radio");
  bt.disable();
}
