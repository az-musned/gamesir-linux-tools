"""Read-only register dumps and diffs, for decoding settings GameSir's app has
that Deadband doesn't yet (e.g. a macro's loop / trigger mode).

The method needs no USB capture driver: dump the controller, change ONE option
in GameSir Connect, dump again, diff. Every byte that moved is a candidate for
that option, labelled by where it sits in the register map (which programmable
button's block, and at what offset).

    deadband --dump-registers before.json      # close Deadband + GameSir Connect first
    deadband --dump-registers after.json
    deadband --diff-registers before.json after.json

Strictly read-only: the dump sends only the heartbeat, the active-profile query
and register READS (0x0F 0x04). It never switches profile, so it covers the
ACTIVE profile's bank plus the global bank 0x20.
"""
import json
import time
from datetime import datetime

PROFILE_RANGE = (0x0000, 0x0800)      # past the Tarantula's last mapped field (0x077b)
GLOBAL_BANK, GLOBAL_RANGE = 0x20, (0x0000, 0x0100)
CHUNK = 48                            # < one reply's 56 data bytes
BLOCK_LEN = 0xa9                      # one programmable button's block

# Offsets inside a programmable button's block (docs/RESEARCH.md, Tarantula).
BLOCK_FIELDS = {0x00: 'remap enable', 0x01: 'remap target', 0x02: 'remap target 2',
                0x03: 'remap target 3', 0x04: 'remap target 4', 0x05: 'macro enable',
                0x06: 'UNKNOWN +6', 0x07: 'UNKNOWN +7', 0x08: 'macro step count'}


def label(prof, bank, addr):
    """Where `addr` sits in `prof`'s map, as text."""
    if bank == GLOBAL_BANK:
        return f'global bank 0x20 @0x{addr:04x}'
    for name, base in getattr(prof, 'MACRO_SLOTS', ()) or ():
        if base <= addr < base + BLOCK_LEN:
            off = addr - base
            if off in BLOCK_FIELDS:
                what = BLOCK_FIELDS[off]
            else:
                step, part = divmod(off - 0x09, 5)
                what = f'macro step {step + 1} ' + ('target', 'hold hi', 'hold lo',
                                                    'delay hi', 'delay lo')[part]
            return f'{name} block +0x{off:02x} ({what})'
    labels = prof.field_labels() if prof is not None else {}
    best = max((a for a in labels if a <= addr), default=None)
    if best is not None and addr - best < 12:
        return f'{labels[best]}' + (f' +{addr - best}' if addr != best else '')
    return f'0x{addr:04x} (unmapped)'


def _ranges(bank_profile):
    out = []
    for lo, hi in (PROFILE_RANGE,):
        out += [(bank_profile, a, min(CHUNK, hi - a)) for a in range(lo, hi, CHUNK)]
    lo, hi = GLOBAL_RANGE
    out += [(GLOBAL_BANK, a, min(CHUNK, hi - a)) for a in range(lo, hi, CHUNK)]
    return out


def dump(dev, prof, ask, wake, pace=0.02):
    """Read the active profile bank + global bank from an OPEN vendor handle.

    `ask(dev, cmd, kind, match=...)` and `wake(dev)` are the doctor's read helpers
    (passed in so this module stays transport-free). Returns the dump dict."""
    wake(dev)
    r = ask(dev, (0x0F, 0x0B), 0x0C)
    if not r:
        raise OSError('no active-profile reply (asleep, wrong mode, or another app has it?)')
    profile = r[2]
    bank = prof.profile_bank(profile)
    if bank is None:
        raise OSError(f'active profile {profile} is not one of this model\'s banks')
    data, missing = {}, []
    for b, addr, ln in _ranges(bank):
        rr = None
        for _try in range(3):                   # the pad drops some commands
            rr = ask(dev, (0x0F, 0x04, b, addr >> 8, addr & 0xFF, ln), 0x05,
                     match=lambda x, a=addr, bb=b: x[2] == bb and ((x[3] << 8) | x[4]) == a)
            if rr:
                break
            time.sleep(0.05)
        if rr:
            for i, v in enumerate(rr[6:6 + rr[5]]):
                data[f'{b:02x}:{addr + i:04x}'] = v
        else:
            missing.append(f'{b:02x}:{addr:04x}+{ln}')
        time.sleep(pace)
    return {'kind': 'deadband-register-dump', 'version': 1,
            'model': prof.name, 'profile': profile, 'bank': bank,
            'taken': datetime.now().isoformat(timespec='seconds'),
            'bytes': data, 'unread': missing}


def diff(a, b, prof):
    """[(bank, addr, old, new, label)] for every byte that differs."""
    if a.get('bank') != b.get('bank'):
        raise ValueError(f'the dumps are of different profiles ({a.get("profile")} vs '
                         f'{b.get("profile")}); switch the pad to the same profile')
    out = []
    for k in sorted(set(a['bytes']) | set(b['bytes'])):
        old, new = a['bytes'].get(k), b['bytes'].get(k)
        if old != new and old is not None and new is not None:
            bank, addr = (int(x, 16) for x in k.split(':'))
            out.append((bank, addr, old, new, label(prof, bank, addr)))
    return out


def format_diff(rows, a, b):
    L = [f'Register diff, profile {a.get("profile")} ({a.get("model")}): '
         f'{a.get("taken")} -> {b.get("taken")}']
    unread = sorted(set(a.get('unread', [])) | set(b.get('unread', [])))
    if unread:
        L.append(f'(not compared, unreadable: {", ".join(unread)})')
    if not rows:
        L.append('No byte changed. (Was the option saved to the pad, on this profile?)')
    for bank, addr, old, new, where in rows:
        L.append(f'  {bank:02x}:{addr:04x}  {old:02x} -> {new:02x}   {where}')
    return '\n'.join(L)


def save(d, path):
    with open(path, 'w') as fh:
        json.dump(d, fh, indent=1)


def load(path):
    with open(path) as fh:
        d = json.load(fh)
    if d.get('kind') != 'deadband-register-dump':
        raise ValueError(f'{path} is not a Deadband register dump')
    return d
