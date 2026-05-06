# PathOverlay Testing

This project has three test layers: user-mode tests, service integration tests,
and driver end-to-end tests on a separate Windows test machine.

## User-Mode Tests

Run the default test suite from the repository root:

```powershell
.\scripts\test.ps1
```

Use `-Configuration Release` to run the release build output:

```powershell
.\scripts\test.ps1 -Configuration Release
```

`scripts/test.ps1` validates `task.json`, checks the expected project layout,
copies `sqlite3.dll` beside the test executable when needed, and runs
`PathOverlayTests.exe` if it has already been built. The test executable covers:

- path normalization, rule validation, and real-to-shadow path mapping;
- SQLite metadata initialization, rule persistence, change records, and commit
  log records;
- service-side overlay operations for copy-on-write, created files, tombstones,
  directory view preparation, commit, discard, conflict detection, occupied-file
  failure, backup creation, and metadata retention after failed commit;
- compatibility cases for long and Unicode paths, case variants, 8.3 short paths
  when available, read-only attributes, last-write timestamps, and empty nested
  directories that must be created only in shadow;
- source-child reparse passthrough for already-existing symlink or junction
  subtrees, where the subtree is not copied into shadow and does not create
  overlay metadata.

The user-mode tests use directories under `%TEMP%\PathOverlayTests` and clean
that data before and after the run.

## Stabilization Verification

Stabilization coverage is split between user-mode tests, service diagnostics,
and the driver E2E package.

User-mode tests must cover:

- interrupted commit/discard operation recovery metadata;
- persistent discard cleanup queue processing and failed cleanup reporting;
- `status` and `doctor` summaries without modifying metadata, shadow, or real
  source files;
- `diagnostics collect` smoke behavior when the service is unavailable;
- `changes --rule`, `commit --dry-run --rule`, and `discard --dry-run --rule`
  without mutating real source, shadow, or metadata;
- long paths, Unicode paths, case variants, 8.3 short paths when available,
  read-only attributes, last-write timestamps, empty nested directories, and
  source-child reparse passthrough.

For a local service diagnostic check without loading the driver, build Debug and
start the service with the packaged installer:

```powershell
.\scripts\build.ps1
cd .\x64\Debug
.\install-start.ps1 -SkipDriver -ResetData
.\pathoverlay.exe status
.\pathoverlay.exe doctor
.\pathoverlay.exe diagnostics collect --output "$env:TEMP\PathOverlayDiagnostics"
.\uninstall.ps1 -RemoveData
```

Expected diagnostic behavior:

- `status` reports service connectivity, driver connectivity, rule counts,
  pending changes, cleanup counts, and the most recent operation summary;
- `doctor` reports `no issues` on a clean store, or emits `WARN`/`ERROR` lines
  for recoverable operations, failed cleanup, missing source, invalid store, or
  missing shadow data;
- `WARN reparse passthrough` is expected when a rule source contains an
  already-existing junction, symlink, or mount point subtree; `ERROR missing
  shadow` is not expected for valid pending changes and must be investigated;
- `diagnostics collect` creates a directory containing command output files,
  SCM status, a manifest, and the service log when present;
- diagnostic collection must not copy real source file content or shadow file
  content into the package.

## Cleanup

To remove local user-mode and test-machine temporary data:

```powershell
.\scripts\cleanup-test-data.ps1
```

To also remove `%ProgramData%\PathOverlay`, run from an elevated PowerShell:

```powershell
.\scripts\cleanup-test-data.ps1 -IncludeServiceData
```

To unload and delete the installed test service and minifilter service as well,
run from an elevated PowerShell:

```powershell
.\scripts\cleanup-test-data.ps1 -IncludeDriverAndService
```

## Driver E2E Tests

Build and package a signed test-machine bundle from the development machine:

```powershell
.\scripts\package-test-machine.ps1 -Configuration Release
```

Copy the whole `test-machine-package\` directory to a Windows VM. The VM must
meet these requirements:

- Administrator permission for installation, driver load, and cleanup.
- Windows test-signing enabled, followed by a reboot:

```powershell
bcdedit /set testsigning on
```

- A Windows version compatible with the WDK used for the build.
- The packaged `PathOverlayTest.cer` certificate, or another trusted certificate
  used to sign `PathOverlayFlt.sys`.

On the VM, run:

```powershell
.\Run-PathOverlay-Test.cmd
```

The E2E script installs and loads `PathOverlayFlt`, verifies the communication
port, installs and starts `PathOverlaySvc`, verifies CLI IPC, and exercises:

- no-rule filesystem passthrough;
- rule creation, enable, disable, and source/store/service-process exclusions;
- copy-on-write modification and shadow read priority;
- new file creation in the overlay view;
- deletion tombstones and tombstone-aware open/query behavior;
- directory tombstones, tombstone-aware enumeration, and directory view merging
  without duplicate entries;
- discard without modifying real files;
- commit writeback, deletion, backup creation, rule restore, shadow cleanup, and
  metadata cleanup;
- file and directory rename/move isolation;
- commit/discard by rule id;
- path and attribute compatibility for long and Unicode paths, case variants,
  8.3 short paths when available, read-only attributes, last-write timestamps,
  empty nested directories, and source-child junction passthrough;
- occupied-file detection before commit/discard.

Compatibility expectations:

- Empty nested directory creation, such as `source\empty-root\nested\leaf`, must
  be visible through the overlay and materialized under the rule shadow store,
  but it must not pre-create `empty-root` or any child directory in the real
  source before commit.
- Long Unicode paths must write to shadow and remain valid for `doctor`; a
  valid pending change for such a path must not produce `ERROR missing shadow`.
- Source-child reparse passthrough covers reparse subtrees that already exist in
  the real source while the rule is enabled. The E2E script creates the junction
  while the rule is disabled, re-enables the rule, writes through the junction,
  and expects the write to reach the link target without creating shadow content
  or overlay metadata for the junction subtree.
- Creating a new junction or symlink inside an active overlay is outside the
  current supported virtualization boundary. Use a disabled rule or create the
  reparse point before adding/enabling the rule when that subtree must remain a
  real-source passthrough boundary.

The run is accepted only when the transcript ends with:

```text
PathOverlay test package passed all automated checks.
```

Each failing check prints rule id, source, store, relevant shadow or real paths,
and the observed command output when that context is available.

The script writes a transcript under `test-machine-package\logs\` and performs
service and driver cleanup unless `-SkipCleanup` is used.
On failure, it also runs `pathoverlay diagnostics collect --output <logs path>`
and prints the diagnostics directory path.

For stabilization acceptance, inspect a failed E2E run before cleanup if
`-SkipCleanup` was used, or inspect the printed diagnostics directory otherwise.
The diagnostics directory should include `rule-show.txt`, `changes.txt`,
`status.txt`, `doctor.txt`, `driver-status.txt`, SCM state files,
`manifest.txt`, and `PathOverlaySvc.log` when the service log exists. The
transcript should show the failing check context and the diagnostics path.
