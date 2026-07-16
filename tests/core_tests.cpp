#include "neothemis/ContestArchive.hpp"
#include "neothemis/Config.hpp"
#include "neothemis/Csv.hpp"
#include "neothemis/JudgeCore.hpp"
#include "neothemis/Spreadsheet.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::string& prefix)
        : path_(neothemis::make_temp_directory(prefix)) {}

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "failed to create " + path.string());
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    require(static_cast<bool>(output), "failed to write " + path.string());
}

void write_testlib_protocol_fixture(const fs::path& path) {
    write_file(path, R"TESTLIB(#pragma once

#include <cstdlib>
#include <fstream>
#include <iostream>

enum TResult { _ok = 0, _wa = 1 };

class TestStream {
public:
    void open(const char* path) {
        input_.open(path);
        if (!input_) {
            std::exit(3);
        }
    }

    int readInt() {
        int value = 0;
        if (!(input_ >> value)) {
            std::exit(3);
        }
        return value;
    }

    long long readLong(long long minimum, long long maximum, const char*) {
        long long value = 0;
        if (!(input_ >> value) || value < minimum || value > maximum) {
            std::exit(3);
        }
        return value;
    }

private:
    std::ifstream input_;
};

static TestStream inf;
static TestStream ouf;
static TestStream ans;

inline void registerTestlibCmd(int argc, char** argv) {
    if (argc != 4) {
        std::exit(3);
    }
    inf.open(argv[1]);
    ouf.open(argv[2]);
    ans.open(argv[3]);
}

[[noreturn]] inline void quitf(TResult result, const char*) {
    std::exit(static_cast<int>(result));
}

[[noreturn]] inline void quitp(int points, const char* message) {
    std::cout << points << ' ' << message;
    std::exit(7);
}
)TESTLIB");
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "failed to read " + path.string());
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string cpp_string(const fs::path& path) {
    std::string value = path.string();
    std::string escaped;
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

void create_problem(const fs::path& contest, const std::string& problem, const std::string& answer,
                    const std::string& checker = "token") {
    const fs::path root = contest / "tests" / problem;
    write_file(root / "problem.conf", "time_limit_ms=1000\n"
                                      "memory_limit_mb=256\n"
                                      "default_points=2\n"
                                      "checker=" +
                                          checker + "\n");
    write_file(root / "1" / (problem + ".inp"), "\n");
    write_file(root / "1" / (problem + ".out"), answer);
}

neothemis::JudgeOptions judge_options(const fs::path& contest,
                                      neothemis::ExecutionSecurity security) {
    neothemis::JudgeOptions options;
    options.contest_root = contest;
    options.compiler = NEOTHEMIS_TEST_COMPILER;
#ifdef __linux__
    options.compile_flags = "-std=c++14 -O0 -pthread";
#else
    options.compile_flags = "-std=c++14 -O0";
#endif
    options.parallel_jobs = 2;
    options.execution_security = security;
    return options;
}

const neothemis::TestResult& find_result(const std::vector<neothemis::TestResult>& results,
                                         const std::string& contestant,
                                         const std::string& problem) {
    auto found = std::find_if(results.begin(), results.end(), [&](const auto& result) {
        return result.contestant == contestant && result.problem == problem;
    });
    require(found != results.end(), "missing result for " + contestant + "/" + problem);
    return *found;
}

const neothemis::TestResult& find_test_result(
    const std::vector<neothemis::TestResult>& results,
    const std::string& contestant,
    const std::string& problem,
    const std::string& test) {
    auto found = std::find_if(results.begin(), results.end(), [&](const auto& result) {
        return result.contestant == contestant && result.problem == problem &&
               result.test == test;
    });
    require(found != results.end(),
            "missing result for " + contestant + "/" + problem + "/" + test);
    return *found;
}

std::vector<fs::path> judge_run_directories(const fs::path& contest) {
    std::vector<fs::path> runs;
    const fs::path work_root = contest / ".neothemis-work";
    if (!fs::exists(work_root)) {
        return runs;
    }
    for (const auto& entry : fs::directory_iterator(work_root)) {
        if (entry.is_directory() && entry.path().filename().string().rfind("run-", 0) == 0) {
            runs.push_back(entry.path());
        }
    }
    std::sort(runs.begin(), runs.end());
    return runs;
}

void test_judge_and_sandbox() {
    TemporaryDirectory temporary("neothemis-core-security-test");
    const fs::path contest = temporary.path() / "contest with spaces";
    const fs::path secret = temporary.path() / "host-secret.txt";
    const fs::path marker = temporary.path() / "host-marker.txt";
    const fs::path secret_header = temporary.path() / "host-secret.hpp";
    write_file(secret, "host-secret-value");
    write_file(secret_header, "#define EXFILTRATED_SECRET \"must-not-compile\"\n");

#ifdef __linux__
    create_problem(contest, "SAFE", "00000000\n");
    create_problem(contest, "THREAD", "42\n");
    create_problem(contest, "MEMORY", "unreachable\n");
    write_file(contest / "tests" / "MEMORY" / "problem.conf",
               "time_limit_ms=2000\n"
               "memory_limit_mb=48\n"
               "default_points=2\n"
               "checker=token\n");
    create_problem(contest, "SELFKILL", "unreachable\n");
    create_problem(contest, "LOUD", "unreachable\n");
#else
    create_problem(contest, "SAFE", "safe\n");
#endif
    create_problem(contest, "LEAK", "unreachable\n");
    create_problem(contest, "CUSTOM", "42\n", "custom");
    write_file(contest / "tests" / "CUSTOM" / "checker.cpp",
               "#include <fstream>\n"
               "#include <string>\n"
               "int main(int argc,char** argv){"
               "if(argc!=4)return 3;"
               "std::ifstream host(\"" +
                   cpp_string(secret) +
                   "\");"
                   "if(host.good())return 3;"
                   "std::ifstream actual(argv[2]);std::string value;actual>>value;"
                   "return value==\"42\"?0:1;}\n");

    const fs::path contestant = contest / "contestants" / "Attacker";
    write_file(contestant / "SAFE.cpp",
#ifdef __linux__
               "#include <cerrno>\n"
               "#include <fstream>\n"
               "#include <sys/inotify.h>\n"
               "#include <sys/shm.h>\n"
               "#include <sys/socket.h>\n"
               "#include <sys/syscall.h>\n"
               "#include <unistd.h>\n"
               "int main(){"
               "std::ifstream secret(\"" +
                   cpp_string(secret) +
                   "\");"
                   "std::ofstream marker(\"" +
                   cpp_string(marker) +
                   "\");"
                   "std::ofstream root(\"/rootfile\");std::ofstream dev(\"/dev/neothemis\");"
                   "int network=socket(AF_INET,SOCK_STREAM,0);int child=fork();"
                   "if(child==0)_exit(0);"
                   "int memory_fd=syscall(SYS_memfd_create,\"escape\",0);"
                   "int shared=shmget(IPC_PRIVATE,4096,IPC_CREAT|0600);"
                   "long synced=syscall(SYS_sync);int notify=inotify_init1(IN_CLOEXEC);"
                   "std::ofstream out(\"SAFE.out\");"
                   "out<<(secret.good()?'1':'0')<<(marker.good()?'1':'0')"
                   "<<(root.good()?'1':'0')<<(dev.good()?'1':'0')"
                   "<<(network>=0?'1':'0')<<(child>=0?'1':'0')"
                   "<<(memory_fd>=0||shared>=0?'1':'0')"
                   "<<(synced>=0||notify>=0?'1':'0');}\n");
#else
               "#include <fstream>\n"
               "int main(){std::ofstream(\"SAFE.out\")<<\"safe\";}\n");
#endif
    write_file(contestant / "LEAK.cpp",
               "#include \"" + cpp_string(secret_header) +
                   "\"\n"
                   "#include <fstream>\n"
                   "int main(){std::ofstream(\"LEAK.out\")<<EXFILTRATED_SECRET;}\n");
    write_file(contestant / "CUSTOM.cpp", "#include <fstream>\n"
                                          "int main(){std::ofstream(\"CUSTOM.out\")<<42;}\n");
#ifdef __linux__
    write_file(contestant / "THREAD.cpp",
               "#include <atomic>\n"
               "#include <fstream>\n"
               "#include <pthread.h>\n"
               "#include <sched.h>\n"
               "#include <thread>\n"
               "#include <vector>\n"
               "std::atomic<bool> release_threads{false};\n"
               "void* wait_for_release(void*){while(!release_threads.load())sched_yield();return nullptr;}\n"
               "int main(){int value=0;std::thread worker([&](){value=42;});worker.join();"
               "pthread_attr_t attr;pthread_attr_init(&attr);"
               "pthread_attr_setstacksize(&attr,64*1024);std::vector<pthread_t> threads(128);"
               "int count=0;for(;count<128;++count){if(pthread_create(&threads[count],&attr,"
               "wait_for_release,nullptr)!=0)break;}pthread_attr_destroy(&attr);"
               "cpu_set_t expanded;CPU_ZERO(&expanded);for(int i=0;i<CPU_SETSIZE;++i)CPU_SET(i,&expanded);"
               "int escaped=sched_setaffinity(0,sizeof(expanded),&expanded);"
               "cpu_set_t cpus;CPU_ZERO(&cpus);int affinity=sched_getaffinity(0,sizeof(cpus),&cpus);"
               "release_threads=true;for(int i=0;i<count;++i)pthread_join(threads[i],nullptr);"
               "std::ofstream(\"THREAD.out\")"
               "<<((value==42&&count<128&&escaped!=0&&affinity==0&&CPU_COUNT(&cpus)==1)?42:0);}\n");
    write_file(contestant / "MEMORY.cpp",
               "#include <chrono>\n"
               "#include <new>\n"
               "#include <thread>\n"
               "#include <vector>\n"
               "int main(){std::vector<char*> blocks;try{for(;;){char* p=new char[256*1024];"
               "p[0]=1;blocks.push_back(p);}}catch(const std::bad_alloc&){"
               "std::this_thread::sleep_for(std::chrono::milliseconds(50));return 2;}}\n");
    write_file(contestant / "SELFKILL.cpp",
               "#include <csignal>\nint main(){std::raise(SIGKILL);}\n");
    write_file(contestant / "LOUD.cpp",
               "#include <iostream>\nint main(){for(int i=0;i<200000;++i)"
               "std::cerr<<'x';return 2;}\n");
#endif

#ifdef __linux__
    std::string reason;
    const bool sandbox_available = neothemis::secure_sandbox_available(&reason);
    require(sandbox_available,
            "required Linux sandbox unavailable: " + reason);
    int probe_pipe[2] = {-1, -1};
    require(pipe(probe_pipe) == 0, "failed to create closed-stdio sandbox probe pipe");
    const pid_t probe_pid = fork();
    require(probe_pid >= 0, "failed to fork closed-stdio sandbox probe");
    if (probe_pid == 0) {
        close(probe_pipe[0]);
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        std::string closed_reason;
        const char result = neothemis::secure_sandbox_available(&closed_reason) ? '1' : '0';
        (void)write(probe_pipe[1], &result, 1);
        _exit(result == '1' ? 0 : 1);
    }
    close(probe_pipe[1]);
    char closed_probe_result = '0';
    const ssize_t probe_bytes = read(probe_pipe[0], &closed_probe_result, 1);
    close(probe_pipe[0]);
    int probe_status = 0;
    require(waitpid(probe_pid, &probe_status, 0) == probe_pid,
            "failed to reap closed-stdio sandbox probe");
    require(probe_bytes == 1 && closed_probe_result == '1' && WIFEXITED(probe_status) &&
                WEXITSTATUS(probe_status) == 0,
            "secure sandbox failed when standard descriptors were initially closed");
    const auto security = neothemis::ExecutionSecurity::Required;
#else
    std::string reason;
    require(!neothemis::secure_sandbox_available(&reason),
            "unsupported platform unexpectedly reported a secure sandbox");
    const auto security = neothemis::ExecutionSecurity::ExplicitlyUnsafe;
#endif

    neothemis::JudgeOptions options = judge_options(contest, security);
    std::vector<neothemis::TestResult> callbacks;
    std::mutex callback_mutex;
    options.result = [&](const neothemis::TestResult& result) {
        std::lock_guard<std::mutex> lock(callback_mutex);
        callbacks.push_back(result);
    };
    auto results = neothemis::make_judge_core("builtin")->judge(options);
#ifdef __linux__
    require(results.size() == 7, "expected seven judge rows");
#else
    require(results.size() == 3, "expected three judge rows");
#endif
    require(callbacks.size() == results.size(), "result callback count mismatch");
    const auto& safe = find_result(results, "Attacker", "SAFE");
    require(safe.verdict == neothemis::Verdict::Accepted,
            "sandboxed submission did not produce the expected isolation result: exit=" +
                std::to_string(safe.exit_code) + " verdict=" +
                neothemis::to_string(safe.verdict) + " message=" + safe.message);
#ifdef __linux__
    const auto& leak = find_result(results, "Attacker", "LEAK");
    require(leak.verdict == neothemis::Verdict::CompileError,
            "absolute host include escaped the compiler sandbox");
    require(leak.message.find("must-not-compile") == std::string::npos,
            "compiler diagnostic leaked host-only file contents");
    require(find_result(results, "Attacker", "CUSTOM").verdict == neothemis::Verdict::Accepted,
            "sandboxed custom checker failed");
    const auto& thread_result = find_result(results, "Attacker", "THREAD");
    require(thread_result.verdict == neothemis::Verdict::Accepted,
            "sandbox pthread compatibility or CPU/task limits regressed: " +
                neothemis::to_string(thread_result.verdict) + " " + thread_result.message);
    const auto& memory = find_result(results, "Attacker", "MEMORY");
    require(memory.verdict == neothemis::Verdict::MemoryLimitExceeded,
            "observed address-space exhaustion was not classified as MLE: exit=" +
                std::to_string(memory.exit_code) + " verdict=" +
                neothemis::to_string(memory.verdict) + " message=" + memory.message);
    require(find_result(results, "Attacker", "SELFKILL").verdict ==
                neothemis::Verdict::RuntimeError,
            "a deliberate SIGKILL was incorrectly classified as MLE");
    const auto& loud = find_result(results, "Attacker", "LOUD");
    require(loud.verdict == neothemis::Verdict::RuntimeError &&
                loud.message.size() < 66 * 1024 &&
                loud.message.find("diagnostic truncated") != std::string::npos,
            "runtime diagnostics were not bounded before entering judge results");
    require(!fs::exists(marker), "submission wrote outside its private filesystem");
    require(read_file(secret) == "host-secret-value", "submission modified the host secret");
#endif
}

