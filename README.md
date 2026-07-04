# NeoThemis

NeoThemis is a local competitive-programming contest judge for C++
submissions. It has a Qt GUI, a CLI, `.ncontest` archive support, parallel test
execution, built-in token comparison, and per-problem custom checkers.

## Build

Linux:

```sh
cmake -S . -B build
cmake --build build
```

Windows with Qt MinGW:

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/mingw_64
cmake --build build
C:\Qt\6.11.1\mingw_64\bin\windeployqt.exe build\neothemis-gui.exe
```

Use the MinGW version that matches your Qt package. The built-in judge expects
`g++` to be available on `PATH`, unless `compiler` is changed in
`neothemis.conf`.

GitHub Actions builds Linux and Windows packages on pushes and pull requests to
`main`. The workflow also runs a CLI smoke test and uploads build artifacts.

## Run

```sh
./build/neothemis-gui
./build/neothemis-gui contest.ncontest
./build/neothemis-gui --register-file-association
./build/neothemis-cli judge /path/to/contest
./build/neothemis-cli judge contest.ncontest
./build/neothemis-cli judge /path/to/contest --problem VENUE
./build/neothemis-cli judge /path/to/contest --contestant "Tran Minh Duy"
./build/neothemis-cli rejudge /path/to/contest --problem VENUE --contestant "Tran Minh Duy"
./build/neothemis-cli export-scoreboard contest.ncontest scoreboard.xlsx
./build/neothemis-cli export-data contest.ncontest data.xlsx
```

Passing a `.ncontest` file to `neothemis-gui` opens it automatically. The
application settings screen can register NeoThemis as the user-level opener for
`.ncontest` files on Windows and Linux. The same registration is available from
`--register-file-association`; it does not require administrator or root access.

`judge` and `rejudge` use the same execution path. Problem and contestant
filters are case-insensitive and repeatable. Archive inputs are extracted to a
temporary folder; mutating commands save changes back to the archive.

## Contest Layout

```text
contest/
  neothemis.conf
  contestants/
    Alice/
      A.cpp
      B.cpp
    Bob/
      A.cpp
  tests/
    A/
      problem.conf
      checker.cpp
      testlib.h
      1/
        A.inp
        A.out
      2/
        A.inp
        A.out
    B/
      problem.conf
      1/
        B.inp
        B.out
```

Problem names are matched by source-file stem. For example, tests in `tests/A`
are judged against each contestant's `A.cpp`, `A.cc`, or `A.cxx`.

Each test folder stores the official input and answer using the problem name,
such as `tests/A/1/A.inp` and `tests/A/1/A.out`.

## Judging Flow

For each selected problem, NeoThemis loads `problem.conf`, discovers test
folders, and prepares the checker. For each selected contestant/problem pair it
then:

1. Finds a source file named `<problem>.cpp`, `<problem>.cc`, or
   `<problem>.cxx`.
2. Rejects forbidden source patterns with `SV`.
3. Compiles the submission once for that problem.
4. Queues every runnable test into one shared worker queue.
5. Runs each test in a separate temporary working directory.
6. Compares the produced output with either the token checker or a compiled
   custom checker.

Submissions do not receive stdin/stdout redirection for normal judging. Instead,
NeoThemis copies the input file into the run directory using aliases such as
`A.inp`, `a.inp`, `A.INP`, `1.inp`, and `1.INP`. It then looks for output files
such as `A.out`, `a.out`, `A.OUT`, `1.out`, and `1.OUT`. This supports
`freopen`-based solutions.

Temporary run directories are removed after judging unless `keep_workdir=true`.
Output symlinks are ignored. This is hardening, not a full sandbox; run
untrusted submissions inside an OS-level sandbox or container if strong
isolation is required.

`parallel_jobs=0` uses the number of physical CPU cores when available, or half
the logical thread count as a fallback. Set it to `1` for serial judging or a
positive number to cap the worker queue.

## Checkers And Verdicts

`checker=token` uses NeoThemis' built-in whitespace-token comparison. The
contestant output and official answer are split by whitespace and compared token
by token. Exact token equality gives `AC` and full points for the test;
otherwise the result is `WA`.

`checker=custom` compiles `checker.cpp` from the problem folder.
`checker=custom:<path>` compiles a checker source relative to that same problem
folder.

For a problem such as `VENUE`, the common layout is:

```text
tests/VENUE/
  problem.conf
  checker.cpp
  testlib.h
  1/
    VENUE.inp
    VENUE.out
