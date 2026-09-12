# NeoThemis

NeoThemis is a local competitive-programming contest judge for C++
submissions. It has a Qt GUI, a CLI, `.ncontest` archive support, parallel test
execution, built-in token comparison, per-problem custom checkers, and an
optional local web server for contestant submissions.

## Build

Linux:

```sh
sudo apt-get install build-essential cmake pkg-config qt6-base-dev libqt6sql6-sqlite \
  bubblewrap libseccomp-dev util-linux zlib1g-dev
cmake -S . -B build
cmake --build build
```

Secure judging on Linux requires `bubblewrap` at `/usr/bin/bwrap` or
`/bin/bwrap`, `prlimit` from `util-linux` at `/usr/bin/prlimit` or
`/bin/prlimit`, plus libseccomp at build time. NeoThemis still builds when that
backend is absent, but judging fails closed instead of running submitted code
with host access.

Windows with Qt MinGW:

```sh
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/mingw_64
cmake --build build
C:\Qt\6.11.1\mingw_64\bin\windeployqt.exe build\neothemis-gui.exe
```

Use the MinGW version that matches your Qt package. The default `compiler=g++`
is resolved through `PATH`, but a directly selected compiler executable does
not need to be on the app's `PATH`. On Windows, NeoThemis supplies that
compiler's directory only to compiler, submission, and custom-checker child
processes, allowing MinGW runtime DLLs beside the compiler to load without
mutating the app-wide environment. Paths containing spaces or Unicode are
supported. The GUI uses Qt Widgets; the local server uses Qt Core, Network, and
Sql.

GitHub Actions builds Linux and Windows packages on pushes and pull requests to
`main`, runs the regression and integration tests, performs a CLI smoke test,
and uploads build artifacts.

## Run