void test_workdir_ownership_and_concurrency() {
    TemporaryDirectory temporary("neothemis-workdir-test");
    const fs::path contest = temporary.path() / "contest";
    create_problem(contest, "A", "7\n");
    write_file(contest / "contestants" / "User" / "A.cpp",
               "#include <fstream>\nint main(){std::ofstream(\"A.out\")<<7;}\n");

#ifdef __linux__
    const auto security = neothemis::ExecutionSecurity::Required;
#else
    const auto security = neothemis::ExecutionSecurity::ExplicitlyUnsafe;
#endif
    neothemis::JudgeOptions retained_options = judge_options(contest, security);
    retained_options.keep_workdir = true;
    auto retained_results = neothemis::make_judge_core("builtin")->judge(retained_options);
    require(find_result(retained_results, "User", "A").verdict == neothemis::Verdict::Accepted,
            "retained judge run failed");

    std::vector<fs::path> retained_runs = judge_run_directories(contest);
    require(retained_runs.size() == 1, "expected one retained work directory");
    write_file(retained_runs.front() / "owner.marker", "owned");

    neothemis::JudgeOptions cleanup_options = judge_options(contest, security);
    auto cleanup_results = neothemis::make_judge_core("builtin")->judge(cleanup_options);
    require(find_result(cleanup_results, "User", "A").verdict == neothemis::Verdict::Accepted,
            "cleanup judge run failed");
    require(read_file(retained_runs.front() / "owner.marker") == "owned",
            "one judge run removed another run's work directory");
    require(judge_run_directories(contest) == retained_runs,
            "a successful transient judge run left its invocation work directory behind");

    neothemis::JudgeOptions exception_options = judge_options(contest, security);
    exception_options.result = [](const neothemis::TestResult&) {
        throw std::runtime_error("intentional result callback failure");
    };
    bool callback_failed = false;
    try {
        (void)neothemis::make_judge_core("builtin")->judge(exception_options);
    } catch (const std::runtime_error& ex) {
        callback_failed = std::string(ex.what()).find("intentional result callback failure") !=
                          std::string::npos;
    }
    require(callback_failed, "judge did not propagate the intentional callback failure");
    require(judge_run_directories(contest) == retained_runs,
            "an exceptional judge run left its invocation work directory behind");
    require(read_file(retained_runs.front() / "owner.marker") == "owned",
            "exception cleanup removed another invocation's retained work");

    std::atomic<bool> cancellation_requested{false};
    neothemis::JudgeOptions cancellation_options = judge_options(contest, security);
    cancellation_options.result = [&](const neothemis::TestResult&) {
        cancellation_requested.store(true);
    };
    cancellation_options.should_cancel = [&] {
        return cancellation_requested.load();
    };
    bool cancellation_reported = false;
    try {
        (void)neothemis::make_judge_core("builtin")->judge(cancellation_options);
    } catch (const std::runtime_error& ex) {
        cancellation_reported =
            std::string(ex.what()).find("judging cancelled") != std::string::npos;
    }
    require(cancellation_reported,
            "a cancellation between test jobs returned placeholder result rows");
    require(judge_run_directories(contest) == retained_runs,
            "a cancelled judge run left its invocation work directory behind");

    auto first = std::async(std::launch::async, [=]() {
        return neothemis::make_judge_core("builtin")->judge(judge_options(contest, security));
    });
    auto second = std::async(std::launch::async, [=]() {
        return neothemis::make_judge_core("builtin")->judge(judge_options(contest, security));
    });
    require(find_result(first.get(), "User", "A").verdict == neothemis::Verdict::Accepted,
            "first concurrent judge failed");
    require(find_result(second.get(), "User", "A").verdict == neothemis::Verdict::Accepted,
            "second concurrent judge failed");
    require(fs::exists(retained_runs.front() / "owner.marker"),
            "concurrent cleanup removed retained work");
    require(judge_run_directories(contest) == retained_runs,
            "concurrent transient judge runs left invocation work directories behind");
}

