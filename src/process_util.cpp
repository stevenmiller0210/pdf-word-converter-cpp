#include "process_util.h"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <vector>

CommandResult runCommand(const std::vector<std::string>& argv, const std::string* cwd) {
    if (argv.empty()) throw std::runtime_error("runCommand: empty argv");

    int outPipe[2];
    if (pipe(outPipe) != 0) throw std::runtime_error("pipe() failed");

    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork() failed");

    if (pid == 0) {
        // Child.
        close(outPipe[0]);
        dup2(outPipe[1], STDOUT_FILENO);
        close(outPipe[1]);
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) { dup2(devNull, STDERR_FILENO); close(devNull); }
        if (cwd && chdir(cwd->c_str()) != 0) _exit(126);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);

        execvp(cargv[0], cargv.data());
        _exit(127); // execvp only returns on failure
    }

    // Parent.
    close(outPipe[1]);
    CommandResult result;
    char buf[4096];
    ssize_t n;
    while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) {
        result.stdoutData.append(buf, static_cast<size_t>(n));
    }
    close(outPipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}
