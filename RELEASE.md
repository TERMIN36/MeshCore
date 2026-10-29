# Releasing Firmware

GitHub Actions is set up to automatically build and release firmware.

## Combined release (`v*` tags)

Push a tag like `v0.2.0` or `v0.2.0-rc12`. The Release workflow builds companion, repeater and room-server, then publishes **three zip bundles** (one per role) to avoid GitHub rate limits from hundreds of individual assets:

**Device-facing version string** (max 20 characters, see `build.sh`):

| Git tag | `FIRMWARE_VERSION` in firmware |
|--------|----------------------------------|
| `v0.2.0` (stable) | `termin36-v0.2.0` |
| `v0.2.0-rc12`, `-beta`, `-alpha` | `t36-v0.2.0-rc12` |

The splash shows the same device-facing string (without commit hash): `t36-v0.2.0-rc12` or `termin36-v0.2.0`.

- `companion-<tag>.zip`
- `repeater-<tag>.zip`
- `room-server-<tag>.zip`

Inside each zip are the per-board `.bin` / `-merged.bin` / `.uf2` / DFU `.zip` files.

## Per-role draft tags

These still build a single role and create a **draft** release with loose files:

- `companion-v1.0.0`
- `repeater-v1.0.0`
- `room-server-v1.0.0`

> NOTE: replace `v1.0.0` with the version you want to release as.

- You can push one, or more tags on the same commit, and they will all build separately.
- Once the firmware has been built, a new (draft) GitHub Release will be created.
- You will need to update the release notes, and publish it.