```sh
./build/neothemis-gui
./build/neothemis-gui contest.ncontest
./build/neothemis-gui --register-file-association
./build/neothemis-server --contest /path/to/contest
./build/neothemis-cli judge /path/to/contest
./build/neothemis-cli judge contest.ncontest
./build/neothemis-cli judge /path/to/contest --problem VENUE
./build/neothemis-cli judge /path/to/contest --contestant "Tran Minh Duy"
./build/neothemis-cli rejudge /path/to/contest --problem VENUE --contestant "Tran Minh Duy"
./build/neothemis-cli judge /path/to/trusted-contest --unsafe-no-sandbox
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

Judging requires the secure Linux backend by default. For trusted local code on
an unsupported platform, the CLI `--unsafe-no-sandbox` flag and the GUI's
global `Settings` -> `Application` -> `Enable secure sandbox` switch are
explicit controls. The GUI switch is on by default. Turning it off, or passing
the CLI flag, gives compiled code the permissions of the current OS user; never
do either for downloaded archives or contestant-controlled submissions.

The GUI switch is stored only in that OS user's local `QSettings` as
`sandbox_enabled`. It is not written to `neothemis.conf`, a
`.ncontest` archive, the server database, or server launch arguments, so it
affects only judging started locally by the GUI. The server has no unsafe
override and always requires the secure backend.

## Local Judging Server

`neothemis-server` runs a small local HTTP server where contestants can
register, log in, paste C++ source, submit to a problem, and view verdicts. It
reuses the same `core/` judge as the CLI and GUI.

Local-only mode:

```sh
./build/neothemis-server --contest /path/to/contest
```

The GUI can start the same server from `Server` -> `Start Local Server`. The
server defaults are configured from `Settings` -> `Server Settings`, and
contest-local users can be created from `Settings` -> `Server Users`. The
`neothemis-server` executable must be in the same folder as `neothemis-gui` or
available on `PATH`; CI packages include it beside the GUI.

The Server Users tab supports changing or removing an account later. With the
default settings it can show account passwords for local administration. Enable
the `Secure password storage (PBKDF2)` setting before creating accounts when
passwords must not be recoverable. Removing an account also removes its server sessions
and submission records, but does not remove its contestant folder. Its retained
snapshot files are safely reclaimed on the next server startup. NeoThemis
prevents removal of the final admin account.

`Contestant` -> `Sync Contestants to Server` creates an account for every
contestant folder that does not already have one. Existing accounts are left
unchanged, and contestant accounts whose folders no longer exist are removed.
Admin accounts are never removed by synchronization. New synchronized accounts
use the default password `123456`; change these passwords in Server Users before
exposing the server to contestants. Removing only a server account while keeping
its contestant folder causes the next synchronization to recreate that account.

While a GUI-started server is running, the GUI auto-refreshes the contest table
when server users, submitted sources, or judged results change. Registered
contestants are created under the contest `contestants_dir`, submitted source is
copied to that contestant folder, and judged server submissions are merged into
the configured `output_csv` file, defaulting to `results.csv`. The configured
scoreboard is regenerated from that same merged result snapshot.

By default the server binds to `127.0.0.1:8080`. On first run it creates an
admin account and a contestant join code, prints generated credentials to the
terminal, and stores data in `<contest>/.neothemis-server/`.

All server state is contest-local by default:

```text
<contest>/.neothemis-server/server.db
<contest>/.neothemis-server/submissions/
<contest>/.neothemis-server/jobs/
```

The data root, its `submissions` and `jobs` directories, and SQLite/lock files
must be real filesystem entries rather than symlinks. Startup fails before
opening state if one of these paths could redirect outside the configured data
root.

The GUI always starts the bundled server with that contest-local data folder.
When working from a `.ncontest` archive, the database lives in the extracted
contest folder and is included when the contest archive is saved again.

Contestant usernames may contain letters, digits, spaces, underscores, and
dashes. Slashes, path separators, and control characters are rejected.

The web submit page includes lightweight C++ syntax highlighting. It is served
by the local NeoThemis server itself, so no internet CDN is required.

`Settings` -> `Server Settings` has two contest-local web visibility options:

- `Enable web ranking` lets contestants open the live `/ranking` page. The table
  auto-refreshes and shows rank, contestant name, the score for each problem,
  and total points. It reads the contest `output_csv`, so local GUI/CLI judging
  and server judging update the same ranking. Verdicts, messages, and per-test
  data are not shown there.
- `Contestants can see own judge details` lets contestants open the detail page
  for their own submissions. They still cannot open another contestant's details.

Admins can always open the ranking and all submission detail pages. On the admin
submission table, `Ignore` marks a submission as skipped for ranking and
`results.csv` output while keeping the submission row and stored test details
available for audit.

### Import Server Users From CSV

Use `Settings` -> `Server Users` -> `Import CSV Users` to create accounts in
the contest-local server database. The CSV format is:

```text
username,password,role
Nguyen Van A,secret123,contestant
Admin User,admin-secret,admin
```

The header row is optional. `role` is optional and defaults to `contestant`.
Invalid rows and duplicate usernames are skipped and reported after import.

LAN mode must be explicit. HTTPS is optional and is disabled unless both TLS files are supplied:

```sh
./build/neothemis-server --contest /path/to/contest \
  --host 0.0.0.0 --port 8080 --allow-lan \
  --tls-cert server.crt --tls-key server.key \
  --admin-password change-this --join-code contest-join-code
