---
name: passport-setup
description: Prepare or diagnose the local ESP-IDF environment for FoloToy AI Passport when onboarding, tools are missing, or toolchain activation and dependency setup fail. Not for runtime firmware crashes.
---

<p align="right"><a href="SKILL.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Prepare the Development Environment

Resolve the target checkout and read its `AGENTS.md` and
`docs/development/engineering/environment-setup.md`. Paths refer to that
checkout. Preserve the user's chosen OS, installation paths, proxies, and
existing tools; do not replace another ESP-IDF installation.

## Inspect before changing anything

- Run `git status --short --branch`; identify OS, architecture, shell, and
  whether execution is local, WSL, a VM, container, or remote agent.
- Check the active `idf.py --version` and the selected installation. An inactive
  shell is not proof the toolchain needs reinstalling. Activate an existing
  ESP-IDF 5.5.3 in the current shell when available.
- Check the build prerequisites and dependency lock before downloading tools.
  Do not print complete environment variables or authentication configuration.
- Discover USB/serial access without opening ports when relevant to the user's
  request. Separate build readiness from access to the user's physical device.

## Repair the smallest demonstrated problem

Follow the environment guide's matching platform and download route. Explain
the proposed installation or configuration change and obtain the approvals
required by the environment before system-package installation, network access,
USB forwarding, group changes, or writes outside the workspace. Do not disable
TLS verification, antivirus, or other security controls to make setup pass.

Do not run destructive recovery recipes, remove tool caches/checkouts, or edit
global Git/shell settings as routine setup. Preserve partial/user installations;
if a repair would remove or replace material, identify exact targets and ask
first. After a retry, inspect the new evidence instead of repeating the same
failed operation indefinitely. If progress needs access or a user choice, stop
that operation and report the blocker; continue independent safe checks.

Require the selected environment to report ESP-IDF 5.5.3 before generating
configuration. Preserve intentional `sdkconfig` and partition changes. On
native Windows, use the documented ESP-IDF terminal; the repository's shell
gate also needs a compatible shell environment. Do not treat POSIX commands as
PowerShell commands or promise WSL automatically has USB access.

## Verify and hand off

For a setup-and-build request, run the existing gate in the activated
environment; prefer its isolated build rather than resetting the user's
configuration. For a diagnosis-only request, report the diagnosis and proposed
repair without installing or changing configuration. No flashing is implied.

Report the observed OS/shell, IDF version, build readiness, USB-access status,
and any remaining user action. If enumeration is possible but no device appears,
ask the user to power it on and connect it to computer USB using a data-capable
cable. If the environment cannot see local USB, explain that limitation instead.
Keep `Build`, `Host tests`, `Device tests`, and `Unverified` separate; do not mark
checks PASS just because the required command now exists.
