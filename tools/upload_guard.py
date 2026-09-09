Import("env")

if any(target in COMMAND_LINE_TARGETS for target in ("upload", "uploadfs", "erase")):
    raise RuntimeError("Phase 0: use tools/device.py after checking/backing up the existing Flash; normal upload/erase is disabled")

