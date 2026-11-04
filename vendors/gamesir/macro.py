"""GameSir per-paddle macro encoding (bank = active profile).

Each back paddle (Cyclone: L4/R4; 8K: L4/R4/L5/R5) can hold a macro — a sequence
of button/key events played when the paddle is pressed. It lives inside the
paddle's register block, relative to the paddle's REMAP_SLOTS base address:
  base-0x02 : turbo on, base-0x01 : turbo rate (Hz) -- the block really starts here
  base+0x00 : remap enable, +0x01..+0x03 : up to 3 targets, +0x04 : toggle
  base+0x05 : FLAGS -- bit 0 macro enabled, bit 1 "hold to activate" (Long Press;
              clear = Single Press), bit 2 loop
  base+0x06 : loop interval, ms (BE16): pause after the last step before repeating
  base+0x08 : event count
  base+0x09 : events, 5 bytes each = [target_code, hold_ms(BE16), delay_ms(BE16)],
              except the block's LAST slot, which has room for [target, hold] only

The flags, loop interval and block geometry come from GameSir Connect 1.16.7's own
profile codec, extracted and hardware-verified on a Cyclone 2 by Pedro Segato's
gamesir-reverse-engineering project (MIT; docs/protocolo.md there). Their block
offsets are this module's +2, and they line up with everything found here
independently: remap enable +0, macro enable +5, count +8, steps +9, and the
8K's "setting byte" at +0xa8 (#51), which is the NEXT block's turbo rate.

Firmware behaviour (verified there on a Cyclone 2): Single Press -- one press
plays the whole sequence and another press stops it (presses never overlap); with
loop, it repeats until the next press. Hold -- plays while held and stops the
moment it's released; with loop, repeats while held.
`target_code` reuses the remap target table (A=0x09 ..) plus keyboard codes
(0x28+). hold = how long the input is held; delay = pause after, before the next
event. Reverse-engineered from USBPcap (caps 12_full_macro, 15c_macro_timing).

DATA ONLY (encode/decode). The bridge does the reads/writes so it can pin the
write-style + session generation like every other vendor write. The whole macro
also shares the paddle block with the remap record, so enabling a macro and
remapping the same paddle are mutually exclusive in the UI.
"""
import vendors.gamesir.config as cfg

ENABLE_OFF = 0x05         # the flags byte (bit 0 = enabled)
FLAGS_OFF  = ENABLE_OFF
LOOP_MS_OFF = 0x06        # BE16
COUNT_OFF  = 0x08
EVENTS_OFF = 0x09
EVENT_LEN  = 5
DEFAULT_MS = 100          # app default hold/delay per event
SHORT_LEN  = 3            # the block's final slot: [target, hold] (no delay)

FLAG_ENABLED, FLAG_HOLD, FLAG_LOOP = 0x01, 0x02, 0x04
BLOCK_HEAD = 2 + EVENTS_OFF   # turbo bytes before the base + header up to the steps


def region_len(block_len, max_events):
    """Bytes the step region may use. `block_len` is the paddle block's full
    length (the stride between paddle bases; 0 = unknown). Without it, the old
    max_events x 5 (callers that don't know the block)."""
    if block_len:
        return block_len - BLOCK_HEAD
    return max_events * EVENT_LEN


def region_for(prof):
    """The step region for a controller profile: its paddle blocks are laid end
    to end, so the stride between paddle bases IS the block length. A profile
    with a single paddle gives no stride: fall back to max_events x 5."""
    bases = sorted(b for _n, b in prof.MACRO_SLOTS)
    strides = {b - a for a, b in zip(bases, bases[1:])}
    block = strides.pop() if len(strides) == 1 else 0
    return region_len(block, prof.macro_max)


def slot_sizes(region, max_events):
    """Byte size of each step slot that fits `region`: 5, and a trailing 3 when
    only [target, hold] still fits. At most max_events slots."""
    out, used = [], 0
    while len(out) < max_events:
        if used + EVENT_LEN <= region:
            out.append(EVENT_LEN)
        elif used + SHORT_LEN <= region:
            out.append(SHORT_LEN)
        else:
            break
        used += out[-1]
    return out

# Max steps is paddle-block dependent (8K fits 32, Cyclone 30) — passed in per
# controller. A single read reply only carries ~55 B, so the event region is
# read in chunks. MAX_EVENTS is just a safe default when no max is supplied.
MAX_EVENTS = 32
READ_CHUNK = 50          # bytes per event-region read (< one reply's ~55 B)

