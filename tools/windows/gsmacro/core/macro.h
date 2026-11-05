// Macro model.
//
// A macro is an ordered list of actions. `wait` advances time; every other
// action happens at the current offset. Compiling turns the list into events
// at ABSOLUTE offsets from the macro's start, so scheduling never accumulates
// drift: step N fires at start + offset[N], however late step N-1 ran.
//
// While a macro runs it owns a set of overrides layered over the physical
// input:
//   press     X   force button(s) on
//   release   X   stop forcing / suppressing button(s) (physical value shows again)
//   suppress  X   force button(s) off even if physically held
//   axis      LX v  override an axis (sticks -1..1, triggers 0..1)
//   axis-release LX  give the axis back to the physical stick
//   release-all    drop every override
//   wait      ms   advance time (fractional ms allowed)
// Every override is dropped automatically when the macro ends or is cancelled.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "clock.h"
#include "gamepad.h"

namespace gsm {

enum class Op : uint8_t { Press, Release, Suppress, SetAxis, ReleaseAxis, ReleaseAll, Wait };

struct Action {
    Op op = Op::Wait;
    uint16_t buttons = 0;   // Press / Release / Suppress
    Axis axis = AX_LX;      // SetAxis / ReleaseAxis
    double value = 0;       // SetAxis: normalised value
    double ms = 0;          // Wait

    static Action press(uint16_t b) { Action a; a.op = Op::Press; a.buttons = b; return a; }
    static Action release(uint16_t b) { Action a; a.op = Op::Release; a.buttons = b; return a; }
    static Action suppress(uint16_t b) { Action a; a.op = Op::Suppress; a.buttons = b; return a; }
    static Action setAxis(Axis ax, double v) { Action a; a.op = Op::SetAxis; a.axis = ax; a.value = v; return a; }
    static Action releaseAxis(Axis ax) { Action a; a.op = Op::ReleaseAxis; a.axis = ax; return a; }
    static Action releaseAll() { Action a; a.op = Op::ReleaseAll; return a; }
    static Action wait(double m) { Action a; a.op = Op::Wait; a.ms = m; return a; }
};

struct Macro {
    std::string name;
    std::vector<Action> actions;
};

// One compiled event: an action (never Wait) at an absolute offset.
struct MacroEvent {
    Time offset = 0;
    Op op = Op::Press;
    uint16_t buttons = 0;
    Axis axis = AX_LX;
    int raw = 0;              // SetAxis: raw XInput value
    int sourceIndex = 0;      // index into Macro::actions (for the GUI)
};

struct CompiledMacro {
    std::string name;
    std::vector<MacroEvent> events;  // sorted by offset (stable: source order at equal offsets)
    Time duration = 0;               // total of all waits; overrides drop at start + duration
    int id = 0;                      // which slot (0 left, 1 right) - informational
};

std::shared_ptr<const CompiledMacro> compileMacro(const Macro& m, int id);

// Human-readable problems that make a macro misbehave in a game, e.g. a press
// that is released at the same instant (zero-length, never visible).
std::vector<std::string> validateMacro(const Macro& m);

// Text form, one action per line (see header comment). Used by the config file
// and by the GUI's text editor.
std::string actionToText(const Action& a);
bool actionFromText(const std::string& line, Action* out, std::string* err);
std::string macroToText(const Macro& m);
// Parses action lines; '#' starts a comment. On error returns false and sets
// *err to "line N: ...".
bool macroFromText(const std::string& text, Macro* out, std::string* err);

}  // namespace gsm
