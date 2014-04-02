#include "eaudit.h"

#include <sstream>
#include <cassert>
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
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ucontext.h>

#include "papi.h"
#include <byfl-common.h>

#ifdef DEBUG
#define print(...) printf(__VA_ARGS__)
#else
#define print(...)
#endif

using namespace std;

string parse_backtrace_entry(string entry);

namespace{
const unsigned kSleepSecs = 0;
const unsigned kSleepUsecs = 2000;
const double kNanoToBase = 1e-9;
const long long kBaseToNano = 1000000000;
const long long kMicroToNano = 1000;
const long long kCounterMax = numeric_limits<unsigned int>::max();
const int kMaxTrace = 3;
const int kTopOfStackID = 2;
const char* kBacktraceNameFile = "eaudit.btrace";
const int kBufSize = 1024;
}

static long long last_energy_value[2] = {0LL, 0LL};

map<void*, stats_t>* total_stats;

int* get_eventset(){
  static int eventset = PAPI_NULL;
  return &eventset;
}

static int inited = init_papi();
static int myfd[2];
static volatile int cnt = 0;

int init_papi(){
  assert(kSleepSecs > 0 || kSleepUsecs > 1000 && "ERROR: must sleep for more than 1ms");
  int* eventset = get_eventset();
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

  total_stats = new map<void*, stats_t>;

  if(pipe(myfd) == -1){
    cerr << "Unable to open file" << endl;
    exit(-1);
  }

  struct itimerval work_time;
  work_time.it_value.tv_sec = kSleepSecs;
  work_time.it_value.tv_usec = kSleepUsecs;
  work_time.it_interval.tv_sec = kSleepSecs;
  work_time.it_interval.tv_usec = kSleepUsecs;
  setitimer(ITIMER_REAL, &work_time, nullptr);

  retval = PAPI_reset(*eventset);
  retval=PAPI_read(*eventset, last_energy_value);
  if(retval != PAPI_OK){
    PAPI_perror(NULL);
    exit(-1);
  }
  return 1;
}

stats_t read_rapl()
{
  stats_t res;
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
  //print("read: %lld\t%lld\n", total_energy[0], total_energy[1]);
  res.package_energy = total_energy[0];
  res.pp0_energy = total_energy[1];
  res.time = kSleepSecs * kBaseToNano + kSleepUsecs * kMicroToNano;

  last_energy_value[0] = energy_val[0];
  last_energy_value[1] = energy_val[1];
  return res;
}

void EAUDIT_push() {}

void EAUDIT_pop(const char* n) {};

void EAUDIT_shutdown(){
  print("shutdown\n");
  struct itimerval work_time;
  work_time.it_value.tv_sec = 0;
  work_time.it_value.tv_usec = 0;
  work_time.it_interval.tv_sec = 0;
  work_time.it_interval.tv_usec = 0;
  setitimer(ITIMER_REAL, &work_time, nullptr);

  map<string, stats_t> stat_map;
  for(auto& func : (*total_stats)){
    auto entry = backtrace_symbols(&func.first, 1)[0];
    auto mangled = parse_backtrace_entry(entry);
    auto name = demangle_func_name(mangled);
    stat_map[name] += func.second;
  }

  vector<pair<string, stats_t> > stats;
  for(auto& func : stat_map){
    stats.emplace_back(func.first, func.second);
    print("%s\tpackage:%lld\tpp0:%lld\ttime:%lld\n", func.first.c_str(),
        func.second.package_energy,
        func.second.pp0_energy,
        func.second.time);
  }

  delete total_stats;

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
}

void overflow(int signum, siginfo_t* info, void* context){
  if(signum == SIGALRM){
    //print("overflow\n");

    ucontext_t* uc = (ucontext_t*) context;
    void* caller = (void*) uc->uc_mcontext.gregs[REG_RIP];
    auto res = read_rapl();
    (*total_stats)[caller] += res;
  }
}

string parse_backtrace_entry(string entry){
  char* name_start = nullptr;
  string::size_type name_len = 0;
  for(char* p = (char*) entry.c_str(); *p; ++p){
    if(*p == '('){
      name_start = p + 1;
    }
    if(*p == '+' && name_start){
      name_len = p - name_start;
      break;
    }
  }
  return string{name_start, name_len};
}

