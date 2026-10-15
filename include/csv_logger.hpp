// Append-only CSV logging. Three files, one jobman run per directory:
//   events.csv  - one line per state transition (the scheduling history)
//   samples.csv - one line per tick for the RUNNING job (live CPU% evidence)
//   metrics.csv - one line per finished job (the summary used in the report)
#pragma once

#include "types.hpp"

#include <fstream>
#include <string>

class CsvLogger {
public:
    // Creates logs/run-<timestamp>/ inside the project root and opens the files.
    explicit CsvLogger(const std::string& dir);

    void event(double t, int jid, JobState from, JobState to, const std::string& note);
    void sample(double t, int jid, const Metrics& m);
    void job_done(const Job& j);

private:
    std::ofstream events_, samples_, metrics_;
};