void test_artifact_namespace_collisions() {
    TemporaryDirectory temporary("neothemis-artifact-namespace-test");
    const fs::path contest = temporary.path() / "contest";

    // This used to place the executable and the test work directory at the
    // identical path: <run>/checkers/1/1.
    create_problem(contest, "1", "1\n");

    const fs::path colliding_tests = contest / "tests" / "COLLIDE";
    write_file(colliding_tests / "problem.conf",
               "time_limit_ms=1000\n"
               "memory_limit_mb=256\n"
               "default_points=2\n"
               "checker=token\n");
    for (const std::string& test : {std::string("x"), std::string("x.run.err")}) {
        write_file(colliding_tests / test / "COLLIDE.inp", "\n");
        write_file(colliding_tests / test / "COLLIDE.out", "9\n");
    }

    const fs::path custom_tests = contest / "tests" / "CUSTOM";
    write_file(custom_tests / "problem.conf",
               "time_limit_ms=1000\n"
               "memory_limit_mb=256\n"
               "default_points=2\n"
               "checker=custom\n");
    write_file(custom_tests / "checker" / "CUSTOM.inp", "\n");
    write_file(custom_tests / "checker" / "CUSTOM.out", "ignored\n");
    write_file(custom_tests / "checker.cpp",
               "#include <fstream>\n"
               "int main(int argc,char** argv){if(argc!=4)return 3;"
               "std::ifstream actual(argv[2]);int value=0;actual>>value;"
               "return value==42?0:1;}\n");

    const fs::path contestant = contest / "contestants" / "checkers";
    write_file(contestant / "1.cpp",
               "#include <fstream>\nint main(){std::ofstream(\"1.out\")<<1;}\n");
    write_file(contestant / "COLLIDE.cpp",
               "#include <fstream>\nint main(){std::ofstream(\"COLLIDE.out\")<<9;}\n");
    write_file(contestant / "CUSTOM.cpp",
               "#include <fstream>\nint main(){std::ofstream(\"CUSTOM.out\")<<42;}\n");

    neothemis::JudgeOptions options =
        judge_options(contest, neothemis::ExecutionSecurity::ExplicitlyUnsafe);
    options.parallel_jobs = 1;
    options.keep_workdir = true;
    const auto results = neothemis::make_judge_core("builtin")->judge(options);
    require(results.size() == 4, "artifact collision contest returned the wrong row count");
    require(find_test_result(results, "checkers", "1", "1").verdict ==
                neothemis::Verdict::Accepted,
            "problem=test=1 replaced the submission executable");
    require(find_test_result(results, "checkers", "COLLIDE", "x").verdict ==
                    neothemis::Verdict::Accepted &&
                find_test_result(results, "checkers", "COLLIDE", "x.run.err").verdict ==
                    neothemis::Verdict::Accepted,
            "test x collided with test x.run.err artifacts");
    require(find_test_result(results, "checkers", "CUSTOM", "checker").verdict ==
                neothemis::Verdict::Accepted,
            "contestant checkers collided with the internal custom checker");

    const std::vector<fs::path> runs = judge_run_directories(contest);
    require(runs.size() == 1, "expected one retained artifact namespace run");
    const fs::path& run = runs.front();
    const fs::path submissions = run / "submissions" / "checkers";
    require(fs::is_regular_file(submissions / "1" / "build" /
#ifdef _WIN32
                                "program.exe"
#else
                                "program"
#endif
                                ),
            "submission executable was not isolated in its build namespace");
    require(fs::is_directory(submissions / "1" / "tests" / "1" / "work"),
            "problem=test=1 did not retain a distinct test work namespace");
    require(fs::is_directory(submissions / "COLLIDE" / "tests" / "x" / "work") &&
                fs::is_regular_file(submissions / "COLLIDE" / "tests" / "x" / "run.err") &&
                fs::is_directory(submissions / "COLLIDE" / "tests" / "x.run.err" / "work") &&
                fs::is_regular_file(submissions / "COLLIDE" / "tests" / "x.run.err" /
                                    "run.err"),
            "colliding test names did not retain independent run and log artifacts");
    const fs::path checker = run / "internal" / "checkers" / "CUSTOM" / "build" /
#ifdef _WIN32
                             "checker.exe";
#else
                             "checker";
#endif
    require(fs::is_regular_file(checker),
            "custom checker was not isolated in the internal artifact namespace");
    require(fs::is_regular_file(submissions / "CUSTOM" / "build" /
#ifdef _WIN32
                                "program.exe"
#else
                                "program"
#endif
                                ) &&
                !fs::equivalent(checker, submissions / "CUSTOM" / "build" /
#ifdef _WIN32
                                             "program.exe"
#else
                                             "program"
#endif
                                             ),
            "custom checker and contestant executable share an artifact path");
}

void test_custom_checker_partial_points() {
    TemporaryDirectory temporary("neothemis-checker-points-test");
    const fs::path contest = temporary.path() / "contest";
    const fs::path percent_problem = contest / "tests" / "PERCENT";

    write_file(percent_problem / "problem.conf",
               "time_limit_ms=1000\n"
               "memory_limit_mb=256\n"
               "default_points=8\n"
               "checker=testlib:percent.cpp\n");
    write_file(percent_problem / "percent.cpp",
               "#include \"testlib.h\"\n"
               "int main(int argc,char** argv){registerTestlibCmd(argc,argv);"
               "int percentage=ouf.readInt();quitp(percentage,\"percentage score\");}\n");
    write_testlib_protocol_fixture(percent_problem / "testlib.h");
    for (const auto& [test, percentage] :
         std::vector<std::pair<std::string, int>>{{"zero", 0}, {"over", 150}}) {
        write_file(percent_problem / test / "PERCENT.inp",
                   std::to_string(percentage) + "\n");
        write_file(percent_problem / test / "PERCENT.out", "unused\n");
    }

    const fs::path example_problem = contest / "tests" / "EXAMPLE";
    write_file(example_problem / "problem.conf",
               "time_limit_ms=1000\n"
               "memory_limit_mb=256\n"
               "default_points=8\n"
               "checker=testlib\n");
    write_file(example_problem / "checker.cpp",
               "#include \"testlib.h\"\n"
               "int main(int argc,char** argv){registerTestlibCmd(argc,argv);"
               "long long a=ouf.readLong(-1000000000000000000LL,"
               "1000000000000000000LL,\"a\");"
               "long long b=ouf.readLong(-1000000000000000000LL,"
               "1000000000000000000LL,\"b\");"
               "long long expected_a=ans.readLong(-1000000000000000000LL,"
               "1000000000000000000LL,\"expected a\");"
               "long long expected_b=ans.readLong(-1000000000000000000LL,"
               "1000000000000000000LL,\"expected b\");"
               "if(a!=expected_a)quitf(_wa,\"wrong first value\");"
               "if(b!=expected_b)quitp(50,\"partial second value\");"
               "quitf(_ok,\"correct\");}\n");
    write_testlib_protocol_fixture(example_problem / "testlib.h");
    write_file(example_problem / "half" / "EXAMPLE.inp", "\n");
    write_file(example_problem / "half" / "EXAMPLE.out", "1 2\n");

    const fs::path absolute_problem = contest / "tests" / "ABSOLUTE";
    write_file(absolute_problem / "problem.conf",
               "time_limit_ms=1000\n"
               "memory_limit_mb=256\n"
               "default_points=8\n"
               "checker=custom:legacy.cpp\n");
    write_file(absolute_problem / "legacy.cpp",
               "#include <iostream>\n"
               "int main(){std::cerr<<\"3 legacy absolute score\";return 7;}\n");
    write_file(absolute_problem / "one" / "ABSOLUTE.inp", "\n");
    write_file(absolute_problem / "one" / "ABSOLUTE.out", "unused\n");

    const fs::path contestant = contest / "contestants" / "Scored";
    write_file(contestant / "PERCENT.cpp",
               "#include <fstream>\n"
               "int main(){int value=0;std::ifstream(\"PERCENT.inp\")>>value;"
               "std::ofstream(\"PERCENT.out\")<<value;}\n");
    write_file(contestant / "ABSOLUTE.cpp",
               "#include <fstream>\n"
               "int main(){std::ofstream(\"ABSOLUTE.out\")<<0;}\n");
    write_file(contestant / "EXAMPLE.cpp",
               "#include <fstream>\n"
               "int main(){std::ofstream(\"EXAMPLE.out\")<<\"1 999\";}\n");

    neothemis::JudgeOptions options = judge_options(
        contest,
#ifdef __linux__
        neothemis::ExecutionSecurity::Required
#else
        neothemis::ExecutionSecurity::ExplicitlyUnsafe
#endif
    );
    options.parallel_jobs = 1;
    const auto results = neothemis::make_judge_core("builtin")->judge(options);
    require(results.size() == 4, "partial checker fixture returned the wrong row count");

    const auto& zero = find_test_result(results, "Scored", "PERCENT", "zero");
    const auto& half = find_test_result(results, "Scored", "EXAMPLE", "half");
    const auto& over = find_test_result(results, "Scored", "PERCENT", "over");
    const auto& absolute = find_test_result(results, "Scored", "ABSOLUTE", "one");
    require(zero.exit_code == 7 && zero.verdict == neothemis::Verdict::WrongAnswer &&
                zero.earned_points == 0.0,
            "testlib quitp(0) did not produce zero-point WA: exit=" +
                std::to_string(zero.exit_code) + " verdict=" +
                neothemis::to_string(zero.verdict) + " points=" +
                std::to_string(zero.earned_points) + " message=" + zero.message);
    require(half.exit_code == 7 && half.verdict == neothemis::Verdict::Partial &&
                half.earned_points == 4.0,
            "examples/checker.cpp quitp(50) did not award half of the configured test points");
    require(over.exit_code == 7 && over.verdict == neothemis::Verdict::Accepted &&
                over.earned_points == 8.0,
            "testlib quitp above 100 was not clamped to full points");
    require(absolute.exit_code == 7 && absolute.verdict == neothemis::Verdict::Partial &&
                absolute.earned_points == 3.0,
            "legacy custom exit-7 checker stopped using absolute points");
    require(neothemis::to_string(neothemis::Verdict::Partial) == "PC" &&
                neothemis::verdict_from_string("PC") == neothemis::Verdict::Partial,
            "partial verdict did not round-trip through its persistent code");

    std::ostringstream csv;
    neothemis::write_csv(csv, results);
    const neothemis::CsvTable rows = neothemis::parse_csv(csv.str());
    require(std::any_of(rows.begin(), rows.end(), [](const auto& row) {
                return row.size() >= 8 && row[1] == "EXAMPLE" && row[2] == "half" &&
                       row[3] == "PC" && row[7] == "4";
            }),
            "partial verdict or decimal score was lost in detailed CSV output");
}