```

Useful options:

```text
--data <folder>           Server database, submissions, and job folders
--admin-user <name>       Admin username, default: admin
--admin-password <pass>   Admin password
--join-code <code>        Required for contestant self-registration
--allow-lan               Required for network bind; HTTP is used unless TLS is enabled
--tls-cert <file>         PEM certificate; enables HTTPS with --tls-key
--tls-key <file>          PEM private key; enables HTTPS with --tls-cert
--secure-password-storage Store account passwords as PBKDF2 hashes
```

Security model:

- The default bind address is loopback only. LAN exposure requires `--allow-lan`.
  HTTPS is optional; provide `--tls-cert` and `--tls-key` to encrypt LAN traffic.
- Contestant registration requires the join code.
- Password storage is plaintext by default for compatibility with the local GUI.
  Enable `--secure-password-storage` (or the GUI setting) to migrate existing
  plaintext rows and store new passwords as salted PBKDF2 hashes. In secure mode
  the GUI never displays the password value.
- HTTPS sessions mark session cookies `Secure`; HTTP sessions omit that attribute.
- When HTTPS is disabled, LAN traffic is plain HTTP and is suitable only for a trusted,
  isolated network.
- Sessions use random HttpOnly SameSite cookies and POST submissions require a
  CSRF token.
- Admin submission-ignore actions also require the admin session CSRF token.
- Request bodies and source code are size-limited.
- HTTP/1.1 request framing is parsed strictly: only CRLF-delimited,
  origin-form requests with one unambiguous `Content-Length` are accepted;
  transfer encoding, trailing bytes, and pipelined requests are rejected.
- Submission admission is enforced atomically in SQLite: each user may have at
  most 3 active submissions, the server may have at most 64 active submissions
  globally, and each user may create at most 10 submissions in a rolling
  60-second window. Active means running, staging, or queued (ignored queued
  work is terminalized); the rolling rate includes accepted submissions even
  if they have already finished, failed, or were ignored. Rejected attempts do
  not create database rows or source snapshots.
- Usernames and problem names are restricted to safe identifier characters.
- Submissions are stored under the server data folder, never by user-supplied
  paths.
- Every accepted request first receives an immutable, submission-ID-scoped
  source snapshot under `.neothemis-server/submissions/`; later submissions by
  the same contestant cannot change a queued job's source. Snapshot publication
  uses a new numeric directory and an atomic file commit; pre-existing or
  symlinked destinations are rejected rather than overwritten.
- Snapshots are not overwritten. The newest 20 terminal submission snapshots
  per user are retained; active snapshots are always retained. Older terminal
  source files are removed at startup and after terminal outcomes, while their
  submission and per-test database audit rows remain (the pruned `source_path`
  is cleared). Cleanup canonicalizes each path and deletes only a regular file
  in its exact `.neothemis-server/submissions/<submission-id>/` directory;
  outside-root, sibling-prefix, and symlink-escaped paths are left untouched.
  Startup also reclaims database-unreferenced numeric snapshot directories only
  when they match the exact empty-or-single-regular-`.cpp` server layout;
  unexpected contents are preserved for manual inspection.
- Each submission is judged in a generated mini contest containing only that
  contestant source and the selected problem tests.
- Problem copies skip symlinks.
- The compiler, submitted program, and custom checker run in separate Linux
  bubblewrap sandboxes with user, mount, PID, IPC, UTS, cgroup, and network
  namespaces, no capabilities, a private filesystem view, bounded writable
  temporary filesystems, resource limits, and a libseccomp syscall filter.
- Sandbox setup is mandatory for the server. Startup fails if the isolation
  probe does not succeed.
- Required secure execution bounds each test or checker to at most 5 minutes
  and 4096 MiB of address space. Zero time or memory values become those finite
  ceilings, larger values are clamped, and an explicit stack limit is capped at
  4096 MiB.
- Runtime limits and reported per-test times use user-plus-kernel CPU time for
  the complete submitted process tree on both Linux and Windows. A separate,
  more generous parent-enforced wall-clock guard prevents sleeping processes
  from running forever and is never reported as the test time. Linux also uses
  process limits: `RLIMIT_AS` for virtual address space, `RLIMIT_STACK`,
  `RLIMIT_FSIZE`, a 64-descriptor `RLIMIT_NOFILE`, and disabled core dumps.
  Submission and checker file-size limits are 64 MiB and 16 MiB respectively;
  compilation uses fixed 30-second CPU-time, 1024 MiB address-space, 256 MiB
  stack, and 64 MiB file-size limits. Bubblewrap also caps the private `/tmp`
  at 256 MiB and the writable `/work` tmpfs at 64 MiB for submissions or
  16 MiB for checkers. Every sandbox invocation is restricted to one allowed
  CPU. Inside the sandbox user namespace, submissions and checkers are capped
  at 64 tasks and compiler invocations at 128 tasks; these hard limits also
  bound thread fan-out by their descendants. Runtime `MLE` attribution samples
  descendant address-space use near the configured limit; an otherwise
  unexplained `SIGKILL` remains `RE` instead of being guessed as a memory failure.
- The cgroup namespace isolates the sandbox's view of cgroups; NeoThemis does
  not create cgroups or use cgroup CPU/memory accounting. Address-space limits
  remain per process; CPU affinity and task limits bound fan-out, but they are
  not aggregate cgroup CPU or memory quotas.
- The judge also applies the configured `forbidden_pattern` source filters as a
  contest-policy convenience; these text filters are not the security boundary.
- The server runs one judge worker and forces `parallel_jobs=1`, `compile_jobs=1`
  and `test_jobs=1` per submitted source, regardless of the contest's desktop/CLI
  worker settings, to avoid one contestant consuming all CPU cores.
- Result publication is backed by a transactional SQLite outbox. The server
  repairs all database-owned contestant/problem cells on startup, retries
  unacknowledged generations, and acknowledges only the exact generation it
  published. Detailed results are merged by key under an adjacent persistent
  advisory lock; scoreboard generation occurs while holding that same lock, so
  concurrent CLI, GUI, and server writers cannot silently replace one another's
  cells.

Important limitation: namespace and syscall isolation still shares the host
Linux kernel. It does not protect against kernel, bubblewrap, compiler, dynamic
loader, or CPU side-channel vulnerabilities. For hostile code or a high-assurance
event, run the whole server inside a maintained VM on a dedicated contest
machine as an additional boundary. A secure backend is not currently provided
on Windows, so the server refuses to start there.

Explicitly unsafe local judging on Windows is still contained in a mandatory
job object: descendants are killed before the judge returns, CPU accounting is
job-wide, configured memory is enforced per process and for the aggregate job,
and children inherit only their three standard handles. This is resource and
lifecycle control, not filesystem, syscall, or network isolation; Windows does
not currently enforce the configured output-file-size limit.

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
Contestant, problem, and test directories and source, configuration, checker,
input, answer, and `testlib.h` files must be real entries under their canonical
parent. Nested symlinks are skipped or rejected and are never followed outside
the validated contest trees.

In the GUI, `Contestant` -> `Add Contestants From Folder` imports contestant
folders into the configured `contestants_dir`. If the selected folder contains
subfolders, each subfolder is imported as one contestant. If it has no
subfolders, the selected folder itself is imported as one contestant. Existing
contestant folders are merged and matching files are overwritten.

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
NeoThemis exposes the input read-only inside the private run filesystem using
aliases such as
`A.inp`, `a.inp`, `A.INP`, `1.inp`, and `1.INP`. It then looks for output files
such as `A.out`, `a.out`, `A.OUT`, `1.out`, and `1.OUT`. This supports
`freopen`-based solutions.

Temporary run directories are removed after judging unless `keep_workdir=true`.
Each judge invocation owns one uniquely named work directory, so its cleanup
cannot remove another concurrent or retained run. In secure mode output aliases
are pre-created writable bind mounts backed by a size-limited private run
filesystem; the contestant cannot browse the host contest or work directory.
An existing `.neothemis-work` symlink or non-directory is rejected before any
judge work is created.
Within a retained invocation, submission executables live under
`submissions/<contestant>/<problem>/build`, each test has its own
`tests/<test>` artifact directory, and custom checkers live under the separate
`internal/checkers` namespace. User-controlled names therefore cannot replace
an executable, log, run directory, or checker artifact.

`parallel_jobs=0` chooses an automatic worker limit, preferring detected
performance cores on heterogeneous CPUs and falling back to physical cores or
half the logical thread count. Positive limits are capped by available physical
cores. `compile_jobs` and `test_jobs` override that default independently; a zero
phase limit inherits `parallel_jobs`. Compilation finishes before timed tests
begin, so compiler processes from the same judging run do not compete with tests.

On Linux and Windows, test workers are assigned distinct physical cores when
the OS exposes usable topology and affinity controls. Core reservations are
shared across judging runs in the same NeoThemis process. The
assignment prefers detected performance cores and avoids running simultaneous
tests on hardware threads that share one physical core. This is not exclusive
ownership of the machine: other applications, separate NeoThemis processes, shared
cache/memory bandwidth, CPU power limits and temperature can still affect timing.

Enable `timing_focused=true` for parallel compilation followed by serial test
execution. Its timed phase also waits for other reserved judging work in the same
process to finish and blocks new judging work there until that phase completes.
This ignores, but preserves, `test_jobs`, so switching the mode off
restores the configured testing limit. In the GUI these controls are in
Settings → Contest. For example, `compile_jobs=4`, `test_jobs=2` keeps compilation
parallel while limiting simultaneous timed tests to two; turning timing-focused
judging on runs one test at a time instead. Use an otherwise idle machine when
comparing timing-sensitive results; CPU-time accounting does not eliminate
resource contention.

Compiler warnings never cause `CE`: NeoThemis removes warning-as-error flags,
adds the compiler's warning-tolerant option, and accepts the build whenever a
non-empty executable was produced. Actual compile failures remain `CE` and keep
the first 64 KiB of compiler diagnostics plus a truncation marker when needed.

## Checkers And Verdicts

`checker=token` uses NeoThemis' built-in whitespace-token comparison. The
contestant output and official answer are split by whitespace and compared token
by token. Exact token equality gives `AC` and full points for the test;
otherwise the result is `WA`.

`checker=custom` compiles `checker.cpp` from the problem folder.
`checker=custom:<path>` compiles a checker source relative to that same problem
folder. These settings preserve the original NeoThemis exit-7 protocol: the
first number printed by the checker is an absolute number of points for that
test.

`checker=testlib` also compiles `checker.cpp`, while
`checker=testlib:<path>` selects another relative source file. These settings
use the testlib percentage protocol: `quitp(50, "message")` awards 50% of the
current test's configured points. This explicit setting avoids changing the
meaning of existing `custom` checkers.

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

with either:

```text
checker=custom
```

or, for a checker that uses `quitp`:

```text
checker=testlib
```

Custom checker compilation uses the same `compiler` and `compile_flags` as
submissions and adds the problem folder as an include directory. The source is
mounted read-only at `/src`, while the checker binary and diagnostics are
written to the current judge invocation's private work directory:

```text
<contest>/.neothemis-work/<run>/internal/checkers/<problem>/build/checker
```

On Windows the output name is `checker.exe`; elsewhere it is `checker`. The
checker is rebuilt for each judge invocation, so generated executables never
modify or become part of the problem test folder. If the checker is missing or
does not compile, all tests for that problem return `IE` with the checker error
message; this is not a contestant `CE`.

`testlib.h` is a single-header checker library commonly used in programming
contests. It provides helpers such as `registerTestlibCmd`, `inf`, `ouf`,
`ans`, typed readers, and verdict helpers. If `checker.cpp` contains
`#include "testlib.h"` or `#include <testlib.h>`, NeoThemis requires
`testlib.h` to be in the same problem folder and compiles with the problem
folder as an include directory, so the include is found.

