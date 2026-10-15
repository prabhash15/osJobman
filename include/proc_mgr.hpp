// Real process management: fork/exec/waitpid + SIGSTOP/SIGCONT/SIGKILL.
// A child is born STOPPED (the 1-CPU model: nothing runs until dispatched).
#pragma once

#include <string>
#include <sys/types.h>
#include <vector>

class ProcMgr {
public:
    // fork() + raise(SIGSTOP) + execvp(). Waits (WUNTRACED) until the child is
    // actually stopped before returning, so it can never run uninvited.
    // Returns -1 on fork failure.
    pid_t spawn(const std::vector<std::string>& argv);

    bool cont(pid_t pid); // SIGCONT
    bool stop(pid_t pid); // SIGSTOP
    bool kill(pid_t pid); // SIGKILL
};