```

with:

```text
checker=custom
```

Custom checker compilation uses the same `compiler` and `compile_flags` as
submissions, adds the problem folder as an include directory, and writes the
binary into the problem folder:

```text
<compiler> <compile_flags> <stack flags> -I <problem folder> <checker source> -o <problem folder>/checker
```

On Windows the output is `checker.exe`. On Linux and macOS it is `checker`.
NeoThemis reuses the existing checker binary when it is newer than the checker
source and, if used, `testlib.h`. Otherwise it recompiles. If the checker is
missing or does not compile, all tests for that problem return `IE` with the
checker error message; this is not a contestant `CE`.

`testlib.h` is a single-header checker library commonly used in programming
contests. It provides helpers such as `registerTestlibCmd`, `inf`, `ouf`,
`ans`, typed readers, and verdict helpers. If `checker.cpp` contains
`#include "testlib.h"` or `#include <testlib.h>`, NeoThemis requires
`testlib.h` to be in the same problem folder and compiles with the problem
folder as an include directory, so the include is found.

After the contestant program exits successfully and an output file is found,
NeoThemis runs a custom checker in the test run directory as:

```text
checker <input-file-name> <contestant-output-file-name> <absolute-answer-path>
```

Checker stdout is written to `<test>.checker.out`; checker stderr is written to
`<test>.checker.err`. The first non-empty checker output becomes the result
message.

Custom checker verdict mapping:

| Checker result | NeoThemis result |
| --- | --- |
| Times out | `IE`, message `checker time limit exceeded` |
| Exceeds memory | `IE`, message `checker memory limit exceeded` |
| Exit code `0` | `AC`, full test points |
| Exit code `1` | `WA`, zero points |
| Exit code `7` | Parse the first number printed by the checker and clamp it to `[0, max_points]`; positive points are reported as `AC`, zero points as `WA` |
| Any other exit code | `IE` |

This means testlib checkers should use normal accepted/wrong-answer exits, and
partial-point checkers should print a numeric score when returning exit code
`7`. Checker presentation-error or internal-error exits are treated as
NeoThemis `IE`.

Submission verdict mapping before output checking:

| Condition | Verdict |
| --- | --- |
| Source file missing | `MS` |
| Forbidden source pattern | `SV` |
| Submission compile failure | `CE` |
| Runtime timeout | `TLE` |
| Runtime memory limit | `MLE` |
| Non-zero submission exit code | `RE` |
| Expected input/answer missing or judge setup failure | `IE` |
| Output file not found | `WA` |

Detailed CSV rows use:

```text
contestant,problem,test,verdict,time_ms,exit_code,max_points,earned_points,message
```

The simplified scoreboard has one row per contestant, one column per problem,
and a numeric `total` column. Missing source cells are written as `MS(0)`;
compile-error cells are written as `CE(0)`.

## Contest Settings

Judge settings live in `neothemis.conf` inside each contest folder. If the file
does not exist, NeoThemis creates this default file before judging:

```text
core=builtin
contestants_dir=contestants
tests_dir=tests
output_csv=results.csv
scoreboard_csv=scoreboard.csv
keep_workdir=false
compiler=g++
compile_flags=-std=c++17 -O2 -pipe
stack_limit_mb=64
parallel_jobs=0

forbidden_pattern=system(
forbidden_pattern=popen(
forbidden_pattern=fork(
forbidden_pattern=exec(
forbidden_pattern=#include <unistd.h>
forbidden_pattern=#include <sys/
forbidden_pattern=#include <windows.h>
```

`core`
: Judge backend to use. Default: `builtin`. This is currently the only shipped
backend.

`contestants_dir`
: Directory containing contestant folders. Default: `contestants`. Relative
paths are resolved from the contest folder.

`tests_dir`
: Directory containing problem test folders. Default: `tests`. Relative paths
are resolved from the contest folder.

`output_csv`
: Detailed result CSV path. Default: `results.csv`. Relative paths are written
inside the contest folder.

`scoreboard_csv`
: Simplified scoreboard CSV path. Default: `scoreboard.csv`. Relative paths are
written inside the contest folder.

`keep_workdir`
: Whether to keep `.neothemis-work` after judging. Default: `false`. Accepted
values are `true`, `false`, `1`, `0`, `yes`, `no`, `on`, and `off`.

`compiler`
: C++ compiler command. Default: `g++`. This command is used for submissions and
custom checker sources.

`compile_flags`
: Compiler flags used for submissions and custom checker sources. Default:
`-std=c++17 -O2 -pipe`.