void test_symlinked_contest_entries_are_not_followed() {
#ifndef _WIN32
    TemporaryDirectory temporary("neothemis-symlinked-entries-test");
    const fs::path contest = temporary.path() / "contest";
    const fs::path outside = temporary.path() / "outside";

    auto source_for = [](const std::string& problem) {
        return "#include <fstream>\nint main(){std::ofstream(\"" + problem +
               ".out\")<<7;}\n";
    };

    for (const std::string& problem : {"GOOD", "EVILSOURCE", "CHECKER", "TESTLIB",
                                       "INPUT", "ANSWER"}) {
        create_problem(contest, problem, "7\n",
                       (problem == "CHECKER" || problem == "TESTLIB")
                           ? "custom"
                           : "token");
    }
    for (const std::string& problem : {"GOOD", "CHECKER", "TESTLIB", "INPUT", "ANSWER",
                                       "EVILPROBLEM"}) {
        write_file(contest / "contestants" / "Real" / (problem + ".cpp"),
                   source_for(problem));
    }

    create_problem(outside / "outside-contest", "EVILPROBLEM", "7\n");
    fs::create_directory_symlink(outside / "outside-contest" / "tests" / "EVILPROBLEM",
                                 contest / "tests" / "EVILPROBLEM");

    write_file(outside / "outside-contestant" / "GOOD.cpp", source_for("GOOD"));
    fs::create_directory_symlink(outside / "outside-contestant",
                                 contest / "contestants" / "Symlinked");

    write_file(outside / "EVILSOURCE.cpp", source_for("EVILSOURCE"));
    fs::create_symlink(outside / "EVILSOURCE.cpp",
                       contest / "contestants" / "Real" / "EVILSOURCE.cpp");

    write_file(outside / "checker.cpp", "int main(){return 0;}\n");
    fs::create_symlink(outside / "checker.cpp",
                       contest / "tests" / "CHECKER" / "checker.cpp");

    write_file(contest / "tests" / "TESTLIB" / "checker.cpp",
               "#include \"testlib.h\"\nint main(){return 0;}\n");
    write_file(outside / "testlib.h", "// outside testlib must not be included\n");
    fs::create_symlink(outside / "testlib.h",
                       contest / "tests" / "TESTLIB" / "testlib.h");

    write_file(outside / "INPUT.inp", "outside input\n");
    fs::remove(contest / "tests" / "INPUT" / "1" / "INPUT.inp");
    fs::create_symlink(outside / "INPUT.inp",
                       contest / "tests" / "INPUT" / "1" / "INPUT.inp");

    write_file(outside / "ANSWER.out", "7\n");
    fs::remove(contest / "tests" / "ANSWER" / "1" / "ANSWER.out");
    fs::create_symlink(outside / "ANSWER.out",
                       contest / "tests" / "ANSWER" / "1" / "ANSWER.out");

    write_file(outside / "escaped-test" / "GOOD.inp", "\n");
    write_file(outside / "escaped-test" / "GOOD.out", "7\n");
    fs::create_directory_symlink(outside / "escaped-test",
                                 contest / "tests" / "GOOD" / "escaped-test");

    neothemis::JudgeOptions options =
        judge_options(contest, neothemis::ExecutionSecurity::ExplicitlyUnsafe);
    const neothemis::ContestOverview overview = neothemis::inspect_contest(options);
    require(std::find(overview.problems.begin(), overview.problems.end(), "EVILPROBLEM") ==
                overview.problems.end(),
            "contest inspection followed a symlinked problem directory");
    require(std::find(overview.contestants.begin(), overview.contestants.end(), "Symlinked") ==
                overview.contestants.end(),
            "contest inspection followed a symlinked contestant directory");

    const auto real_contestant =
        std::find(overview.contestants.begin(), overview.contestants.end(), "Real");
    const auto evil_source_problem =
        std::find(overview.problems.begin(), overview.problems.end(), "EVILSOURCE");
    const auto good_problem =
        std::find(overview.problems.begin(), overview.problems.end(), "GOOD");
    require(real_contestant != overview.contestants.end() &&
                evil_source_problem != overview.problems.end() &&
                good_problem != overview.problems.end(),
            "contest inspection omitted normal entries in the symlink regression fixture");
    const std::size_t contestant_index =
        static_cast<std::size_t>(real_contestant - overview.contestants.begin());
    const std::size_t evil_source_index =
        static_cast<std::size_t>(evil_source_problem - overview.problems.begin());
    const std::size_t good_index =
        static_cast<std::size_t>(good_problem - overview.problems.begin());
    require(!overview.has_source[contestant_index][evil_source_index] &&
                overview.has_source[contestant_index][good_index],
            "contest inspection followed a symlinked source or rejected a normal source");

    const auto results = neothemis::make_judge_core("builtin")->judge(options);
    require(std::none_of(results.begin(), results.end(), [](const auto& result) {
                return result.problem == "EVILPROBLEM" || result.contestant == "Symlinked";
            }),
            "judge followed a symlinked problem or contestant directory");
    require(std::count_if(results.begin(), results.end(), [](const auto& result) {
                return result.contestant == "Real" && result.problem == "GOOD";
            }) == 1,
            "judge followed a symlinked test directory");
    require(find_result(results, "Real", "GOOD").verdict == neothemis::Verdict::Accepted,
            "normal files stopped working after entry containment hardening");
    require(find_result(results, "Real", "EVILSOURCE").verdict ==
                neothemis::Verdict::MissingSource,
            "judge followed a symlinked contestant source");
    require(find_result(results, "Real", "CHECKER").verdict ==
                neothemis::Verdict::InternalError,
            "judge followed a symlinked custom checker");
    require(find_result(results, "Real", "TESTLIB").verdict ==
                neothemis::Verdict::InternalError,
            "judge followed a symlinked testlib header");
    require(find_result(results, "Real", "INPUT").verdict ==
                neothemis::Verdict::InternalError,
            "judge followed a symlinked test input");
    require(find_result(results, "Real", "ANSWER").verdict ==
                neothemis::Verdict::InternalError,
            "judge followed a symlinked test answer");

    const fs::path config_contest = temporary.path() / "config-contest";
    create_problem(config_contest, "CONFIG", "7\n");
    write_file(config_contest / "contestants" / "Real" / "CONFIG.cpp",
               source_for("CONFIG"));
    write_file(outside / "problem.conf",
               "time_limit_ms=1000\nmemory_limit_mb=256\ndefault_points=1\nchecker=token\n");
    fs::remove(config_contest / "tests" / "CONFIG" / "problem.conf");
    fs::create_symlink(outside / "problem.conf",
                       config_contest / "tests" / "CONFIG" / "problem.conf");
    bool config_symlink_rejected = false;
    try {
        (void)neothemis::make_judge_core("builtin")->judge(
            judge_options(config_contest,
                          neothemis::ExecutionSecurity::ExplicitlyUnsafe));
    } catch (const std::exception&) {
        config_symlink_rejected = true;
    }
    require(config_symlink_rejected,
            "judge followed a symlinked problem configuration");
#endif
}

