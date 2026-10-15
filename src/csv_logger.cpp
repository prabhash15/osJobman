#include "csv_logger.hpp"

CsvLogger::CsvLogger(const std::string& dir) {
    const std::string prefix = dir + "/";
    events_.open(prefix + "events.csv");
    samples_.open(prefix + "samples.csv");
    metrics_.open(prefix + "metrics.csv");
    events_ << "t,jid,from,to,event\n";
    samples_ << "t,jid,cpu_pct,cpu_time_s,ctx_vol,ctx_nonvol\n";
    metrics_ << "jid,name,arrived,first_run,finished,turnaround,waiting,response,"
             << "burst_est,cpu_time_s,ctx_vol,ctx_nonvol,dispatches\n";
}

void CsvLogger::event(double t, int jid, JobState from, JobState to, const std::string& note) {
    events_ << t << ',' << jid << ',' << state_name(from) << ',' << state_name(to) << ','
            << note << '\n';
}

void CsvLogger::sample(double t, int jid, const Metrics& m) {
    samples_ << t << ',' << jid << ',' << m.cpu_pct << ',' << m.cpu_time_s << ',' << m.ctx_vol
             << ',' << m.ctx_nonvol << '\n';
}

void CsvLogger::job_done(const Job& j) {
    const double turnaround = j.finish_t - j.arrival_t;
    const double waiting = turnaround - j.m.cpu_time_s;
    const double response = j.first_run_t - j.arrival_t;
    metrics_ << j.id << ',' << j.name << ',' << j.arrival_t << ',' << j.first_run_t << ','
             << j.finish_t << ',' << turnaround << ',' << waiting << ',' << response << ','
             << j.burst_estimate << ',' << j.m.cpu_time_s << ',' << j.m.ctx_vol << ','
             << j.m.ctx_nonvol << ',' << j.m.dispatch_count << '\n';
}
