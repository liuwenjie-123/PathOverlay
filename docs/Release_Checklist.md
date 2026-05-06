# PathOverlay Release Checklist

This checklist is for stabilization-phase release candidates. It assumes the
current release scope is still a prototype, not a production sandbox.

## Scope

- Confirm `task.json` has no release-blocking task left in `pending`,
  `in_progress`, or `blocked` unless the release notes explicitly call it out.
- Review `README.md`, `docs/Testing.md`, and
  `docs/Stabilization_and_Recovery_Plan.md` for command and limitation drift.
- Confirm the release notes do not promise whole-drive overlays, automatic
  repair, automatic restore, complete rollback, registry virtualization, or a
  security sandbox boundary.

## Build

From the repository root:

```powershell
.\scripts\build.ps1
.\scripts\build.ps1 -Configuration Release
```

Check that the expected outputs exist under `x64\Debug` and `x64\Release`:

- `PathOverlayFlt.sys`
- `PathOverlaySvc.exe`
- `pathoverlay.exe`
- `sqlite3.dll`
- `Install-Start-PathOverlay.cmd`
- `Uninstall-PathOverlay.cmd`

## User-Mode Tests

Run:

```powershell
.\scripts\test.ps1
.\scripts\test.ps1 -Configuration Release
```

Confirm the tests cover:

- metadata initialization and migration compatibility;
- rule validation and real-to-shadow path mapping;
- copy-on-write, created files, tombstones, rename/move, commit, discard, and
  backup creation;
- operation recovery and cleanup queue processing;
- `status`, `doctor`, diagnostics collection smoke behavior, `changes --rule`,
  and commit/discard dry-run behavior;
- long paths, Unicode paths, case variants, 8.3 short paths when available,
  read-only attributes, timestamps, empty nested directories, and source-child
  reparse passthrough.

## Service Diagnostics

Run a local service-only diagnostic pass when a driver load is not required:

```powershell
.\scripts\build.ps1
cd .\x64\Debug
.\install-start.ps1 -SkipDriver -ResetData
.\pathoverlay.exe status
.\pathoverlay.exe doctor
.\pathoverlay.exe diagnostics collect --output "$env:TEMP\PathOverlayDiagnostics"
.\uninstall.ps1 -RemoveData
```

Confirm:

- `status` reports service, driver, rule, pending change, cleanup, and operation
  state;
- `doctor` is read-only and reports no issues on a clean store;
- the diagnostics directory contains command outputs, SCM state, manifest, and
  service log when present;
- the diagnostics directory does not include real source file content or shadow
  file content.

## Driver E2E

Package the test-machine bundle:

```powershell
.\scripts\package-test-machine.ps1 -Configuration Release
```

On a Windows test machine with administrator permission, test-signing enabled,
and a reboot completed after `bcdedit /set testsigning on`, run:

```powershell
.\Run-PathOverlay-Test.cmd
```

Accept the E2E run only when the transcript ends with:

```text
PathOverlay test package passed all automated checks.
```

Confirm the E2E run covers:

- driver load and communication port connection;
- service install/start and CLI IPC;
- rule add/show/enable/disable/delete behavior;
- no-rule passthrough and source/store/service exclusions;
- copy-on-write, shadow read priority, tombstones, directory enumeration,
  rename/move, commit, discard, backup creation, and cleanup;
- rule-scoped `changes`, commit/discard dry-run, and occupied-file detection;
- compatibility paths and attributes listed in `docs/Testing.md`;
- empty nested directory creation is isolated to shadow and does not pre-create
  real source directories;
- source-child junction or symlink passthrough for already-existing reparse
  subtrees does not create changes, shadow content, commit work, or discard
  work; active-overlay creation of new junctions/symlinks remains outside the
  supported virtualization boundary;
- failure diagnostics path printing when an assertion fails.

## Cleanup

After local or test-machine validation, run the narrowest cleanup that matches
the environment:

```powershell
.\scripts\cleanup-test-data.ps1
.\scripts\cleanup-test-data.ps1 -IncludeServiceData
.\scripts\cleanup-test-data.ps1 -IncludeDriverAndService
```

For packaged output directories, also verify:

```powershell
.\Uninstall-PathOverlay.cmd
```

Confirm:

- `PathOverlaySvc` is stopped and uninstalled;
- `PathOverlayFlt` is unloaded and deleted;
- `%ProgramData%\PathOverlay` is removed when data cleanup was requested;
- temporary source, store, and package log directories are either intentionally
  retained for investigation or removed.

## Final Review

- Run `git diff --check`.
- Re-run `.\scripts\test.ps1` after documentation or script changes.
- Confirm `task.json` parses as JSON.
- Confirm `docs/task-notes.md` records the release validation outcome or any
  explicit environment limitation.
- Commit only release-related changes.
