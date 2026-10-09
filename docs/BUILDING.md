# Building Echo

Echo builds with CMake + Ninja and either **LLVM-MinGW** (clang, no Visual
Studio or administrator rights needed) or **MSVC** (Visual Studio 2022 Build
Tools). The result is a single statically linked `Echo.exe` with no runtime
DLL dependencies.

## 1. Install the tools (one time)

Option A — no admin rights (used to develop Echo):

```powershell
winget install --id MartinStorsjo.LLVM-MinGW.UCRT --scope user
python -m pip install --user cmake ninja
```

Option B — Visual Studio 2022 Build Tools with the "Desktop development with
C++" workload (includes CMake and Ninja); run the commands below from a
"Developer PowerShell for VS 2022".

Optional:

* Inno Setup 6 for the installer: `winget install --id JRSoftware.InnoSetup --scope user`
* Python packages `onnx numpy` only to regenerate the AI test model.

## 2. Build

```powershell
./build.ps1                 # Release build into build/release/bin
./build.ps1 -Config Debug   # Debug build into build/debug/bin
./build.ps1 -Test           # build and run the unit tests
./build.ps1 -Package        # build, test, portable ZIP, installer, checksums
```

`build.ps1` finds LLVM-MinGW on PATH or in the winget package folder, and
CMake/Ninja on PATH or in the Python user Scripts folder.

Manual equivalent (LLVM-MinGW):

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++ `
      -DCMAKE_RC_COMPILER=llvm-windres
cmake --build build/release
```

MSVC: same, with `-DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl` from a
developer prompt.

Configure options: `-DECHO_APP_NAME=...` and `-DECHO_APP_ID=...` rename the
product (window title, executable, settings folder), `-DECHO_BUILD_TESTS=OFF`,
`-DECHO_BUILD_TOOLS=OFF`. The version is `project(Echo VERSION x.y.z)` in
`CMakeLists.txt`.

## 3. Outputs

```
build/release/bin/Echo.exe          the application
build/release/bin/echo_tests.exe    automated tests (doctest)
build/release/bin/echo_hwtest.exe   real-hardware probe
build/package/Echo-<ver>-portable-win64.zip
build/package/Echo-<ver>-setup-win64.exe
build/package/SHA256SUMS.txt
```

## 4. Tests

```powershell
build/release/bin/echo_tests.exe                 # all automated tests
build/release/bin/echo_tests.exe -tc="*pitch*"   # filter by name
build/release/bin/echo_tests.exe -s              # show every assertion
```

The AI inference tests run only when `onnxruntime.dll` is present in
`build/release/bin/ai/` (copy it there or run `tools/fetch_onnxruntime.ps1
-Destination build/release/bin/ai`); otherwise they report "SKIPPED".

Hardware probe (uses your real devices; see `TESTING.md`):

```powershell
build/release/bin/echo_hwtest.exe list
build/release/bin/echo_hwtest.exe mic 3
build/release/bin/echo_hwtest.exe playback      # quiet -40 dBFS tone + -30 dBFS chirps
build/release/bin/echo_hwtest.exe procloop
build/release/bin/echo_hwtest.exe sysaudio
build/release/bin/echo_hwtest.exe cables        # needs a virtual cable driver
build/release/bin/echo_hwtest.exe approute      # experimental per-app routing API
build/release/bin/echo_hwtest.exe soak 60
```

UI smoke test (renders every page into PNGs using a throwaway profile):

```powershell
build/release/bin/Echo.exe --ui-snapshots=C:\temp\echo-shots
```

## 5. Debugging

* Debug builds log to stderr as well as `%LOCALAPPDATA%\Echo\logs\echo.log`.
* LLVM-MinGW ships `lldb`; MSVC builds work with Visual Studio's debugger.
* Crash dumps: `%LOCALAPPDATA%\Echo\crashes\*.dmp`.
* Command line: `--safe-mode` (effects bypassed, audio not started),
  `--reset-settings` (moves `session.json` aside), `--minimized`.

## 6. Project layout

See `ARCHITECTURE.md` (module map). Dependencies are vendored under
`third_party/` with their licenses; nothing is downloaded during the build.