After the contestant program exits successfully and an output file is found,
NeoThemis runs a custom checker with only three read-only data mounts:

```text
/checker /input/original.inp /actual/output.out /answer/expected.out
```

Checker stdout and stderr are written to
`submissions/<contestant>/<problem>/tests/<test>/checker.out` and
`checker.err` inside that judge run. The first non-empty checker output becomes
the result message.

Custom checker verdict mapping:

| Checker result | NeoThemis result |
| --- | --- |
| Times out | `IE`, message `checker time limit exceeded` |
| Exceeds memory | `IE`, message `checker memory limit exceeded` |
| Exit code `0` | `AC`, full test points |
| Exit code `1` | `WA`, zero points |
| Exit code `7`, `checker=custom[...]` | Parse the first number printed by the checker as absolute points and clamp it to `[0, max_points]` |
| Exit code `7`, `checker=testlib[...]` | Parse the first number produced by `quitp`, clamp it to `[0, 100]`, and award that percentage of `max_points` |
| Any other exit code | `IE` |

An exit-7 score of zero gives `WA`, a score at the full-point boundary gives
`AC`, and a score strictly between them gives `PC` (partially correct). The
earned decimal points are retained in CSV, the GUI, the server database, and
scoreboards. `quitpi` is not a numeric score and therefore produces `IE` unless
its first output token is numeric. Checker presentation-error or internal-error
exits are also treated as NeoThemis `IE`.

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

