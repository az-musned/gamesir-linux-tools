#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#endif

#include "test.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    // The engine tests run on the real clock with PortableWaiter and short
    // sleeps; at Windows' default ~15.6 ms tick those overshoot by ~10 ms.
    timeBeginPeriod(1);
#endif
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0, failedCases = 0;
    for (auto& c : t::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        t::caseFailed() = false;
        std::printf("[ RUN  ] %s\n", c.name);
        c.fn();
        std::printf("[ %s ] %s\n", t::caseFailed() ? "FAIL" : " OK ", c.name);
        run++;
        if (t::caseFailed()) failedCases++;
    }
    std::printf("\n%d test cases, %d passed, %d failed (%d failed checks)\n", run, run - failedCases, failedCases, t::failures());
    return failedCases ? 1 : 0;
}
