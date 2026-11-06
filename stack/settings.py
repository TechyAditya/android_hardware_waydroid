#!/usr/bin/env python3
"""Sets or restores the waydroid.cfg settings that the all-fixes stack is tested with.

  settings.py install CFG BACKUP    sets each SETTINGS value that BACKUP doesn't list yet, and saves
                                    the value it replaces there
  settings.py uninstall CFG BACKUP  puts the saved values back and removes the ones it added

Prints what it changed. Waydroid's configparser lowercases keys, so every key here is lowercase.
"""
import configparser
import json
import os
import sys

SETTINGS = {
    "waydroid": {
        # A frozen container stops apps in their own windows and the media bridge.
        "suspend_action": "none",
    },
    "properties": {
        # The composer's default, written so that an earlier `true` doesn't stay.
        "persist.waydroid.multi_windows": "false",
        # Reconnect the main display at most every 150 ms while its window is resized (#92).
        "persist.waydroid.resize_interval_ms": "150",
        # Android's Codec2 codecs, which screen recording and scrcpy encode with. Waydroid sets 0
        # when it falls back to software rendering, as it does on NVIDIA.
        "debug.stagefright.ccodec": "4",
    },
}


def main(mode, cfg_path, backup_path):
    cfg = configparser.ConfigParser()
    cfg.read(cfg_path)
    saved = {}
    if os.path.exists(backup_path):
        with open(backup_path) as f:
            saved = json.load(f)

    changed = []
    for section, values in SETTINGS.items():
        if not cfg.has_section(section):
            cfg.add_section(section)
        for key, value in values.items():
            name = f"{section}.{key}"
            current = cfg[section].get(key)
            if mode == "install":
                # A setting is written once, so a later install keeps the user's own value.
                if name in saved:
                    continue
                saved[name] = current
                target = value
            else:
                if name not in saved:
                    continue
                target = saved[name]
            if current == target:
                continue
            if target is None:
                cfg.remove_option(section, key)
            else:
                cfg[section][key] = target
            changed.append(f"{key} = {target}" if target is not None else f"{key} removed")
        if mode == "uninstall" and not cfg.items(section, raw=True):
            cfg.remove_section(section)

    if changed:
        with open(cfg_path, "w") as f:
            cfg.write(f)
    if mode == "install":
        with open(backup_path, "w") as f:
            json.dump(saved, f, indent=1)
    print(("set " if mode == "install" else "restored ") + ", ".join(changed)
          if changed else "Waydroid settings unchanged")


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("install", "uninstall"):
        sys.exit(__doc__)
    main(*sys.argv[1:])