void test_unicode_process_paths() {
    TemporaryDirectory temporary("neothemis-unicode-path-test");
    const std::string contestant_name = u8"Người-dự-thi-测试";
    const std::string problem_name = u8"Bài-toán-算法";
    const fs::path contest = temporary.path() / fs::u8path(u8"cuộc-thi-竞赛");
    const fs::path problem_component = fs::u8path(problem_name);
    const fs::path test_root = contest / "tests" / problem_component / "1";

    write_file(contest / "tests" / problem_component / "problem.conf",
               "time_limit_ms=1000\n"
               "memory_limit_mb=256\n"
               "default_points=1\n"
               "checker=token\n");
    write_file(test_root / fs::u8path(problem_name + ".inp"), "\n");
    write_file(test_root / fs::u8path(problem_name + ".out"), "17\n");
    write_file(contest / "contestants" / fs::u8path(contestant_name) /
                   fs::u8path(problem_name + ".cpp"),
               "#include <fstream>\n"
               "int main(){std::ofstream(\"1.out\")<<17;}\n");

    neothemis::JudgeOptions options =
        judge_options(contest, neothemis::ExecutionSecurity::ExplicitlyUnsafe);
    options.parallel_jobs = 1;
    const auto results = neothemis::make_judge_core("builtin")->judge(options);
    require(results.size() == 1,
            "Unicode-path contest returned the wrong number of judge rows");
    const auto& result = find_result(results, contestant_name, problem_name);
    require(result.test == "1" && result.verdict == neothemis::Verdict::Accepted,
            "Unicode contest, contestant, problem, or process path was not preserved: test=" +
                result.test + " exit=" + std::to_string(result.exit_code) + " verdict=" +
                neothemis::to_string(result.verdict) + " message=" + result.message);
}

void test_cpu_time_accounting() {
    TemporaryDirectory temporary("neothemis-cpu-time-test");
    const fs::path contest = temporary.path() / "contest";
    constexpr std::uint64_t time_limit_ms = 200;

    auto create_timed_problem = [&](const std::string& problem) {
        create_problem(contest, problem, "ok\n");
        write_file(contest / "tests" / problem / "problem.conf",
                   "time_limit_ms=" + std::to_string(time_limit_ms) + "\n"
                   "memory_limit_mb=256\n"
                   "default_points=2\n"
                   "checker=token\n");
    };
    create_timed_problem("SLEEP");
    create_timed_problem("WORK");
    create_timed_problem("BURN");

    write_file(contest / "contestants" / "Timing" / "SLEEP.cpp",
               "#include <chrono>\n"
               "#include <fstream>\n"
               "#include <thread>\n"
               "int main(){"
               "std::this_thread::sleep_for(std::chrono::milliseconds(1500));"
               "std::ofstream(\"SLEEP.out\")<<\"ok\\n\";"
               "}\n");
    auto cpu_burn_source = [](const std::string& problem, std::uint64_t target_ms) {
        return std::string(
                   "#include <cstdint>\n"
                   "#include <fstream>\n"
                   "#ifdef _WIN32\n"
                   "#include <windows.h>\n"
                   "std::uint64_t cpu_ms(){"
                   "FILETIME creation{},exit{},kernel{},user{};"
                   "if(!GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user))return 0;"
                   "const auto ticks=[](const FILETIME& value){"
                   "return(static_cast<std::uint64_t>(value.dwHighDateTime)<<32)|"
                   "value.dwLowDateTime;};"
                   "return(ticks(kernel)+ticks(user))/10000ULL;"
                   "}\n"
                   "#else\n"
                   "#include <ctime>\n"
                   "std::uint64_t cpu_ms(){"
                   "return static_cast<std::uint64_t>(std::clock())*1000ULL/CLOCKS_PER_SEC;"
                   "}\n"
                   "#endif\n"
                   "int main(){"
                   "volatile unsigned long long value=1;"
                   "const std::uint64_t start=cpu_ms();"
                   "while(cpu_ms()-start<") +
               std::to_string(target_ms) +
               "){value=value*1664525ULL+1013904223ULL;}"
               "std::ofstream(\"" + problem + ".out\")<<\"ok\\n\";"
               "(void)value;return 0;}\n";
    };
    write_file(contest / "contestants" / "Timing" / "WORK.cpp",
               cpu_burn_source("WORK", 80));
    write_file(contest / "contestants" / "Timing" / "BURN.cpp",
               cpu_burn_source("BURN", 600));

#ifdef __linux__
    constexpr auto security = neothemis::ExecutionSecurity::Required;
#else
    constexpr auto security = neothemis::ExecutionSecurity::ExplicitlyUnsafe;
#endif
    neothemis::JudgeOptions options = judge_options(contest, security);
    options.parallel_jobs = 1;
#ifdef __linux__
    int initial_subreaper_state = -1;
    require(prctl(PR_GET_CHILD_SUBREAPER, &initial_subreaper_state) == 0,
            "failed to read the application's initial child-subreaper state");
#endif
    const auto wall_started = std::chrono::steady_clock::now();
    const auto results = neothemis::make_judge_core("builtin")->judge(options);
    const auto wall_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - wall_started)
                                     .count();

#ifdef __linux__
    int parent_is_subreaper = -1;
    require(prctl(PR_GET_CHILD_SUBREAPER, &parent_is_subreaper) == 0 &&
                parent_is_subreaper == initial_subreaper_state,
            "sandbox accounting leaked child-subreaper state into the application");
#endif

    require(results.size() == 3, "CPU-time fixture returned the wrong row count");
    const auto& sleeping = find_result(results, "Timing", "SLEEP");
    require(sleeping.verdict == neothemis::Verdict::Accepted,
            "a sleeping program was charged wall time instead of CPU time");
    require(sleeping.time_ms < 150,
            "reported test time included a sleeping program's wall time");
    require(wall_elapsed_ms >= 1300,
            "CPU-time fixture did not exercise a meaningful wall-time delay");

    const auto& working = find_result(results, "Timing", "WORK");
    require(working.verdict == neothemis::Verdict::Accepted,
            "CPU work below the configured limit was not accepted");
    require(working.time_ms >= 40 && working.time_ms <= time_limit_ms,
            "a completed program's reported CPU time was not preserved");

    const auto& burning = find_result(results, "Timing", "BURN");
    require(burning.verdict == neothemis::Verdict::TimeLimitExceeded,
            "a CPU-burning program did not exceed the CPU-time limit");
    require(burning.time_ms >= time_limit_ms / 2,
            "CPU-burning program reported implausibly little CPU time");

#ifdef __linux__
    const fs::path unsafe_contest = temporary.path() / "unsafe-contest";
    create_problem(unsafe_contest, "ORPHAN", "ok\n");
    write_file(unsafe_contest / "tests" / "ORPHAN" / "problem.conf",
               "time_limit_ms=200\n"
               "memory_limit_mb=256\n"
               "default_points=1\n"
               "checker=token\n");
    write_file(unsafe_contest / "contestants" / "Timing" / "ORPHAN.cpp",
               "#include <ctime>\n"
               "#include <fstream>\n"
               "#include <unistd.h>\n"
               "int main(){pid_t child=fork();if(child<0)return 2;if(child==0){"
               "volatile unsigned long long value=1;const auto start=std::clock();"
               "while((std::clock()-start)*1000/CLOCKS_PER_SEC<80){"
               "value=value*1664525ULL+1013904223ULL;}_exit(value==0);}"
               "usleep(400000);std::ofstream(\"ORPHAN.out\")<<\"ok\\n\";return 0;}\n");
    const fs::path escaped_marker = temporary.path() / "escaped-child.marker";
    create_problem(unsafe_contest, "ESCAPE", "unused\n");
    write_file(unsafe_contest / "tests" / "ESCAPE" / "problem.conf",
               "time_limit_ms=100\n"
               "memory_limit_mb=256\n"
               "default_points=1\n"
               "checker=token\n");
    write_file(unsafe_contest / "contestants" / "Timing" / "ESCAPE.cpp",
               "#include <ctime>\n"
               "#include <fstream>\n"
               "#include <unistd.h>\n"
               "int main(){pid_t child=fork();if(child<0)return 2;if(child==0){setsid();"
               "volatile unsigned long long value=1;const auto start=std::clock();"
               "while((std::clock()-start)*1000/CLOCKS_PER_SEC<500){"
               "value=value*1664525ULL+1013904223ULL;}std::ofstream(\"" +
                   cpp_string(escaped_marker) +
                   "\")<<value;_exit(0);}for(;;)pause();}\n");
    neothemis::JudgeOptions unsafe_options = judge_options(
        unsafe_contest, neothemis::ExecutionSecurity::ExplicitlyUnsafe);
    unsafe_options.parallel_jobs = 1;
    const auto unsafe_results =
        neothemis::make_judge_core("builtin")->judge(unsafe_options);
    const auto& orphan = find_result(unsafe_results, "Timing", "ORPHAN");
    require(orphan.verdict == neothemis::Verdict::Accepted &&
                orphan.time_ms >= 40,
            "unsafe Linux accounting omitted an un-waited child process");
    const auto& escaped = find_result(unsafe_results, "Timing", "ESCAPE");
    require(escaped.verdict == neothemis::Verdict::TimeLimitExceeded &&
                escaped.time_ms >= 50,
            "an unsafe setsid descendant escaped CPU-time enforcement");
    std::this_thread::sleep_for(std::chrono::milliseconds(650));
    require(!fs::exists(escaped_marker),
            "an unsafe setsid descendant survived the judged process tree");
