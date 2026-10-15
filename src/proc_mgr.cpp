#include "proc_mgr.hpp"

// Real process management: fork + exec, controlled with signals.
// Children are born STOPPED (raise(SIGSTOP) before exec), so the scheduler
// must SIGCONT them before any work happens — the 1-CPU model made real.
#include <csignal>
#include <sys/wait.h>
#include <vector>

pid_t ProcMgr::spawn(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Child: stop myself right away, then exec. A stop survives exec,
        // so this process cannot run until the scheduler SIGCONTs it.
        raise(SIGSTOP);
        // Build the char* array execvp() needs (const_cast is safe: execvp
        // neither modifies nor stores these strings).
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127); // reached only when exec failed
    }
    // Parent: wait until the child is really stopped before returning,
    // so it can never run or exit without us knowing about it.
    int status = 0;
    waitpid(pid, &status, WUNTRACED);
    return pid;
}

static bool send(pid_t pid, int sig) { return pid > 0 && ::kill(pid, sig) == 0; }

bool ProcMgr::cont(pid_t pid) { return send(pid, SIGCONT); }
bool ProcMgr::stop(pid_t pid) { return send(pid, SIGSTOP); }
bool ProcMgr::kill(pid_t pid) { return send(pid, SIGKILL); }