The `message` field is stored in full, including quoted multiline diagnostics.
In the GUI result-detail table, long descriptions are visually elided; hover the
description cell for the pointer cursor and click it to open the full text.

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
server_ranking_enabled=false
server_contestant_details_enabled=false
compiler=g++
compile_flags=-std=c++14 -O2 -pipe
stack_limit_mb=64
parallel_jobs=0
compile_jobs=0
test_jobs=0
timing_focused=false

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

Configured input and output paths are canonicalized and must remain inside the
contest root. The two output files must be distinct and cannot target the
contest configuration, lock-file names, contestant sources, tests, judge work,
or server-state trees. Existing symlink escapes and Windows case-insensitive
aliases are rejected.

`keep_workdir`
: Whether to keep `.neothemis-work` after judging. Default: `false`. Accepted
values are `true`, `false`, `1`, `0`, `yes`, `no`, `on`, and `off`.

`server_ranking_enabled`
: Whether contestants can view the live web ranking page. Default: `false`.
Admins can always view the ranking.

`server_contestant_details_enabled`
: Whether contestants can view per-test details for their own web submissions.
Default: `false`. Admins can always view all submission details.

`compiler`
: C++ compiler command. Default: `g++`. This command is used for submissions and
custom checker sources. A full executable path may contain spaces or Unicode.
On Windows, its parent directory is prepended to `PATH` in each judge child
process only, so MinGW-generated submissions and custom checkers can find the
compiler's adjacent runtime DLLs even when `g++` is absent from the parent
process `PATH`; NeoThemis does not change the global application environment.