#endif
}

void test_windows_custom_compiler_runtime_path() {
#ifdef _WIN32
    TemporaryDirectory temporary("neothemis-windows-compiler-path-test");
    const fs::path compiler_directory =
        temporary.path() / fs::u8path(u8"custom compiler 工具");
    fs::create_directories(compiler_directory);

    const fs::path fake_compiler =
        compiler_directory / fs::u8path(u8"selected compiler.exe");
    const fs::path fixture_submission =
        compiler_directory / fs::path(NEOTHEMIS_WINDOWS_FIXTURE_SUBMISSION).filename();
    const fs::path fixture_runtime =
        compiler_directory / fs::path(NEOTHEMIS_WINDOWS_FIXTURE_RUNTIME).filename();
    fs::copy_file(fs::path(NEOTHEMIS_WINDOWS_FAKE_COMPILER), fake_compiler);
    fs::copy_file(fs::path(NEOTHEMIS_WINDOWS_FIXTURE_SUBMISSION), fixture_submission);
    fs::copy_file(fs::path(NEOTHEMIS_WINDOWS_FIXTURE_RUNTIME), fixture_runtime);

    const fs::path contest = temporary.path() / "contest";
    create_problem(contest, "TOKEN", "17\n");
    create_problem(contest, "CUSTOM", "ignored\n", "custom");
    write_file(contest / "tests" / "CUSTOM" / "checker.cpp",
               "int main(){return 0;}\n");
    write_file(contest / "contestants" / "User" / "TOKEN.cpp",
               "int main(){return 0;}\n");
    write_file(contest / "contestants" / "User" / "CUSTOM.cpp",
               "int main(){return 0;}\n");

    neothemis::JudgeOptions options =
        judge_options(contest, neothemis::ExecutionSecurity::ExplicitlyUnsafe);
    options.compiler = fake_compiler.u8string();
    options.compile_flags.clear();
    options.stack_limit_mb = 0;
    options.parallel_jobs = 1;
    const char* original_path_value = std::getenv("PATH");
    const bool original_path_present = original_path_value != nullptr;
    const std::string original_path = original_path_value ? original_path_value : "";
    const auto results = neothemis::make_judge_core("builtin")->judge(options);
    const char* final_path_value = std::getenv("PATH");
    require((final_path_value != nullptr) == original_path_present &&
                (final_path_value ? std::string(final_path_value) : "") == original_path,
            "custom compiler handling mutated the application-wide PATH");
    require(results.size() == 2,
            "custom compiler runtime-path fixture returned the wrong row count");
    require(find_result(results, "User", "TOKEN").verdict ==
                neothemis::Verdict::Accepted,
            "a submission could not load runtime DLLs beside a selected compiler");
    require(find_result(results, "User", "CUSTOM").verdict ==
                neothemis::Verdict::Accepted,
            "a custom checker could not load runtime DLLs beside a selected compiler");
#endif
}

void test_windows_descendant_cleanup() {
#ifdef _WIN32
    TemporaryDirectory temporary("neothemis-windows-descendant-test");
    const fs::path contest = temporary.path() / "contest";
    create_problem(contest, "A", "1\n");

    const std::string marker_name =
        "neothemis-descendant-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        ".marker";
    const fs::path marker = fs::temp_directory_path() / marker_name;
    std::error_code marker_error;
    fs::remove(marker, marker_error);
    write_file(
        contest / "contestants" / "Tree" / "A.cpp",
        "#include <windows.h>\n"
        "#include <string>\n"
        "int main(int argc,char**){"
        "if(argc>1){Sleep(700);wchar_t temp[32768];DWORD n=GetTempPathW(32768,temp);"
        "std::wstring path(temp,n);path+=L\"" + marker_name +
            "\";HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,"
            "nullptr);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);return 0;}"
            "wchar_t exe[32768];DWORD n=GetModuleFileNameW(nullptr,exe,32768);"
            "std::wstring command=L\"\\\"\"+std::wstring(exe,n)+L\"\\\" child\";"
            "STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};"
            "if(!CreateProcessW(nullptr,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,"
            "nullptr,&startup,&process))return 2;CloseHandle(process.hThread);"
            "CloseHandle(process.hProcess);return 0;}\n");

    neothemis::JudgeOptions options =
        judge_options(contest, neothemis::ExecutionSecurity::ExplicitlyUnsafe);
    options.parallel_jobs = 1;
    const auto results = neothemis::make_judge_core("builtin")->judge(options);
    require(results.size() == 1,
            "Windows descendant-cleanup fixture returned the wrong row count");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const bool descendant_survived = fs::exists(marker);
    fs::remove(marker, marker_error);
    require(!descendant_survived,
            "a descendant process outlived the judged Windows process tree");
    require(judge_run_directories(contest).empty(),
            "Windows descendant process prevented invocation work cleanup");
#endif
}

void test_windows_descendant_cpu_accounting() {
#ifdef _WIN32
    TemporaryDirectory temporary("neothemis-windows-descendant-cpu-test");
    const fs::path contest = temporary.path() / "contest";
    create_problem(contest, "TREECPU", "1\n");
    write_file(
        contest / "contestants" / "Tree" / "TREECPU.cpp",
        "#include <windows.h>\n"
        "#include <cstdint>\n"
        "#include <fstream>\n"
        "#include <string>\n"
        "std::uint64_t cpu_ms(){FILETIME c{},e{},k{},u{};"
        "GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);"
        "auto ticks=[](const FILETIME& v){return(static_cast<std::uint64_t>(v.dwHighDateTime)"
        "<<32)|v.dwLowDateTime;};return(ticks(k)+ticks(u))/10000ULL;}"
        "int main(int argc,char**){if(argc>1){volatile unsigned long long value=1;"
        "const auto start=cpu_ms();while(cpu_ms()-start<100){"
        "value=value*1664525ULL+1013904223ULL;}return value==0;}"
        "wchar_t exe[32768];DWORD n=GetModuleFileNameW(nullptr,exe,32768);"
        "std::wstring command=L\"\\\"\"+std::wstring(exe,n)+L\"\\\" child\";"
        "STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};"
        "if(!CreateProcessW(nullptr,&command[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,"
        "nullptr,&startup,&process))return 2;CloseHandle(process.hThread);"
        "WaitForSingleObject(process.hProcess,INFINITE);CloseHandle(process.hProcess);"
        "std::ofstream(\"TREECPU.out\")<<1;return 0;}\n");

    neothemis::JudgeOptions options =
        judge_options(contest, neothemis::ExecutionSecurity::ExplicitlyUnsafe);
    options.parallel_jobs = 1;
    const auto results = neothemis::make_judge_core("builtin")->judge(options);
    const auto& result = find_result(results, "Tree", "TREECPU");
    require(result.verdict == neothemis::Verdict::Accepted && result.time_ms >= 50,
            "Windows job CPU accounting omitted a child process");
#endif
}

void test_archive_and_csv() {
    TemporaryDirectory temporary("neothemis-archive-test");
    const fs::path source = temporary.path() / "source";
    const fs::path archive = temporary.path() / "contest.ncontest";
    const fs::path extracted = temporary.path() / "extracted";
    write_file(source / "nested dir" / "binary.dat", std::string("a\0b", 3));
    write_file(source / ".neothemis-work" / "ignored.txt", "ignored");
    write_file(source / "results.csv.neothemis-lock", "");
    neothemis::write_zip_archive_from_directory(archive, source);
    neothemis::extract_zip_archive(archive, extracted);
    require(read_file(extracted / "nested dir" / "binary.dat") == std::string("a\0b", 3),
            "archive round trip changed binary data");
    require(!fs::exists(extracted / ".neothemis-work"), "archive included transient judge work");
    require(!fs::exists(extracted / "results.csv.neothemis-lock"),
            "archive included a transient cross-process CSV lock");

    neothemis::TestResult result;
    result.contestant = "A,\"B";
    result.problem = "P";
    result.test = "1";
    result.verdict = neothemis::Verdict::CompileError;
    result.message = "line one\nline \"two\"";
    std::ostringstream csv;
    neothemis::write_csv(csv, {result});
    require(csv.str().find("\"A,\"\"B\"") != std::string::npos,
            "CSV did not escape a quoted comma field");
    require(csv.str().find("\"line one\nline \"\"two\"\"\"") != std::string::npos,
            "CSV did not escape a multiline diagnostic");
}

