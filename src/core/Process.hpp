#pragma once

#include <string>
#include <vector>

namespace cr {

struct ProcessResult {
    int exitCode = -1;
    std::string out;
    std::string err;

    bool ok() const { return exitCode == 0; }
};

// Runs a program (argv[0] is looked up in PATH) and captures stdout/stderr.
ProcessResult runProcess(const std::vector<std::string>& args,
                         const std::string& cwd = {},
                         const std::vector<std::string>& extraEnv = {});

} // namespace cr