`compile_flags`
: Compiler flags used for submissions and custom checker sources. Default:
`-std=c++14 -O2 -pipe`.

`stack_limit_mb`
: Contest-wide maximum stack size, in megabytes. Default: `64`. Use `0` to
disable the separate stack limit. Required secure execution still applies its
finite address-space ceiling. On Linux the stack limit is enforced with
`RLIMIT_STACK` and capped at 4096 MiB in secure execution.
NeoThemis also appends `-fno-optimize-sibling-calls` when a stack limit is
enabled, so simple recursive submissions cannot be optimized into a loop. On
Windows, supported compilers receive a stack reserve linker flag.

`parallel_jobs`
: Default worker limit for compilation preparation and test judging. Default:
`0`, which prefers detected performance cores on heterogeneous CPUs and falls
back to physical cores or half the logical thread count. Positive limits are
capped by available physical cores. Worker counts must be non-negative decimal
integers; negative, fractional, trailing-text and overflowing values are rejected.

`compile_jobs`
: Compilation worker limit. Default: `0`, meaning inherit `parallel_jobs`.

`test_jobs`
: Timed test worker limit. Default: `0`, meaning inherit `parallel_jobs`.
The same physical-core cap applies to both phase-specific limits.

`timing_focused`
: Default: `false`. When enabled, compilation still uses its configured worker
limit, but tests execute one at a time. The saved `test_jobs` value is retained
and takes effect again when this mode is disabled.

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

# Checker options: token, custom:<path>, testlib:<path>
# Examples: checker=custom, checker=testlib, or checker=testlib:checker.cpp
# Custom checkers that include testlib.h must keep testlib.h in this problem folder.
# In testlib mode, quitp(50, ...) awards 50% of this test's points.

# Optional per-test overrides. Test names match the test folder names.
# Example: test_points.1=2
```

`time_limit_ms`
: Per-test CPU-time limit, in milliseconds. CPU time is user plus kernel time
across the complete process tree, on both Linux and Windows; `time_ms` in judge
results reports the same value. A separate wall-clock safety guard stops
sleeping or stuck processes without changing the reported time. Custom checkers
use this limit too.
In required secure mode, zero becomes 300000 ms and larger values are clamped
to that 5-minute ceiling.

`memory_limit_mb`
: Per-test memory limit, in megabytes. In required secure mode, zero becomes
4096 MiB and larger values are clamped to that ceiling. Linux enforces the
effective value with `RLIMIT_AS`; Windows uses per-process and aggregate job
memory limits. Only the explicitly unsafe runner treats zero as disabled.
Secure judging is currently Linux-only. Custom checkers use this limit too.

`default_points`
: Points awarded for each accepted test unless a per-test override exists.
Decimal values are allowed.

`checker`
: Checker mode for this problem. Supported values are `token`, `custom`,
`custom:<path>`, `testlib`, and `testlib:<path>`. The `custom` forms interpret
exit-7 scores as absolute points; the `testlib` forms interpret `quitp` values
as percentages of the current test's configured points.

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

- `core/` contains shared contest configuration, judging and sandboxed process
  execution, contest inspection, CSV/spreadsheet support, and archive
  operations used by the CLI, GUI, and server.
- `cli/` contains command parsing and terminal-specific output.
- `gui/MainWindow.cpp` provides the main-window factory, while
  `gui/MainWindowPrivate.hpp` owns its private state. The implementation is
  divided by workflow across `gui/MainWindowShell.cpp`,
  `gui/MainWindowAppearance.cpp`, `gui/MainWindowContest.cpp`,
  `gui/MainWindowJudge.cpp`, and `gui/MainWindowServer.cpp`.
- `gui/FileFormats.cpp` contains thin GUI adapters for the shared CSV/archive
  implementations; verdict conversion lives in the shared core.
- `gui/GuiSupport.cpp`, `gui/Theme.cpp`, `gui/ThemeBackground.cpp`,
  `gui/Translations.cpp`, and `gui/WindowResizeHandles.cpp` isolate reusable
  GUI helpers, styling and background effects, localized text, and Windows
  frame behavior.
- `gui/main.cpp` initializes Qt and launches the main window.
- `server/database/` contains the shared SQLite schema and persistence API for
  accounts, sessions, submissions, and results used by both the server and GUI.
- `server/http/` contains reusable HTTP request parsing, response construction,
  and connection handling.
- `server/main.cpp` composes those modules into the local server's routes,
  contest synchronization, and single-worker submission queue.
