"""Save management for the launcher (Riven): automatic backups, import and export of the 32 KiB SRAM save.

Contract: NOTES.md "Gerenciamento de save: plano e contrato"; QA: tests/test_save_qa.py.
No Windows calls here (pure standard library) so it is unit-tested; the dialogs live in launcher.py.

  backup_save(sav, backup_root, rom_sha1, rom_name, now=None) -> path | None
  import_save(src, sav, backup_root, rom_sha1, rom_name, now=None) -> (ok, message)
  export_save(sav, dst)
"""
import datetime as dt
import os
import shutil

SAVE_SIZE = 32768          # SRAM of this game (SRAM_V113): raw file, same format other emulators use
KEEP = 10                  # most recent backups kept per ROM


def backup_name(rom_name, now):
    return '%s_%s.sav' % (rom_name, now.strftime('%Y%m%d_%H%M%S'))


def _read(path):
    with open(path, 'rb') as f:
        return f.read()


def _write_atomic(path, data):
    """Write to `<path>.tmp` then swap with os.replace: the target is either the old or the new file, never half."""
    tmp = path + '.tmp'
    try:
        with open(tmp, 'wb') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def _list_backups(folder):
    try:
        return sorted(n for n in os.listdir(folder) if n.lower().endswith('.sav'))
    except OSError:
        return []


def backup_save(sav_path, backup_root, rom_sha1, rom_name, now=None):
    """Copy the current save into <backup_root>/<sha1>/ unless it equals the newest backup. Keeps the 10 newest.
    Returns the new backup's path, or None (no save, or nothing changed)."""
    if not os.path.isfile(sav_path):
        return None
    data = _read(sav_path)
    folder = os.path.join(backup_root, rom_sha1)
    os.makedirs(folder, exist_ok=True)
    names = _list_backups(folder)
    if names and _read(os.path.join(folder, names[-1])) == data:
        return None
    now = now or dt.datetime.now()
    name = backup_name(rom_name, now)
    n = 0
    while os.path.exists(os.path.join(folder, name)):          # another save inside the same second
        n += 1
        name = '%s_%s_%d.sav' % (rom_name, now.strftime('%Y%m%d_%H%M%S'), n)
    path = os.path.join(folder, name)
    _write_atomic(path, data)
    for old in _list_backups(folder)[:-KEEP]:                  # '.' < '_' keeps "same second" copies in order
        try:
            os.remove(os.path.join(folder, old))
        except OSError:
            pass
    return path


def validate(data):
    """('reject', why) | ('warn', why) | ('ok', '')"""
    if len(data) == 0:
        return 'reject', 'The file is empty.'
    if len(data) != SAVE_SIZE:
        return 'reject', ('Wrong size: %d bytes. A save for this game is exactly %d bytes (a raw .sav).'
                          % (len(data), SAVE_SIZE))
    if data == b'\xff' * SAVE_SIZE or data == b'\x00' * SAVE_SIZE:
        return 'warn', 'The imported save is empty (all 0xFF/0x00): the game will start from scratch.'
    return 'ok', ''


def import_save(src_path, sav_path, backup_root, rom_sha1, rom_name, now=None):
    """Validate, back up the current save, then replace it atomically. Nothing is touched on rejection."""
    try:
        data = _read(src_path)
    except OSError as e:
        return False, 'Could not read the file: %s' % e
    verdict, why = validate(data)
    if verdict == 'reject':
        return False, why
    try:
        os.makedirs(os.path.dirname(os.path.abspath(sav_path)), exist_ok=True)
        backup_save(sav_path, backup_root, rom_sha1, rom_name, now=now)
        _write_atomic(sav_path, data)
    except OSError as e:
        return False, 'Could not import the save (your current save was not changed): %s' % e
    return True, (why + ' ' if why else '') + 'Save imported. The previous one was backed up.'


def export_save(sav_path, dst_path):
    """Copy the save to `dst_path` (overwriting it). Raises OSError when there is no save."""
    if not os.path.isfile(sav_path):
        raise FileNotFoundError('there is no save file yet: ' + sav_path)
    _write_atomic(dst_path, _read(sav_path))
