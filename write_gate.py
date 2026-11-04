"""Per-controller approval before the first configuration write.

Deadband's Windows support has not been confirmed on hardware by the project
yet, and two controllers can share one USB product id while differing inside
(the Tarantula Pro 8K and the Tarantula CE / T3CE both enumerate as 3537:103d).
So where `hostos.REQUIRES_WRITE_APPROVAL` is set (Windows by default),
configuration writes are LOCKED for each controller identity until its owner
explicitly approves them in the app.

Approving does two things, in order:

1. Takes a full backup of the controller (every profile bank, through the same
   code as Backup / Restore -> Export) to the app's data directory. Reading
   banks other than the active one means asking the pad to switch profile, so
   during this step -- and only during it -- the profile-select command
   (0x0F 0x07) is let through; the backup walk returns the pad to the profile
   it was on.
2. Only if the backup succeeded, records the identity as approved (persisted in
   write-approvals.json), which unlocks writes for that identity only.

An identity is (USB VID:PID, product string, firmware bcdDevice). A firmware
update, or a different model under the same PID, is a new identity and asks
again. Reads, the heartbeat and the profile/lighting queries are never gated:
`control.send_cmd(..., probe=True)` traffic bypasses this module entirely.

Dependency-free (no Qt), so control.py and the tests can import it.
"""

import json
import os
import threading
import time

import hostos

LOCKED, BACKING_UP, APPROVED = 'locked', 'backing-up', 'approved'
PROFILE_SELECT = (0x0F, 0x07)           # the only command allowed while backing up

_lock = threading.Lock()
_identity = None                         # dict for the bound controller, or None
_backing_up = False
_approved_cache = None                   # {key: record}, loaded lazily
_path_override = None                    # tests


def required():
    return hostos.REQUIRES_WRITE_APPROVAL


def store_path():
    if _path_override:
        return _path_override
    return os.path.join(hostos.user_data_dir(), 'deadband', 'write-approvals.json')


def backup_dir():
    return os.path.join(hostos.user_data_dir(), 'deadband', 'controller-backups')


def make_identity(vid, pid, product, bcd, model):
    """The identity dict for one controller as the reader sees it."""
    fw = f'{bcd >> 8:x}.{bcd & 0xff:02x}' if bcd else '?'
    return {'vid': vid, 'pid': pid, 'product': product or '', 'bcd': bcd or 0,
            'firmware': fw, 'model': model or 'not recognised',
            'platform': hostos.NAME}


def key(identity):
    if not identity:
        return ''
    return (f'{identity["platform"]}|{identity["vid"]:04x}:{identity["pid"]:04x}|'
            f'{identity["product"]}|{identity["bcd"]:04x}')


def describe(identity=None):
    """'GameSir 3537:103d "Product" firmware 2.49 (Tarantula Pro 8K map)'."""
    i = identity if identity is not None else _identity
    if not i:
        return ''
    product = f' "{i["product"]}"' if i['product'] else ''
    return (f'{i["vid"]:04x}:{i["pid"]:04x}{product}, firmware {i["firmware"]} '
            f'(driven with the {i["model"]} register map)')


# ------------------------------------------------------------------ persistence
def _load():
    global _approved_cache
    if _approved_cache is None:
        try:
            with open(store_path()) as fh:
                data = json.load(fh)
            _approved_cache = data.get('approved', {}) if isinstance(data, dict) else {}
        except (OSError, ValueError):
            _approved_cache = {}
    return _approved_cache


def _save():
    path = store_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp'
    with open(tmp, 'w') as fh:
        json.dump({'approved': _approved_cache or {}}, fh, indent=2)
    os.replace(tmp, path)


def reload():
    """Forget the in-memory copy (tests; or after the file was edited)."""
    global _approved_cache
    with _lock:
        _approved_cache = None


# ---------------------------------------------------------------- binding/state
def bind(identity):
    """Called by the reader whenever it starts driving a controller (None when
    nothing is driven). A change of identity cancels a backup in progress."""
    global _identity, _backing_up
    with _lock:
        if key(identity) != key(_identity):
            _backing_up = False
        _identity = identity


def identity():
    return _identity


def status():
    """LOCKED / BACKING_UP / APPROVED for the bound controller. APPROVED when
    approval isn't required on this platform."""
    if not required():
        return APPROVED
    with _lock:
        if _identity is None:
            return LOCKED
        if key(_identity) in _load():
            return APPROVED
        return BACKING_UP if _backing_up else LOCKED


def approval_record(identity=None):
    with _lock:
        return dict(_load().get(key(identity if identity is not None else _identity)) or {})


def allows(payload):
    """May this NON-probe command go to the device right now?"""
    st = status()
    if st == APPROVED:
        return True
    if st == BACKING_UP:
        return tuple(payload[:2]) == PROFILE_SELECT
    return False


# ------------------------------------------------------------------- approving
def begin_backup():
    """Enter the backup step for the bound identity. False if nothing is bound,
    it is already approved, or a backup is already running."""
    global _backing_up
    with _lock:
        if _identity is None or _backing_up or key(_identity) in _load():
            return False
        _backing_up = True
        return True


def end_backup():
    """Leave the backup step WITHOUT approving (a manual Export while locked:
    the user asked for the profile walk, not for writes)."""
    global _backing_up
    with _lock:
        _backing_up = False


def finish_backup(ok, backup_path, expect_identity):
    """End the backup step. On success -- and only if the SAME controller is
    still bound -- record the approval. Returns True when approved."""
    global _backing_up
    with _lock:
        _backing_up = False
        if not ok or key(expect_identity) != key(_identity) or _identity is None:
            return False
        _load()[key(_identity)] = {
            'identity': dict(_identity), 'backup': backup_path,
            'approved_at': time.strftime('%Y-%m-%d %H:%M:%S'),
        }
        try:
            _save()
        except OSError:
            # still approved for this session; it just asks again next launch
            pass
        return True


def revoke(identity=None):
    """Lock writes again for an identity (default: the bound one)."""
    with _lock:
        k = key(identity if identity is not None else _identity)
        if k in _load():
            del _approved_cache[k]
            try:
                _save()
            except OSError:
                pass
            return True
        return False