`stack_limit_mb`
: Contest-wide maximum stack size, in megabytes. Default: `64`. Use `0` to
disable the stack limit. On Linux this is enforced with `RLIMIT_STACK`.
NeoThemis also appends `-fno-optimize-sibling-calls` when a stack limit is
enabled, so simple recursive submissions cannot be optimized into a loop. On
Windows, supported compilers receive a stack reserve linker flag.

`parallel_jobs`
: Number of worker jobs for compilation preparation and test judging. Default:
`0`, which auto-detects available CPU cores.

`forbidden_pattern`
: Repeatable source-code text filter. Matching is case-insensitive.

## Problem Settings

Each problem has a `problem.conf` file inside its test folder, such as
`tests/A/problem.conf`. If it does not exist, NeoThemis creates this default
file before judging:

```text
time_limit_ms=1000
memory_limit_mb=256
default_points=1
checker=token

# Checker options: token, custom:<path-in-this-problem-folder>
# Examples: checker=custom or checker=custom:checker.cpp
# Custom checkers that include testlib.h must keep testlib.h in this problem folder.

# Optional per-test overrides. Test names match the test folder names.
# Example: test_points.1=2
```

`time_limit_ms`
: Per-test runtime limit, in milliseconds. Custom checkers use this limit too.

`memory_limit_mb`
: Per-test memory limit, in megabytes. Use `0` to disable the memory limit.
On Linux this is enforced with `RLIMIT_AS`; on Windows it is currently
documented but not enforced by the built-in runner. Custom checkers use this
limit too.

`default_points`
: Points awarded for each accepted test unless a per-test override exists.
Decimal values are allowed.

`checker`
: Checker mode for this problem. Supported values are `token`, `custom`, and
`custom:<path>`.

`test_points.<test>`
: Points override for one test folder. The key after `test_points.` normally
matches the test folder name. Test folders with a numeric suffix also match that
suffix without leading zeroes, so `test01`, `01`, and `1` can all be configured
with `test_points.1=2`.

## Config CLI

```sh
./build/neothemis-cli config /path/to/contest available
./build/neothemis-cli config /path/to/contest list
./build/neothemis-cli config /path/to/contest set scoreboard_csv scores.csv
./build/neothemis-cli config /path/to/contest problem A available
./build/neothemis-cli config /path/to/contest problem A list
./build/neothemis-cli config /path/to/contest problem A set time_limit_ms 3000
./build/neothemis-cli config /path/to/contest problem A set memory_limit_mb 512
./build/neothemis-cli config /path/to/contest problem A set checker custom
./build/neothemis-cli config /path/to/contest problem A points 2 1-10
./build/neothemis-cli config /path/to/contest problem A points 5 1 5 3 12
./build/neothemis-cli config contest.ncontest problem A points default 1-10
```

The `points` command uses `points <value> <tests...>`, where tests can be
individual names or numeric ranges. Use `points default <tests...>` to leave
those test points blank so `default_points` is used.

## Archive CLI

```sh
./build/neothemis-cli pack /path/to/contest contest.ncontest
./build/neothemis-cli unpack contest.ncontest /path/to/output-folder
./build/neothemis-cli convert old.contest converted.ncontest
./build/neothemis-cli convert /path/to/old-themis-folder converted.ncontest
```

ZIP compression and extraction use available CPU cores across archive entries.
Compression keeps a bounded number of prepared entries in memory while writing
the archive in stable path order. The GUI uses the same archive implementation
for opening and saving `.ncontest` files.

`convert` reads old Themis contests whose `*.cfg` and `*.config` files are
zlib-compressed XML. It uses `ContestantDirectories.txt` and
`TaskDirectories.txt` to build the new `contestants/` and `tests/` folders, and
drops old Themis-only config files from the generated `.ncontest`.

## Source Layout

- `core/` contains judging, process execution, contest inspection, and archive
  operations shared by both frontends.
- `cli/` contains command parsing and terminal-specific output.
- `gui/MainWindow.cpp` owns Qt window state and coordinates user workflows.
- `gui/FileFormats.cpp` handles CSV/XLSX data and GUI archive adapters.
- `gui/GuiSupport.cpp`, `gui/Theme.cpp`, `gui/ThemeBackground.cpp`,
  `gui/Translations.cpp`, and `gui/WindowResizeHandles.cpp` isolate reusable
  GUI helpers, styling and background effects, localized text, and Windows
  frame behavior.
- `gui/main.cpp` initializes Qt and launches the main window.
