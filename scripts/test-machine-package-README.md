# PathOverlay Test Machine Package

Run `Run-PathOverlay-Test.cmd` on the test machine.

The script requests administrator elevation, checks Windows test-signing, imports
`PathOverlayTest.cer`, installs and loads `PathOverlayFlt`, installs and starts
`PathOverlaySvc`, then runs automated checks.

Logs are written to `logs\PathOverlay-Test-YYYYMMDD-HHMMSS.log`.

The run is considered passed only when the log contains:

```text
PathOverlay test package passed all automated checks.
```

Each assertion writes a `[PASS]` or `[FAIL]` line with relevant rule ids, source
paths, store paths, shadow paths, and observed content. On failure, the script
prints `PathOverlaySvc.log`, `sc queryex PathOverlaySvc`, and recent Service
Control Manager events before cleanup.
