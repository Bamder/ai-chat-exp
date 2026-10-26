#include "aichat/core/sandbox.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace sandbox {
namespace {

// Where the bubblewrap backend exposes the workspace inside the sandbox.
// Scripts must not depend on this literal path: the runner also exports it as
// AI_CHAT_WORKSPACE and runs with it as the working directory.
constexpr const char* kWorkdir = "/mnt/work";

long long nowMs() {
    timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string findExecutable(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        return ::access(name.c_str(), X_OK) == 0 ? name : std::string();
    }

    const char* env = std::getenv("PATH");
    const std::string path = env && *env ? env : "/usr/local/bin:/usr/bin:/bin";
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find(':', i);
        if (j == std::string::npos) j = path.size();
        std::string dir = path.substr(i, j - i);
        if (dir.empty()) dir = ".";
        const std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        if (j == path.size()) break;
        i = j + 1;
    }
    return "";
}

void addTmpfsIfPresent(std::vector<std::string>& args, const char* dir) {
    if (isDirectory(dir)) {
        args.push_back("--tmpfs");
        args.push_back(dir);
    }
}

void addEnv(std::vector<std::string>& args, const char* name,
            const char* value) {
    args.push_back("--setenv");
    args.push_back(name);
    args.push_back(value);
}

void applyLimits(const Limits& limits) {
    auto set = [](int resource, rlim_t value) {
        struct rlimit rl;
        rl.rlim_cur = value;
        rl.rlim_max = value;
        ::setrlimit(resource, &rl);
    };

    set(RLIMIT_CORE, 0);
    if (limits.memoryBytes > 0)
        set(RLIMIT_AS, static_cast<rlim_t>(limits.memoryBytes));
    if (limits.maxFileBytes > 0)
        set(RLIMIT_FSIZE, static_cast<rlim_t>(limits.maxFileBytes));
    if (limits.maxOpenFiles > 0)
        set(RLIMIT_NOFILE, static_cast<rlim_t>(limits.maxOpenFiles));
    if (limits.wallMs > 0) {
        const long long seconds = (limits.wallMs + 999) / 1000 + 1;
        set(RLIMIT_CPU, static_cast<rlim_t>(seconds));
    }
}

class BubblewrapBackend : public Backend {
  public:
    const char* name() const override { return "bubblewrap"; }

    bool available(std::string& reason) const override {
        if (findExecutable("bwrap").empty()) {
            reason = "bubblewrap (bwrap) is not installed";
            return false;
        }
        return true;
    }

    Result run(const Request& request) override {
        Result result;

        if (request.command.empty()) {
            result.error = "the sandbox command is empty";
            return result;
        }

        const std::string bwrap = findExecutable("bwrap");
        if (bwrap.empty()) {
            result.error = "bubblewrap (bwrap) is not installed";
            return result;
        }

        char resolved[PATH_MAX];
        if (request.workdir.empty() ||
            !::realpath(request.workdir.c_str(), resolved)) {
            result.error = "the sandbox workdir does not exist: " +
                           request.workdir;
            return result;
        }
        if (!isDirectory(resolved)) {
            result.error = "the sandbox workdir is not a directory: " +
                           std::string(resolved);
            return result;
        }
        const std::string workdir = resolved;

        std::vector<std::string> args;
        args.push_back(bwrap);
        args.push_back("--unshare-all");
        args.push_back("--die-with-parent");
        args.push_back("--cap-drop");
        args.push_back("ALL");
        // A read-only host root gives the tool the system libraries it needs.
        args.push_back("--ro-bind");
        args.push_back("/");
        args.push_back("/");
        args.push_back("--proc");
        args.push_back("/proc");
        args.push_back("--dev");
        args.push_back("/dev");
        args.push_back("--tmpfs");
        args.push_back("/tmp");

        // Hide the user's data and runtime sockets while keeping the system
        // read-only view. The workdir is bound separately below.
        addTmpfsIfPresent(args, "/home");
        addTmpfsIfPresent(args, "/root");
        addTmpfsIfPresent(args, "/run");
        addTmpfsIfPresent(args, "/media");
        addTmpfsIfPresent(args, "/mnt");
        addTmpfsIfPresent(args, "/srv");
        addTmpfsIfPresent(args, "/var/tmp");

        // /mnt is a fresh tmpfs, so the workdir can be created there even
        // though the rest of the root is read-only.
        args.push_back("--bind");
        args.push_back(workdir);
        args.push_back(kWorkdir);
        args.push_back("--chdir");
        args.push_back(kWorkdir);

        args.push_back("--clearenv");
        addEnv(args, "PATH",
               "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
        addEnv(args, "HOME", kWorkdir);
        addEnv(args, "PWD", kWorkdir);
        addEnv(args, "AI_CHAT_WORKSPACE", kWorkdir);
        addEnv(args, "TMPDIR", "/tmp");
        addEnv(args, "LANG", "C.UTF-8");
        addEnv(args, "LC_ALL", "C.UTF-8");

        args.push_back("--");
        for (const std::string& part : request.command) args.push_back(part);

        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0) {
            result.error = std::string("cannot create the output pipe: ") +
                           std::strerror(errno);
            return result;
        }

        const pid_t pid = ::fork();
        if (pid < 0) {
            result.error =
                std::string("cannot start the sandbox: ") + std::strerror(errno);
            ::close(fds[0]);
            ::close(fds[1]);
            return result;
        }

        if (pid == 0) { // child
            ::setpgid(0, 0);
            ::dup2(fds[1], STDOUT_FILENO);
            ::dup2(fds[1], STDERR_FILENO);
            ::close(fds[0]);
            ::close(fds[1]);

            const int devNull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (devNull >= 0) {
                ::dup2(devNull, STDIN_FILENO);
                if (devNull != STDIN_FILENO) ::close(devNull);
            }

            applyLimits(request.limits);

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const std::string& arg : args)
                argv.push_back(const_cast<char*>(arg.c_str()));
            argv.push_back(nullptr);
            ::execv(bwrap.c_str(), argv.data());
            ::_exit(127);
        }

