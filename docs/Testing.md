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
  directories.

The user-mode tests use directories under `%TEMP%\PathOverlayTests` and clean
that data before and after the run.

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
  and empty nested directories;
- occupied-file detection before commit/discard.

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
