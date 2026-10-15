<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Skills

These skills turn the repository's engineering guidance into focused AI workflows.
Start with `passport-develop` for application development; use the specialized
skills for a single task. They reference the existing documentation and validation
gate, rather than defining another set of hardware rules or build commands.

Each skill must contain at least `SKILL.md` with YAML frontmatter defining `name` and a trigger-focused `description`. Complex skills may add `references/`, `scripts/`, and `assets/`. Keep documentation as plain Markdown and register every added skill in this index.

## Current skills

| Skill | What it does |
| --- | --- |
| [passport-develop](passport-develop/SKILL.md) | Turn a feature request into an application, tests, and a verified delivery; coordinate the other core skills when needed. |
| [passport-setup](passport-setup/SKILL.md) | Inspect and prepare the ESP-IDF 5.5.3 environment without replacing other installations or assuming USB access. |
| [passport-build](passport-build/SKILL.md) | Run the shared gate and retain a verified merged image with matching ELF/MAP artifacts. |
| [passport-device-test](passport-device-test/SKILL.md) | After authorization, identify the target, flash the chosen firmware, and distinguish logs from physical acceptance. |
| [passport-debug](passport-debug/SKILL.md) | Diagnose crashes, memory problems, blank Chinese text, and peripheral failures using evidence and matching symbols. |
| [issue-suggestions](issue-suggestions/SKILL.md) | After a release, collect the releasing developer's own improvement points and file them as feature request issues against the upstream project. |
| [experience-pr](experience-pr/SKILL.md) | After a release, collect reusable development experience and submit it as a documentation pull request. |
| [plays-archive](plays-archive/SKILL.md) | After a release, archive the published application under the repository-relative `docs/reference/<username>/<app-name>/` with an AI-generated bilingual summary; record cover metadata only, without committing the image. |

## Required preparation, handled by the AI

The five `passport-*` skills above are required, not optional recommendations.
Follow the [required-skill policy](../AGENTS.md#required-ai-skills): the AI checks
availability before development and installs any missing skills itself. It
chooses the method appropriate to the active AI tool, operating system, and
permissions; the user does not need to supply installation instructions.

Use the maintained sources in this repository's `skills/` directory. The
repository installer is an available helper, not a mandatory installation
method. Do not assume that every AI tool has the same discovery conventions.
Consult the active tool's documentation when needed; for Codex, see the
[official skills guide](https://learn.chatgpt.com/docs/build-skills).

Verify that the active AI environment can actually discover/read the installed
skills; creating files alone does not prove availability. If installation or
activation is blocked, report the cause and the minimum assistance needed.
Reading source instructions directly may support work while resolving the
blocker, but must not be reported as successful installation. Preserve existing
skills/configuration and obtain any permissions required by the environment.

## Example requests

```text
Use $passport-develop to build an offline Pomodoro app with a Chinese UI.
Use $passport-setup to check my development environment; do not reinstall anything yet.
Use $passport-build to package this firmware without flashing it.
Use $passport-device-test to test the verified firmware while preserving my settings.
Use $passport-debug to explain this crash log; do not change code yet.
```

Natural-language requests can also select skills through their descriptions.
Installing a skill does not grant USB, network, Git, or publishing permission.
An unavailable device does not block coding or host tests. Building does not
authorize flashing; diagnosing does not authorize changes. After a completed
firmware implementation the AI should invite device testing, not silently run it.

## Maintain and validate

Keep one skill per directory, paired English/Chinese instructions, precise
trigger descriptions, and links in this index. Put deterministic operations in
reviewable tools and leave project policy in its authoritative document.
Follow [skill validation](validation.md) for routing and boundary scenarios.
The shared static gate tests the installer and archive tool; the full gate also
builds and checks a real debug archive. Neither replaces actual device testing.
