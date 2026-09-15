"""PlatformIO targets for the guarded app-slot write.

`pio run -t update` builds, then runs tools/device.py with the stored backup and
the detected port. `pio run -t backup` takes a fresh full backup.
"""
Import("env")

SLOT = int(env.GetProjectOption("custom_app_slot", "1"))
if SLOT not in (1, 2, 3):
    raise ValueError("custom_app_slot must be 1, 2, or 3")

DEVICE = env.subst("$PROJECT_DIR/tools/device.py")

if env["PIOENV"] == "m5stopwatch-coexist":
    env.AddCustomTarget(
        name="update",
        dependencies="$BUILD_DIR/firmware.bin",
        actions=['"$PYTHONEXE" "%s" update --slot %d --firmware "$BUILD_DIR/firmware.bin" --execute' % (DEVICE, SLOT)],
        title="Update app slot",
        description="Verify the stored backup, then write only the selected app slot",
    )
else:
    env.AddCustomTarget(
        name="update",
        dependencies=None,
        actions=['"$PYTHONEXE" -c "raise SystemExit(\'update writes an app slot: build the coexist environment\')"'],
        title="Update app slot",
        description="Coexistence builds only",
    )

env.AddCustomTarget(
    name="backup",
    dependencies=None,
    actions=['"$PYTHONEXE" "%s" backup --execute' % DEVICE],
    title="Back up Flash",
    description="Read the whole 16 MB Flash into .pio/phase0/backups",
)
