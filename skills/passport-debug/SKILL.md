---
name: passport-debug
description: Diagnose FoloToy AI Passport runtime failures from logs and symptoms such as reboot, stack fault, watchdog, blank text, black screen, or missing audio. Implement a fix only when requested; not for toolchain installation problems.
---

<p align="right"><a href="SKILL.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Diagnose Firmware Failures

Locate the affected checkout and read `AGENTS.md`. Start with
`git status --short --branch`. Determine whether the user wants an explanation
or a fix: diagnosis does not authorize edits, rebuilding, flashing, or pushing.
The supplied log may come from another firmware; the current checkout is not
automatically its source.

## Establish the evidence

- Record the symptom, reproducible user action, first relevant error, reset/
  exception type, task, firmware version, and ELF SHA shown in the log. Request
  only missing information that would change the conclusion; do not request
  credentials, raw device QR content, or a whole private environment dump.
- Distinguish observation, likely cause, and unverified hypothesis. A stack
  pointer outside the reported task bounds is direct evidence of a stack fault;
  the exact allocating/calling function still requires matching symbols.
- For the current application's code, inspect the relevant headers and
  implementation. Use `docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md`
  for the matching symptom, not a generic ESP32 board's pins or assumptions.

## Match symbols before decoding addresses

For a saved gate build, verify its bundle with
`python3 tools/archive_firmware.py verify <archive-directory>`. Compare the
panic/boot ELF identity with `app_elf_sha256` in its manifest. A truncated log
hash must match a unique candidate; ask for the complete identity if ambiguous.
For other builds, obtain the ELF that produced the flashed image and evidence
of its identity. A similar Git version or a later rebuild is not a substitute.

Only after matching, use the activated toolchain's
`riscv32-esp-elf-addr2line -pfiaC -e <matching.elf> <addresses>` for the panic PC,
return address, and credible trace frames. Raw stack words are not all return
addresses. If symbols are unavailable, explain what the log itself establishes
and request the matching build artifacts; do not invent file/line locations or
rebuild the current source and call it the old firmware.

## Narrow the cause

Check the relevant path: task stack/local buffers and call depth; allocation
failures and largest suitable heap block; UI threading/font/style selection;
peripheral initialization; or tasks/callbacks surviving page exit. For Chinese
text read `docs/development/engineering/lvgl-chinese-fonts.md`. Do not fix missing
glyphs by hiding placeholders, disable watchdog/stack protection, or increase
every buffer/stack without examining the no-PSRAM memory budget.

An added diagnostic probe, recompilation, or hardware experiment is a proposed
next step for diagnosis-only requests, not permission to perform a repair. When
the user requests a fix, make the smallest evidenced correction, add a relevant
regression test, and follow the normal build/device-test handoff. Separate
synthetic/host checks from physical results.

## Return a usable conclusion

State: the observed failure; the supported cause and confidence; what remains
unknown; and the smallest next check or authorized fix. If a fix was made,
report `Build`, `Host tests`, `Device tests`, and `Unverified`. Stop when progress
requires the actual firmware, device access, or a new user decision rather than
changing unrelated code or repeatedly trying uninformative experiments.
