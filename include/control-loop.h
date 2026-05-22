#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

// Public entry points for the host-protocol control loop. The
// implementations live in control-loop.cpp and orchestrate
// connectAndReport, emitDevList, and the per-command handlers. main.cpp
// just wires these into the FreeRTOS tasks and the Arduino loop().

// Dispatch one host→co command frame. Called from uartHandler's
// FramedMsgPackReceiver callback. Ack and follow-up event emission
// (T_DEV_LIST, etc.) are the dispatcher's responsibility.
void dispatchHostCommand(JsonVariantConst root);

// Sequentially attempt to reconnect every slot flagged with a saved
// NVS device name. Triggered once from loop() after the host's first
// C_SET_PERIOD; no-op on subsequent calls until startAutoConnect is
// re-armed.
void autoConnectDevice();

// Poll the BOOT button and dispatch press / long-press semantics.
// Short press → connect first-free slot via proximity scan.
// Long press (≥ BUTTON_LONG_PRESS_MS) → disconnect every active slot.
void buttonHandler();
