#pragma once
#include <string>
#include <vector>

// Runs argv[0] with the given arguments directly via fork+execvp — no shell
// is involved, so filenames with spaces/quotes/`;`/etc. can never be
// interpreted as shell syntax. Captures stdout; stderr is discarded.
struct CommandResult {
    int exitCode = -1;
    std::string stdoutData;
};

// If cwd is non-null, the child process chdir()s there before exec — used
// so `zip` can be run with relative paths against a scratch directory
// without ever building a shell command string.
CommandResult runCommand(const std::vector<std::string>& argv, const std::string* cwd = nullptr);
