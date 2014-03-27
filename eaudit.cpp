#include "eaudit.h"

#include <iomanip>
#include <algorithm>
#include <map>
#include <stack>
#include <limits>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <sys/time.h>
#include <signal.h>
#include <execinfo.h>
#include <errno.h>
#include <cstdio>

#include <ucontext.h>

#include "papi.h"
#include <byfl-common.h>

#ifdef DEBUG
#define print(...) printf(__VA_ARGS__)
#else
#define print(...)
#endif

using namespace std;

namespace{
const unsigned kSleepSecs = 2;
const unsigned kSleepUsecs = 0;
const double kNanoToBase = 1e-9;
const long long kOneMS = 1000000;
const long long kCounterMax = numeric_limits<unsigned int>::max();
const int kMaxTrace = 30;
}

static long long last_read_time = 0LL;
static long long last_energy_value[2] = {0LL, 0LL};

struct stats_t{
  long long package_energy;
  long long pp0_energy;
  long long time;
  stats_t(long long pae, long long ppe, long long t) : package_energy{pae}, pp0_energy{ppe}, time{t} {}
  stats_t() : package_energy{0}, pp0_energy{0}, time{0} {}
};

int* get_eventset(){
  static int eventset = PAPI_NULL;
  if(eventset == PAPI_NULL){
    init_papi(&eventset);
  }
  return &eventset;
}

void init_papi(int* eventset){
  print("init\n");
  int retval;
  if ( ( retval = PAPI_library_init( PAPI_VER_CURRENT ) ) != PAPI_VER_CURRENT ){
    fprintf(stderr, "Unable to init PAPI library.\n");
    switch(retval){
    case PAPI_EINVAL:
      fprintf(stderr, "einval\n");
      break;
    case PAPI_ENOMEM:
      fprintf(stderr, "enomem\n");
      break;
    case PAPI_ESBSTR:
      fprintf(stderr, "esbstr\n");
      break;
    case PAPI_ESYS:
      fprintf(stderr, "esys\n");
      break;
    }
    exit(-1);
  }
  retval = PAPI_create_eventset( eventset ); 
  if ( retval != PAPI_OK  ){
    PAPI_perror(NULL);
    exit(-1);
  }

  retval = PAPI_add_named_event(*eventset, (char*) "rapl:::PACKAGE_ENERGY:PACKAGE0");
  if(retval != PAPI_OK){
    PAPI_perror(NULL);
    exit(-1);
  }

  retval = PAPI_add_named_event(*eventset, (char*) "rapl:::PP0_ENERGY:PACKAGE0");
  if(retval != PAPI_OK){
    PAPI_perror(NULL);
    exit(-1);
  }

  retval=PAPI_start(*eventset);
  if(retval != PAPI_OK){
    PAPI_perror(NULL);
    exit(-1);
  }

  // set up signal handler
  struct sigaction sa;
  sa.sa_sigaction = overflow;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  if(sigaction(SIGALRM, &sa, nullptr) != 0){
    fprintf(stderr, "Unable to set up signal handler\n");
    exit(-1);
  }

  struct itimerval work_time;
  work_time.it_value.tv_sec = kSleepSecs;
  work_time.it_value.tv_usec = kSleepUsecs;
  work_time.it_interval.tv_sec = kSleepSecs;
  work_time.it_interval.tv_usec = kSleepUsecs;
  setitimer(ITIMER_REAL, &work_time, nullptr);

  retval = PAPI_reset(*eventset);
  PAPI_read(*eventset, last_energy_value);
  last_read_time = PAPI_get_real_nsec();
}

void read_rapl(){
  long long curtime = PAPI_get_real_nsec();
  if(curtime - last_read_time > kOneMS){
    long long energy_val[2];
    int retval=PAPI_read(*get_eventset(), energy_val);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
    long long total_energy[2];
    for(int i = 0; i < 2; ++i){
      if(energy_val[i] < last_energy_value[i]){
        total_energy[i] = kCounterMax - last_energy_value[i] + energy_val[i];
      } else {
        total_energy[i] = energy_val[i] - last_energy_value[i];
      }
    }
    print("read: %lld\t%lld\n", total_energy[0], total_energy[1]);
    last_read_time = curtime;
    last_energy_value[0] = energy_val[0];
    last_energy_value[1] = energy_val[1];
    //TODO: attribute to top of call stack
  }
}

void EAUDIT_push() { int* tmp = get_eventset();};
void EAUDIT_pop(const char* n) {};

void EAUDIT_shutdown(){
  print("shutdown\n");
/*
  vector<pair<string, stats_t> > stats;
  for(auto& func : total_stats()){
    auto name = demangle_func_name(func.first); // TODO: figure out demangling
                                                // ie byfl or builtin
    stats.emplace_back(name, func.second);
  }

  stable_sort(stats.begin(), stats.end(),
      [](const pair<string, stats_t>& a,
        const pair<string, stats_t>& b){ return a.second.time > b.second.time; }
      );

  double total_time = 0, total_package_energy = 0, total_pp0_energy = 0;
  for(const auto& func : stats){
    total_time += func.second.time;
    total_package_energy += func.second.package_energy;
    total_pp0_energy += func.second.pp0_energy;
  }

  ofstream myfile;
  myfile.open("eaudit.tsv");
  myfile << "Func Name" 
         << "\t" << "Time(s)"
         << "\t" << "Package Energy(J)" 
         << "\t" << "PP0 Energy(J)" 
         << "\t" << "% Time"
         << "\t" << "% Package Energy"
         << "\t" << "% PP0 Energy"
         << "\t" << "Package Power(w)" 
         << "\t" << "PP0 Power(w)" 
         << endl;
  for(auto& func : stats){
    if(func.second.time == 0) continue;
    myfile << func.first
           << "\t" << func.second.time * kNanoToBase
           << "\t" << func.second.package_energy * kNanoToBase
           << "\t" << func.second.pp0_energy * kNanoToBase
           << "\t" << func.second.time / total_time * 100.0
           << "\t" << func.second.package_energy / total_package_energy * 100.0
           << "\t" << func.second.pp0_energy / total_pp0_energy * 100.0
           << "\t" << func.second.package_energy/(double)func.second.time 
           << "\t" << func.second.pp0_energy/(double)func.second.time 
           << endl;
  }
  myfile.close();
  */
}

void overflow(int signum, siginfo_t* info, void* context){
  if(signum == SIGALRM){
    print("overflow\n");
    ucontext_t* uc = (ucontext_t*) context;
    void* trace[kMaxTrace];
    int trace_size = backtrace(trace, kMaxTrace);
    //trace[1] = (void*) uc->uc_mcontext.gregs[REG_RIP];
    char** names = backtrace_symbols(trace, trace_size);
    cout << "last " << trace_size << " frames:" << endl;
    for(int i = 0; i < trace_size; ++i){
      cout << names[i] << endl;
    }
  }
}

