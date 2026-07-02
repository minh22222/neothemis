# NeoThemis

NeoThemis is a small C++17 competitive-programming contest judge. A contest is
represented by a directory wrapper with contestant submissions and test cases.

## Layout

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
      1/
        A.inp
        A.out
      2/
        A.inp
        A.out
    B/
      1/
        B.inp
        B.out
```

Problem names are matched by source-file stem. For example, tests in
`tests/A` are judged against each contestant's `A.cpp`, `A.cc`, or `A.cxx`.

## Build

```sh
cmake -S . -B build
cmake --build build
```

This builds `neothemis-cli` on Linux and Windows. The built-in judge expects
`g++` to be available on `PATH`.

## Contest settings

Judge settings live in `neothemis.conf` inside each contest folder. If the file
does not exist, `neothemis-cli` creates this default file before judging:

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

### Contest setting reference

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
values are `true`, `false`, `1`, `0`, `yes`, `no`, `on`, and `off`. Enable this
when debugging compile logs, run folders, copied input files, or contestant
outputs.

`compiler`
: C++ compiler command. Default: `g++`. This command is used for both
submissions and checker sources.

`compile_flags`
: Compiler flags used for submissions and checker sources. Default:
`-std=c++17 -O2 -pipe`. Use this for language version, optimization, warnings,
or extra include paths.

`stack_limit_mb`
: Contest-wide maximum stack size, in megabytes. Default: `64`. Use `0` to
disable the stack limit. On Linux this is enforced at runtime with
`RLIMIT_STACK`; NeoThemis also appends `-fno-optimize-sibling-calls` when a
stack limit is enabled so simple recursive submissions cannot be optimized into
a loop. On Windows, supported compilers receive a stack reserve linker flag.

`parallel_jobs`
: Number of worker jobs used for compilation preparation and test judging.
Default: `0`, which auto-detects available CPU cores. Use `1` for serial runs or
a positive number to cap CPU usage. Each individual test still runs on one
worker.

`forbidden_pattern`
: Repeatable source-code text filter. A submission containing any configured
pattern is rejected before compilation with verdict `SV`. Matching is
case-insensitive. File I/O and `freopen` are allowed by default; dangerous
process/system APIs are blocked by the default patterns.

## Problem settings

Each problem has a `problem.conf` file inside its test folder, such as
`tests/A/problem.conf`. If it does not exist, `neothemis-cli` creates this
default file before judging the problem:

```text
time_limit_ms=1000
memory_limit_mb=256
default_points=1
checker=token

# Example: test_points.1=2
```

### Problem setting reference

`time_limit_ms`
: Per-test runtime limit for this problem, in milliseconds. Default: `1000`.
This limit is applied independently to each contestant test run. Custom checkers
also use this limit.

`memory_limit_mb`
: Per-test memory limit for this problem, in megabytes. Default: `256`. Use `0`
to disable the memory limit. On Linux this is enforced with `RLIMIT_AS`; on
Windows it is currently documented but not enforced by the built-in runner.
Custom checkers use this limit too.

`default_points`
: Points awarded for each accepted test unless a per-test override exists.
Default: `1`. Decimal values are allowed, for example `2.5`.

`checker`
: Checker mode for this problem. Default: `token`.

```text
checker=token
checker=custom
checker=custom:checker.cpp
```

`checker=token` uses NeoThemis' built-in whitespace-token comparison.
`checker=custom` compiles `checker.cpp` from the problem folder.
`checker=custom:<path>` compiles a checker source relative to that same problem
folder. Custom checkers are run as `checker input output answer`. If a custom
checker includes `testlib.h`, place `testlib.h` inside that same problem folder,
for example `tests/A/testlib.h`.

`test_points.<test>`
: Points override for one test folder. Example: `test_points.1=2`. The key
after `test_points.` normally matches the test folder name. Test folders with a
numeric suffix also match that suffix without leading zeroes, so `test01`,
`01`, and `1` can all be configured with `test_points.1=2`.

Checker binaries are cached in the problem folder as `checker` on Linux/macOS or
`checker.exe` on Windows. If that binary already exists, NeoThemis uses it
directly instead of rebuilding the checker source.

`parallel_jobs=0` auto-detects the available CPU cores. Set it to `1` for
serial judging or to another positive number to cap the test worker queue.
NeoThemis compiles each contestant/problem once, then runs that submission's
tests in parallel workers. Each worker runs one test process at a time. On an
interactive terminal, the CLI redraws one fixed progress area with worker
states, completed item count, a progress bar, and total elapsed runtime.
Compilation preparation is also parallelized and shown as a separate `prepare`
phase before the `judge` phase.

Each test folder stores its official input and answer using the problem name,
such as `tests/A/1/A.inp` and `tests/A/1/A.out`.

Submissions run in an isolated per-test working directory. NeoThemis copies the
input file into that directory with case-insensitive-style aliases such as
`A.inp`, `a.inp`, `A.INP`, `1.inp`, and `1.INP`, then looks for output files
such as `A.out`, `a.out`, `A.OUT`, `1.out`, and `1.OUT`. This supports
`freopen`-based solutions without using stdin/stdout redirection for judging.
By default, temporary run directories are created outside the contest folder and
removed after judging, which makes accidental relative access to official tests
harder. Output symlinks are ignored. This is hardening, not a full sandbox:
untrusted submissions should still be run inside an OS-level sandbox or
container for strong isolation.

## Config CLI

Contest settings can be edited from the CLI:

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
```

The `points` command uses `points <value> <tests...>`, where tests can be
individual names or numeric ranges.

## Run

```sh
./build/neothemis-cli judge /path/to/contest
./build/neothemis-cli judge /path/to/contest --problem VENUE
./build/neothemis-cli judge /path/to/contest --contestant "Tran Minh Duy"
./build/neothemis-cli rejudge /path/to/contest --problem VENUE --contestant "Tran Minh Duy"
```

`judge` and `rejudge` use the same execution path. The filters are
case-insensitive and repeatable. Quote contestant names that contain spaces.
Filtered runs write CSV files containing the selected scope.

The detailed CSV columns are:

```text
contestant,problem,test,verdict,time_ms,exit_code,max_points,earned_points,message
```

The simplified scoreboard CSV has one row per contestant, one column per
problem, and a final `total` column.

If a contestant has no source for a problem, that problem cell is written as
`MS(0)`. If their source does not compile, the cell is written as `CE(0)`. The
`total` column remains numeric.

Verdicts are `AC`, `WA`, `CE`, `RE`, `TLE`, `MLE`, `MS`, `SV`, and `IE`.
