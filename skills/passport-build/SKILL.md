---
name: passport-build
description: Validate and package FoloToy AI Passport firmware, including a verified merged image and matching debugging artifacts. Use for build, test, and firmware-package requests; does not flash, commit, or publish.
---

<p align="right"><a href="SKILL.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Build and Package Firmware

Resolve the target checkout, run `git status --short --branch`, and read
`AGENTS.md`, `docs/development/engineering/build-and-test.md`, and
`docs/development/engineering/firmware-layout.md` there. All commands below run
from that checkout; never infer a developer-specific IDF path.

## Build the requested configuration

1. Confirm ESP-IDF 5.5.3 is activated. If missing, use `passport-setup` when
   available or its environment-setup document. Obtain required network/system
   permissions rather than bypassing them.
2. Check tracked defaults, dependencies, and partitions against the requested
   application. The gate uses isolated configuration from `sdkconfig.defaults`,
   not the ignored root `sdkconfig`. If the user wants a local-only setting in
   the delivered firmware, resolve that mismatch explicitly; do not silently
   overwrite configuration or build a different variant.
3. Run the smallest relevant checks while iterating and `./tools/validate.sh`
   before final delivery. This is the existing gate, not a replacement build
   pipeline. Custom valid partition layouts are allowed. On a requested
   build-only check, `./tools/validate.sh --firmware` is available; identify any
   omitted host tests in the report.
4. A failed run does not validate an old file still present under `build/`.
   Correct in-scope failures or report the blocker. Never deliver a stale
   artifact as the result of the failed build.

## Identify the exact deliverable

The gate preserves `build/FoloToy-AI-Passport-full.bin` and a content-addressed
bundle under `build/firmware/<full-bin-sha256>/`. Verify the bundle before
handoff with:

```text
python3 tools/archive_firmware.py verify <archive-directory>
```

Report the actual archive path, full-image hash, and matching ELF identity from
`manifest.json`. The archive retains ELF, MAP, merged, application, bootloader,
and partition-table images, and `flash_args`. Extra custom partition images are
not retained separately; obtain them if needed for segmented flashing. A repeat
archive of identical firmware reuses its first verified MAP as documented in the
build guide. Do not substitute an ELF from a later rebuild,
especially for a dirty checkout. The recorded hashes bind artifacts together;
they are not a claim of hardware acceptance or cryptographic trust in a vendor.

Deliver the verified merged `full.bin` for flashing from `0x0`, never the
application-only `.bin` at that offset. Explain that a merged flash can reset
stored data; keeping settings may require a compatible segmented-flash workflow
under the firmware-layout policy. No original-firmware backup is required, but
that does not authorize a full-chip erase.

Keep generated binaries and debugging bundles out of commits. Firmware/ELF can
contain embedded application secrets; do not upload debugging files or publish
an artifact without checking the content and obtaining authorization.

## Report

Separate `Build`, `Host tests`, `Device tests`, and `Unverified`, with the actual
checks and any configuration differences. After a completed firmware change,
offer device testing per the AI guide. Building or packaging alone does not
authorize USB flashing, Git writes, or a release.
