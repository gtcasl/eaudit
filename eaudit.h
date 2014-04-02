#ifndef EAUDIT_H
#define EAUDIT_H

#include <signal.h>

struct stats_t{
  long long package_energy;
  long long pp0_energy;
  long long time;
  stats_t(long long pae, long long ppe, long long t) : package_energy{pae}, pp0_energy{ppe}, time{t} {}
  stats_t() : package_energy{0}, pp0_energy{0}, time{0} {}

  stats_t& operator+=(const stats_t& rhs){
    package_energy += rhs.package_energy;
    pp0_energy += rhs.pp0_energy;
    time += rhs.time;
  }
};

void EAUDIT_shutdown();

void overflow(int signum, siginfo_t* info, void* context);
int init_papi();
stats_t read_rapl();

#endif // EAUDIT_H
