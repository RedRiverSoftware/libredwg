# rr-fork — build and test

This folder is the LibreDWG fork that produces `dwg2SVG.exe` and the
`libredwg-0.dll` consumed by the cableflow backend via C# bindings.

## Build

Run from the **parent directory** (`backend/lib/libredwg/`), not from
inside `rr-fork/`:

```sh
# 1. Windows native (always)
powershell.exe -ExecutionPolicy Bypass -NoProfile -File ./build-windows.ps1

# 2. Linux native via Docker — only required before pushing a PR so CI
#    checks pass.  CI runs on Linux and uses bin/csharp/libredwg.so,
#    which is just a copy of bin/linux/libredwg.so produced by this
#    script.  build-csharp-bindings.ps1 (step 3) only builds the C#
#    wrapper (libredwg_csharp.so) on top of an existing libredwg.so —
#    it does NOT rebuild the base lib.  Skipping this step during local
#    iteration is fine; Windows tests run against libredwg.dll which is
#    refreshed by steps 1 and 3.  Just remember to run it before push
#    or CI will fail with renderer-output mismatches.
powershell.exe -ExecutionPolicy Bypass -NoProfile -File ./build-linux-via-docker.ps1

# 3. C# bindings — builds the C# wrapper on top of the bases produced
#    by steps 1 (and optionally 2), then copies everything into
#    bin/csharp/.  Always required after step 1 to refresh
#    bin/csharp/libredwg.dll.
powershell.exe -ExecutionPolicy Bypass -NoProfile -File ./build-csharp-bindings.ps1
```

Native binaries land in `backend/lib/libredwg/bin/windows/`. The C#
bindings (`libredwg-0.dll`, `LibreDWG.Net.dll`, etc.) land in
`backend/lib/libredwg/bin/csharp/` and are copied into the project's
output dir on every test build.

### DLL-lock gotcha

If the cableflow backend is running (Saffron host) or JetBrains' debugger
is attached, the C# DLLs are file-locked and step 2 (and any `dotnet
test` / `dotnet build` that re-copies them) will fail with `MSB3027`
exhausted-retry errors. The fix is to stop the backend / detach the
debugger, then rebuild. We do **not** kill processes automatically —
ask first.

## Test

SVG-renderer tests live in **cableflow's test project**, not in
`rr-fork/test/unit-testing/`. The latter is libredwg's own API-level
test suite (round-trip parse correctness) and does not exercise SVG
output.

Renderer tests:

- Location: `backend/test/CAD/` — xUnit + FluentAssertions
- Pattern: build a minimal DWG in-memory with `LibreDWG.dwg_add_*`, call
  `DwgSvgApi.ToSvg(dwg)`, regex-assert on the SVG string. See
  `MTextRenderingTests.cs`, `ArcRenderingTests.cs`,
  `EllipseRenderingTests.cs`, `TextViewboxTests.cs` for examples.

Run:

```sh
# from backend/test/
dotnet test --filter 'FullyQualifiedName~CAD' --logger 'console;verbosity=minimal'

# Single test class
dotnet test --filter 'FullyQualifiedName~ArcRenderingTests'
```

When you change `dwg2SVG.c`, rebuild via step 1+2 above before
running the tests — the test project links against the compiled DLL,
not the C source.

## Don't break the tests

Any change to `programs/dwg2SVG.c` should leave the existing CAD tests
passing. If a test fails, decide whether:

- the behaviour change is **intentional** — update the test's
  assertion (and explain in the test comment why the new value is
  correct), or
- the behaviour change is a **regression** — fix the renderer.

Never just relax the assertion to make the test green.

## Perf probe (opt-in)

`PerfProbe.cs` in `backend/test/CAD/` benchmarks the library-mode SVG
renderer against the heaviest sample DWGs. It only runs when the
`RUN_PERF_PROBE` env var is set:

```sh
RUN_PERF_PROBE=1 dotnet test --filter 'FullyQualifiedName~PerfProbe' \
    --logger 'console;verbosity=detailed'
```

Skips silently when the referenced files aren't on disk. Use it to
verify a perf-sensitive change hasn't regressed before/after.