        // parent
        result.started = true;
        ::close(fds[1]);

        const int flags = ::fcntl(fds[0], F_GETFL, 0);
        if (flags >= 0) ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

        const long long startedAt = nowMs();
        const long long deadline =
            request.limits.wallMs > 0 ? startedAt + request.limits.wallMs : 0;
        bool eof = false;

        for (;;) {
            long long waitMs = 1000;
            if (deadline != 0) {
                const long long left = deadline - nowMs();
                if (left <= 0) {
                    result.timedOut = true;
                    ::kill(-pid, SIGKILL);
                    ::kill(pid, SIGKILL);
                    break;
                }
                waitMs = std::min<long long>(left, 1000);
            }

            if (eof) break;

            pollfd pfd;
            pfd.fd = fds[0];
            pfd.events = POLLIN;
            pfd.revents = 0;
            const int rc = ::poll(&pfd, 1, static_cast<int>(waitMs));
            if (rc < 0) {
                if (errno == EINTR) continue;
                ::kill(-pid, SIGKILL);
                ::kill(pid, SIGKILL);
                result.error = std::string("cannot read sandbox output: ") +
                               std::strerror(errno);
                break;
            }
            if (rc == 0) continue;

            char buffer[4096];
            const ssize_t n = ::read(fds[0], buffer, sizeof buffer);
            if (n > 0) {
                const size_t room = result.output.size() <
                                            request.limits.maxOutputBytes
                                        ? request.limits.maxOutputBytes -
                                              result.output.size()
                                        : 0;
                const size_t take =
                    std::min(room, static_cast<size_t>(n));
                result.output.append(buffer, take);
                if (take < static_cast<size_t>(n)) result.truncated = true;
            } else if (n == 0) {
                eof = true;
            } else if (errno != EAGAIN && errno != EINTR) {
                eof = true;
            }
        }

        ::close(fds[0]);

        int status = 0;
        pid_t waited = 0;
        do {
            waited = ::waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0) {
            if (result.error.empty())
                result.error =
                    std::string("cannot wait for the sandbox: ") +
                    std::strerror(errno);
            result.exitCode = -1;
            return result;
        }

        if (result.timedOut) {
            result.error = "the sandbox timed out";
            result.exitCode = 124;
            return result;
        }
        if (WIFEXITED(status)) {
            result.exitCode = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exitCode = 128 + WTERMSIG(status);
            result.error = "the sandboxed command was terminated by signal " +
                           std::to_string(WTERMSIG(status));
        }
        result.ok = result.error.empty() && result.exitCode == 0;
        return result;
    }
};

} // namespace

Backend& platform() {
    static BubblewrapBackend backend;
    return backend;
}

} // namespace sandbox

#elif defined(_WIN32)

namespace sandbox {
namespace {

class WindowsBackend : public Backend {
  public:
    const char* name() const override { return "windows-appcontainer"; }

    bool available(std::string& reason) const override {
        reason = "the Windows sandbox backend is not implemented yet; use an "
                 "AppContainer with Job Object limits";
        return false;
    }

    Result run(const Request&) override {
        Result result;
        result.error = "the Windows sandbox backend is not implemented yet";
        return result;
    }
};

} // namespace

Backend& platform() {
    static WindowsBackend backend;
    return backend;
}

} // namespace sandbox

#else

namespace sandbox {
namespace {

class UnsupportedBackend : public Backend {
  public:
    const char* name() const override { return "unsupported"; }

    bool available(std::string& reason) const override {
        reason = "no sandbox backend exists for this platform";
        return false;
    }

    Result run(const Request&) override {
        Result result;
        result.error = "no sandbox backend exists for this platform";
        return result;
    }
};

} // namespace

Backend& platform() {
    static UnsupportedBackend backend;
    return backend;
}

} // namespace sandbox

#endif