# Buttons pickable as macro event targets (gamepad targets, minus the Disabled
# sentinel). Keyboard/mouse targets are a later addition.
TARGETS = [(n, c) for n, c in cfg.REMAP_TARGETS if c != 0xff]


def _event_chunks(base, max_events, region=None):
    """(addr, length) reads covering the event region, split to fit one reply."""
    out, off = [], 0
    total = region if region is not None else max_events * EVENT_LEN
    while off < total:
        ln = min(READ_CHUNK, total - off)
        out.append((base + EVENTS_OFF + off, ln))
        off += ln
    return out


def read_addrs(base, max_events=MAX_EVENTS, region=None):
    """(addr, length) reads for one paddle's macro: flags + loop interval, count,
    event region. (Flags and interval are one 3-byte read.)"""
    return ([(base + FLAGS_OFF, 3), (base + COUNT_OFF, 1)]
            + _event_chunks(base, max_events, region))


def target_index(code):
    for i, (_n, c) in enumerate(TARGETS):
        if c == code:
            return i
    return 0


def decode(vals, base, max_events=MAX_EVENTS, region=None):
    """A paddle's macro state {enable, hold, loop, loop_ms, flags,
    events:[{target,hold,delay}]}. `flags` keeps the raw byte, so bits not
    understood here survive an edit. (A 1-byte flags read -- an older backup --
    decodes with loop_ms 0.)"""
    head = list(vals[base + FLAGS_OFF])
    flags = head[0]
    loop_ms = (head[1] << 8) | head[2] if len(head) >= 3 else 0
    count = min(vals[base + COUNT_OFF][0], max_events)
    raw = []
    for addr, _ln in _event_chunks(base, max_events, region):
        raw += list(vals[addr])
    sizes = slot_sizes(len(raw), max_events)
    events, o = [], 0
    for size in sizes[:count]:
        if o + size > len(raw):
            break
        events.append({'target': raw[o],
                       'hold':  (raw[o + 1] << 8) | raw[o + 2],
                       'delay': ((raw[o + 3] << 8) | raw[o + 4]) if size == EVENT_LEN else 0})
        o += size
    return {'enable': bool(flags & FLAG_ENABLED), 'hold': bool(flags & FLAG_HOLD),
            'loop': bool(flags & FLAG_LOOP), 'loop_ms': loop_ms, 'flags': flags,
            'events': events}


def flags_byte(old, enable=None, hold=None, loop=None):
    """The flags byte with the given bits changed and every other bit kept."""
    out = old & 0xff
    for value, bit in ((enable, FLAG_ENABLED), (hold, FLAG_HOLD), (loop, FLAG_LOOP)):
        if value is not None:
            out = (out | bit) if value else (out & ~bit)
    return out


def head_bytes(flags, loop_ms):
    """[flags, loop_ms hi, loop_ms lo] -- written as one block at base+FLAGS_OFF."""
    ms = max(0, min(0xffff, int(loop_ms)))
    return [flags & 0xff, (ms >> 8) & 0xff, ms & 0xff]


def _event_bytes(ev):
    tgt = int(ev.get('target', TARGETS[0][1])) & 0xff
    hold = max(0, min(0xffff, int(ev.get('hold', DEFAULT_MS))))
    delay = max(0, min(0xffff, int(ev.get('delay', DEFAULT_MS))))
    return [tgt, (hold >> 8) & 0xff, hold & 0xff, (delay >> 8) & 0xff, delay & 0xff]


def block_bytes(events, max_events=MAX_EVENTS, region=None):
    """[count, event0, event1, ..., zeros] to write at base+COUNT_OFF.

    Zero-filled to the whole step region: the firmware plays on past `count`
    until it reaches an empty step, so a macro shortened in place kept playing
    the old macro's leftover steps (#51, verified on an 8K). With `region` (the
    block's real room, see region_len) the write ends exactly at the block's
    end: an event in the final, short slot is stored without its delay, and
    nothing past the block -- the next button's turbo bytes -- is touched."""
    total = region if region is not None else max_events * EVENT_LEN
    sizes = slot_sizes(total, max_events)
    evs = list(events)[:len(sizes)]
    out = [len(evs)]
    for ev, size in zip(evs, sizes):
        out += _event_bytes(ev)[:size]
    return out + [0] * (1 + total - len(out))
