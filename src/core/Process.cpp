#include "Process.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace cr {

ProcessResult runProcess(const std::vector<std::string>& args,
                         const std::string& cwd,
                         const std::vector<std::string>& extraEnv)
{
    ProcessResult result;
    if (args.empty())
        return result;

    int outPipe[2];
    int errPipe[2];
    if (pipe(outPipe) != 0)
        return result;
    if (pipe(errPipe) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return result;
    }

    // Build argv/envp before fork: only async-signal-safe calls are allowed in the child.
    std::vector<char*> argv;
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    std::vector<std::string> envStorage;
    std::vector<char*> envp;
    for (char** e = environ; e && *e; ++e)
        envp.push_back(*e);
    envStorage = extraEnv;
    for (auto& e : envStorage)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        return result;
    }

    if (pid == 0) {
        int devNull = open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            close(devNull);
        }
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]); close(outPipe[1]);
        close(errPipe[0]); close(errPipe[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0)
            _exit(127);
        execvpe(argv[0], argv.data(), envp.data());
        _exit(127);
    }

    close(outPipe[1]);
    close(errPipe[1]);

    pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
    std::string* sinks[2] = {&result.out, &result.err};
    int open = 2;
    char buf[65536];
    while (open > 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[i]->append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open;
            }
        }
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

} // namespace cr
