// Read-only HidHide status: is the driver service present and running, and
// which configuration values exist. It never changes HidHide: hiding the pad
// and allow-listing gsmacro.exe stay manual steps in HidHide's own
// Configuration Client until the setup has been verified on hardware.
#pragma once
#include <string>

namespace gsm {

struct HidHideStatus {
    bool serviceFound = false;
    bool running = false;
    std::string detail;  // human-readable summary
};

HidHideStatus queryHidHide();

}  // namespace gsm
