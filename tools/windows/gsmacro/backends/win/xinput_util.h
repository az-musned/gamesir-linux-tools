// Thin, read-mostly XInput access shared by the input source, the setup
// wizard and gsm_bench. Uses xinput1_4.dll's GetStateEx (ordinal 100, includes
// Guide) and GetCapabilitiesEx (ordinal 108, reports VID/PID).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../../core/gamepad.h"

namespace gsm {
namespace xi {

struct SlotInfo {
    int slot = 0;
    bool connected = false;
    uint16_t vid = 0, pid = 0;
};

bool load(std::string* err);
// false if the slot is empty / unavailable.
bool getState(int slot, uint32_t* packet, GamepadState* out);
bool setVibration(int slot, uint16_t large, uint16_t small);
std::vector<SlotInfo> slots();
// First slot with this VID (and PID if non-zero), or -1.
int findSlot(uint16_t vid, uint16_t pid = 0);

}  // namespace xi
}  // namespace gsm