void test_config_and_csv_modules() {
    TemporaryDirectory temporary("neothemis-config-csv-test");
    const fs::path contest = temporary.path() / "contest";
    const fs::path contest_config = contest / neothemis::kContestConfigFilename;
    write_file(contest_config, "# custom settings\n"
                               " core = alternate \n"
                               "contestants_dir = people\n"
                               "tests_dir=cases\n"
                               "output_csv=reports/details.csv\n"
                               "scoreboard_csv=reports/board.csv\n"
                               "keep_workdir=yes\n"
                               "server_ranking_enabled=on\n"
                               "server_contestant_details_enabled=1\n"
                               "compiler=clang++\n"
                               "compile_flags=-O0 -g\n"
                               "stack_limit_mb=32\n"
                               "parallel_jobs=3\n"
                               "forbidden_pattern=first\n"
                               "forbidden_pattern=second\n");

    const auto entries = neothemis::read_config_entries(contest_config);
    require(entries.size() == 14, "config parser lost entries");
    require(entries.front().key == "core" && entries.front().value == "alternate",
            "config parser did not trim key/value text");
    require(entries.front().line_number == 2, "config parser reported the wrong source line");
    const auto values = neothemis::config_values(entries);
    require(values.at("forbidden_pattern") == std::vector<std::string>({"first", "second"}),
            "config parser collapsed a repeated setting");

    const neothemis::ContestConfig loaded = neothemis::load_contest_config(contest_config);
    require(loaded.core_name == "alternate" && loaded.contestants_dir == "people" &&
                loaded.tests_dir == "cases",
            "typed contest settings were not loaded");
    require(loaded.keep_workdir && loaded.server_ranking_enabled &&
                loaded.server_contestant_details_enabled,
            "typed boolean contest settings were not loaded");
    require(loaded.stack_limit_mb == 32 && loaded.parallel_jobs == 3,
            "typed numeric contest settings were not loaded");
    require(loaded.forbidden_patterns == std::vector<std::string>({"first", "second"}),
            "typed contest settings lost repeated forbidden patterns");

    const fs::path rewritten_contest_config = contest / "rewritten.conf";
    neothemis::write_contest_config(rewritten_contest_config, loaded);
    const neothemis::ContestConfig rewritten =
        neothemis::load_contest_config(rewritten_contest_config);
    require(rewritten.core_name == loaded.core_name && rewritten.output_csv == loaded.output_csv &&
                rewritten.scoreboard_csv == loaded.scoreboard_csv &&
                rewritten.forbidden_patterns == loaded.forbidden_patterns &&
                rewritten.server_ranking_enabled == loaded.server_ranking_enabled &&
                rewritten.server_contestant_details_enabled ==
                    loaded.server_contestant_details_enabled,
            "typed contest config writer lost settings");

    neothemis::JudgeOptions options;
    options.execution_security = neothemis::ExecutionSecurity::ExplicitlyUnsafe;
    neothemis::apply_contest_config(loaded, options);
    require(options.core_name == "alternate" && options.output_csv == "reports/details.csv",
            "contest settings were not applied to judge options");
    require(options.execution_security == neothemis::ExecutionSecurity::ExplicitlyUnsafe,
            "persisted contest settings changed the runtime sandbox policy");

    neothemis::JudgeOptions safe_paths;
    safe_paths.contest_root = contest;
    safe_paths.output_csv = "reports/results.csv";
    safe_paths.scoreboard_csv = contest / "reports/scoreboard.csv";
    neothemis::validate_judge_paths(safe_paths);
    safe_paths.tests_dir = "../outside-tests";
    bool escaped_tests_rejected = false;
    try {
        neothemis::validate_judge_paths(safe_paths);
    } catch (const std::exception&) {
        escaped_tests_rejected = true;
    }
    require(escaped_tests_rejected, "contest tests path escaped the contest root");
    safe_paths.tests_dir = "tests";
    safe_paths.output_csv = temporary.path() / "outside-results.csv";
    bool escaped_output_rejected = false;
    try {
        neothemis::validate_judge_paths(safe_paths);
    } catch (const std::exception&) {
        escaped_output_rejected = true;
    }
    require(escaped_output_rejected, "contest output path escaped the contest root");

    auto paths_rejected = [&](const fs::path& output, const fs::path& scoreboard) {
        safe_paths.output_csv = output;
        safe_paths.scoreboard_csv = scoreboard;
        try {
            neothemis::validate_judge_paths(safe_paths);
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    require(paths_rejected("reports/same.csv", "reports/same.csv"),
            "details and scoreboard accepted the same output file");
#ifdef _WIN32
    require(paths_rejected("reports/results.csv", "reports/RESULTS.csv"),
            "Windows case aliases were accepted as distinct output files");
#endif
    require(paths_rejected("tests/A/1/A.out", "reports/scoreboard.csv"),
            "details output was allowed to overwrite a test answer");
    require(paths_rejected("reports/results.csv", "contestants/A/A.cpp"),
            "scoreboard output was allowed to overwrite a submission source");
    require(paths_rejected(neothemis::kContestConfigFilename, "reports/scoreboard.csv"),
            "details output was allowed to overwrite the contest config");
    require(paths_rejected("reports/results.csv.neothemis-lock", "reports/scoreboard.csv"),
            "details output accepted a reserved interprocess lock filename");
#ifndef _WIN32
    safe_paths.output_csv = "reports/results.csv";
    safe_paths.scoreboard_csv = "reports/scoreboard.csv";
    const fs::path escaped_work_target = temporary.path() / "escaped-work";
    fs::create_directories(escaped_work_target);
    fs::create_directory_symlink(escaped_work_target, contest / ".neothemis-work");
    bool escaped_work_rejected = false;
    try {
        neothemis::validate_judge_paths(safe_paths);
    } catch (const std::exception&) {
        escaped_work_rejected = true;
    }
    require(escaped_work_rejected,
            "symlinked judge work directory escaped the contest root");
    fs::remove(contest / ".neothemis-work");
#endif

    const fs::path generated_root = temporary.path() / "generated";
    fs::create_directories(generated_root);
    const auto generated = neothemis::load_or_create_contest_config(generated_root);
    require(generated.forbidden_patterns == neothemis::default_forbidden_patterns(),
            "generated contest config did not contain the default filters");
    require(read_file(generated_root / neothemis::kContestConfigFilename)
                    .rfind("# NeoThemis contest settings\n", 0) == 0,
            "documented contest template changed unexpectedly");

    const fs::path compact_problem = temporary.path() / "compact" / "problem.conf";
    fs::create_directories(compact_problem.parent_path());
    neothemis::write_default_problem_config(compact_problem,
                                            neothemis::ConfigTemplateStyle::Compact);
    require(read_file(compact_problem) == "time_limit_ms=1000\n"
                                          "memory_limit_mb=256\n"
                                          "default_points=1\n"
                                          "checker=token\n",
            "compact problem template changed unexpectedly");

    const fs::path problem_root = temporary.path() / "problem";
    write_file(problem_root / neothemis::kProblemConfigFilename, "time_limit_ms=2500\n"
                                                                 "memory_limit_mb=128\n"
                                                                 "stack_limit_mb=999\n"
                                                                 "default_points=1.5\n"
                                                                 "checker=custom:checker.cpp\n"
                                                                 "test_points.1=1.87\n"
                                                                 "test_points.2=\n");
    const auto problem =
        neothemis::load_problem_config(problem_root / neothemis::kProblemConfigFilename);
    require(problem.time_limit_ms == 2500 && problem.memory_limit_mb == 128 &&
                problem.checker == "custom:checker.cpp",
            "typed problem settings were not loaded");
    require(neothemis::points_for_test(problem, "test001") == 1.87 &&
                neothemis::points_for_test(problem, "test002") == 1.5,
            "problem point aliases/defaults changed");
    require(neothemis::test_point_keys("test001") ==
                std::vector<std::string>({"test001", "001", "1"}),
            "test point aliases changed");

    const fs::path rewritten_problem_config = problem_root / "rewritten.conf";
    neothemis::write_problem_config(rewritten_problem_config, problem);
    const neothemis::ProblemConfig rewritten_problem =
        neothemis::load_problem_config(rewritten_problem_config);
    require(rewritten_problem.time_limit_ms == problem.time_limit_ms &&
                rewritten_problem.memory_limit_mb == problem.memory_limit_mb &&
                rewritten_problem.default_points == problem.default_points &&
                rewritten_problem.checker == problem.checker &&
                rewritten_problem.test_points == problem.test_points,
            "typed problem config writer lost settings");
    require(neothemis::read_config_values(rewritten_problem_config).at("stack_limit_mb") ==
                std::vector<std::string>({"999"}),
            "typed problem config writer discarded a harmless legacy setting");
    const neothemis::ConfigValues rewritten_values =
        neothemis::read_config_values(rewritten_problem_config);
    require(rewritten_values.at("default_points") == std::vector<std::string>({"1.5"}) &&
                rewritten_values.at("test_points.1") ==
                    std::vector<std::string>({"1.87"}),
            "problem point values were expanded into binary floating-point artifacts");

    const fs::path malformed = temporary.path() / "malformed.conf";
    write_file(malformed, "good=value\nnot-an-entry\n");
    bool malformed_rejected = false;
    try {
        (void)neothemis::read_config_entries(malformed);
    } catch (const std::exception& ex) {
        malformed_rejected =
            std::string(ex.what()).find(":2: expected key=value") != std::string::npos;
    }
    require(malformed_rejected, "malformed config did not report its source line");

    const fs::path unknown = temporary.path() / "unknown.conf";
    write_file(unknown, "unknown_key=value\n");
    bool unknown_rejected = false;
    try {
        (void)neothemis::load_contest_config(unknown);
    } catch (const std::exception& ex) {
        unknown_rejected = std::string(ex.what()).find("unknown setting") != std::string::npos;
    }
    require(unknown_rejected, "strict contest config accepted an unknown key");
    const auto forward_compatible =
        neothemis::load_contest_config(unknown, neothemis::UnknownConfigKeyPolicy::Ignore);
    const fs::path preserved_unknown = temporary.path() / "preserved-unknown.conf";
    neothemis::write_contest_config(preserved_unknown, forward_compatible);
    require(neothemis::read_config_values(preserved_unknown).at("unknown_key") ==
                std::vector<std::string>({"value"}),
            "forward-compatible config save discarded an unknown setting");

    const fs::path unknown_problem = temporary.path() / "unknown-problem.conf";
    write_file(unknown_problem, "future_checker_option=value\n");
    const auto forward_problem = neothemis::load_problem_config(
        unknown_problem, neothemis::UnknownConfigKeyPolicy::Ignore);
    const fs::path preserved_unknown_problem = temporary.path() / "preserved-unknown-problem.conf";
    neothemis::write_problem_config(preserved_unknown_problem, forward_problem);
    require(neothemis::read_config_values(preserved_unknown_problem)
                .at("future_checker_option") == std::vector<std::string>({"value"}),
            "forward-compatible problem save discarded an unknown setting");

    const std::string csv_text =
        "plain,\"comma,value\",\"line one\nline two\",\"quote\"\"value\",\r\n"
        "second,row\r\n";
    const neothemis::CsvTable table = neothemis::parse_csv(csv_text);
    require(table.size() == 2 && table[0].size() == 5,
            "CSV parser changed row or trailing-field handling");
    require(table[0][1] == "comma,value" && table[0][2] == "line one\nline two" &&
                table[0][3] == "quote\"value" && table[0][4].empty(),
            "CSV parser changed quoting behavior");
    require(table[1] == neothemis::CsvRow({"second", "row"}), "CSV parser changed CRLF handling");

    std::ostringstream encoded;
    neothemis::write_csv_row(encoded, {"plain", "comma,value", "quote\"value", "line\nvalue"});
    require(encoded.str() == "plain,\"comma,value\",\"quote\"\"value\",\"line\nvalue\"\n",
            "CSV writer changed escaping behavior");
    const fs::path csv_path = temporary.path() / "rows.csv";
    write_file(csv_path, encoded.str());
    require(neothemis::read_csv_file(csv_path) == neothemis::parse_csv(encoded.str()),
            "CSV file reader differs from the string parser");

    const fs::path shared_csv = temporary.path() / "shared-results.csv";
    const neothemis::CsvRow shared_header = {"contestant", "problem", "value"};
    write_file(shared_csv,
               "contestant,problem,legacy-value\nLegacy,P,kept\n");
    const fs::path derived_csv = temporary.path() / "shared-derived.txt";
    auto replace_key = [&](const std::string& contestant, const std::string& value) {
        for (int iteration = 0; iteration < 20; ++iteration) {
            neothemis::replace_csv_rows_by_key_atomic(
                shared_csv, shared_header, {{contestant, "P", value}},
                {{contestant, "P"}},
                [&](const neothemis::CsvTable& merged) {
                    neothemis::write_text_file_atomic(
                        derived_csv, std::to_string(merged.size()));
                });
        }
    };
    auto first_writer = std::async(std::launch::async, replace_key, "Alice", "1");
    auto second_writer = std::async(std::launch::async, replace_key, "Bob", "2");
    first_writer.get();
    second_writer.get();
    const neothemis::CsvTable shared_rows = neothemis::read_csv_file_locked(shared_csv);
    require(shared_rows.size() == 4 &&
                std::count_if(shared_rows.begin(), shared_rows.end(), [](const auto& row) {
                    return row.size() >= 2 &&
                           (row[0] == "Legacy" || row[0] == "Alice" || row[0] == "Bob");
                }) == 3,
            "locked atomic CSV updates lost an unrelated concurrent result pair");
    require(read_file(derived_csv) == std::to_string(shared_rows.size()),
            "derived output was not committed from the final lock-held CSV state");

    std::ostringstream derived_scoreboard;
    neothemis::write_scoreboard_csv_from_results(
        derived_scoreboard,
        {{"contestant", "problem", "test", "verdict", "time_ms", "exit_code",
          "max_points", "earned_points", "message"},
         {"Alice", "A", "1", "AC", "3", "0", "2", "2", ""},
         {"Bob", "A", "1", "WA", "4", "0", "2", "0", ""}});
    require(derived_scoreboard.str().find("Alice,2,2\n") != std::string::npos &&
                derived_scoreboard.str().find("Bob,0,0\n") != std::string::npos,
            "scoreboard was not derived from detailed CSV rows");
}

void test_spreadsheet_xlsx() {
    TemporaryDirectory temporary("neothemis-spreadsheet-test");
    const fs::path workbook = temporary.path() / "rich.xlsx";
    const fs::path extracted = temporary.path() / "rich";

    std::string special_text = "A&<>\"'";
    special_text.push_back('\x01');

    neothemis::XlsxSheet sheet;
    sheet.name = "Scores & \"Q\"";
    sheet.rows = {{neothemis::xlsx_text(special_text), neothemis::xlsx_text("Value")},
                  {neothemis::xlsx_text("Alice"), neothemis::xlsx_number(12.5)}};
    sheet.column_widths = {28.0, 14.5};
    neothemis::write_xlsx_file(workbook, sheet);
    neothemis::extract_zip_archive(workbook, extracted);

    const std::string workbook_xml = read_file(extracted / "xl" / "workbook.xml");
    require(workbook_xml.find("name=\"Scores &amp; &quot;Q&quot;\"") != std::string::npos,
            "XLSX writer did not escape the sheet name");

    const std::string worksheet_xml = read_file(extracted / "xl" / "worksheets" / "sheet1.xml");
    require(worksheet_xml.find("<pane ySplit=\"1\"") != std::string::npos,
            "XLSX writer did not freeze the header row");
    require(worksheet_xml.find("<col min=\"1\" max=\"1\" width=\"28\"") != std::string::npos &&
                worksheet_xml.find("<col min=\"2\" max=\"2\" width=\"14.5\"") != std::string::npos,
            "XLSX writer did not preserve column widths");
    require(worksheet_xml.find("<c r=\"A1\" t=\"inlineStr\" s=\"1\">") != std::string::npos,
            "XLSX writer did not style the header row");
    require(worksheet_xml.find("A&amp;&lt;&gt;&quot;&apos; ") != std::string::npos,
            "XLSX writer did not escape text or sanitize control characters");
    require(worksheet_xml.find("<c r=\"B2\"><v>12.5</v></c>") != std::string::npos,
            "XLSX writer emitted a numeric cell as text");
    require(fs::is_regular_file(extracted / "xl" / "styles.xml"),
            "XLSX writer omitted the header style part");

    neothemis::XlsxSheet plain_sheet;
    plain_sheet.name = "Data";
    plain_sheet.rows = {{neothemis::xlsx_text("header")}, {neothemis::xlsx_text("001")}};
    plain_sheet.freeze_first_row = false;
    plain_sheet.bold_first_row = false;
    const fs::path plain_workbook = temporary.path() / "plain.xlsx";
    const fs::path plain_extracted = temporary.path() / "plain";
    neothemis::write_xlsx_file(plain_workbook, plain_sheet);
    neothemis::extract_zip_archive(plain_workbook, plain_extracted);
    const std::string plain_xml = read_file(plain_extracted / "xl" / "worksheets" / "sheet1.xml");
    require(plain_xml.find("<sheetViews>") == std::string::npos &&
                plain_xml.find(" s=\"1\"") == std::string::npos,
            "plain XLSX mode changed CLI workbook presentation");
    require(!fs::exists(plain_extracted / "xl" / "styles.xml"),
            "plain XLSX mode unexpectedly added a style part");
    require(plain_xml.find(">001</t>") != std::string::npos,
            "plain XLSX mode converted CSV text into a number");

    require(neothemis::ensure_xlsx_extension("report") == fs::path("report.xlsx") &&
                neothemis::ensure_xlsx_extension("report.XLSX") == fs::path("report.XLSX"),
            "XLSX extension normalization changed");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--cpu-time-only") {
            test_cpu_time_accounting();
            std::cout << "CPU-time regression passed\n";
            return 0;
        }
#ifdef _WIN32
        if (argc == 2 && std::string(argv[1]) == "--custom-compiler-runtime-only") {
            test_windows_custom_compiler_runtime_path();
            std::cout << "Windows custom compiler runtime regression passed\n";
            return 0;
        }
#else
        (void)argc;
        (void)argv;
#endif
        test_judge_and_sandbox();
        test_workdir_ownership_and_concurrency();
        test_artifact_namespace_collisions();
        test_custom_checker_partial_points();
        test_symlinked_contest_entries_are_not_followed();
        test_unicode_process_paths();
        test_cpu_time_accounting();
        test_windows_custom_compiler_runtime_path();
        test_windows_descendant_cleanup();
        test_windows_descendant_cpu_accounting();
        test_archive_and_csv();
        test_config_and_csv_modules();
        test_spreadsheet_xlsx();
        std::cout << "core regression tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "core regression test failed: " << ex.what() << '\n';
        return 1;
    }
}
