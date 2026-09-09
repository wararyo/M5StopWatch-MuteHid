"""PlatformIO targets for the guarded ota_1 write.

`pio run -t update` builds, then runs tools/device.py with the stored backup and
the detected port. `pio run -t backup` takes a fresh full backup.
"""
Import("env")

DEVICE = env.subst("$PROJECT_DIR/tools/device.py")

if env["PIOENV"] == "m5stopwatch-coexist":
    env.AddCustomTarget(
        name="update",
        dependencies="$BUILD_DIR/firmware.bin",
        actions=['"$PYTHONEXE" "%s" update --firmware "$BUILD_DIR/firmware.bin" --execute' % DEVICE],
        title="Update ota_1",
        description="Verify the stored backup, then write only ota_1",
    )
else:
    env.AddCustomTarget(
        name="update",
        dependencies=None,
        actions=['"$PYTHONEXE" -c "raise SystemExit(\'update writes ota_1: build the coexist environment\')"'],
        title="Update ota_1",
        description="Coexistence builds only",
    )

env.AddCustomTarget(
    name="backup",
    dependencies=None,
    actions=['"$PYTHONEXE" "%s" backup --execute' % DEVICE],
    title="Back up Flash",
    description="Read the whole 16 MB Flash into .pio/phase0/backups",
)
