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
#include <errno.h>
#include <cstdio>

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
const int kOverflowThreshold = 10000000;
const long long kOneMS = 1000000;
const long long kCounterMax = numeric_limits<unsigned int>::max();
}

static long long last_read_time = 0LL;
static long long last_energy_value[2] = {0LL, 0LL};

stack<long long, vector<long long> >& cur_package_energy(){
  static stack<long long, vector<long long> > cur_package_energy_;
  return cur_package_energy_;
}

stack<long long, vector<long long> >& cur_pp0_energy(){
  static stack<long long, vector<long long> > cur_pp0_energy_;
  return cur_pp0_energy_;
}

stack<long long, vector<long long> >& cur_time(){
  static stack<long long, vector<long long> > cur_time_;
  return cur_time_;
}

struct stats_t{
  long long package_energy;
  long long pp0_energy;
  long long time;
  stats_t(long long pae, long long ppe, long long t) : package_energy{pae}, pp0_energy{ppe}, time{t} {}
  stats_t() : package_energy{0}, pp0_energy{0}, time{0} {}
};

#ifdef EAUDIT_RECORD_ALL
map<string, vector<stats_t> >& total_stats(){
  static map<string, vector<stats_t> > total_stats_;
  return total_stats_;
}
#else
map<string, stats_t>& total_stats(){
  static map<string, stats_t> total_stats_;
  return total_stats_;
}
#endif

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
  if(signal(SIGALRM, overflow) == SIG_ERR){
    fprintf(stderr, "Unable to set up signal handler\n");
    exit(-1);
  }

  // Set a dummy first element so that the first do_push has some place to put 
  // the previous function energy.
  cur_package_energy().push(0LL);
  cur_pp0_energy().push(0LL);
  cur_time().push(0LL);

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
    cur_package_energy().top() += total_energy[0];
    cur_pp0_energy().top() += total_energy[1];
    cur_time().top() += curtime - last_read_time;
    last_read_time = curtime;
    last_energy_value[0] = energy_val[0];
    last_energy_value[1] = energy_val[1];
  }
}

void EAUDIT_push(){
  print("push\n");
  read_rapl();
  cur_package_energy().push(0LL);
  cur_pp0_energy().push(0LL);
  cur_time().push(0LL);
}

void EAUDIT_pop(const char* func_name){
  print("popping %s\n", func_name);
  read_rapl();
#ifdef EAUDIT_RECORD_ALL
  total_stats()[func_name].emplace_back(cur_package_energy().top(), cur_pp0_energy().top(), cur_time().top());
#else
  total_stats()[func_name].package_energy += cur_package_energy().top();
  total_stats()[func_name].pp0_energy += cur_pp0_energy().top();
  total_stats()[func_name].time += cur_time().top();
#endif
  cur_package_energy().pop();
  cur_pp0_energy().pop();
  cur_time().pop();
}

void EAUDIT_shutdown(){
  print("shutdown\n");

#ifdef EAUDIT_RECORD_ALL
  vector<pair<string, vector<stats_t> > > stats;
#else
  vector<pair<string, stats_t> > stats;
#endif
  for(auto& func : total_stats()){
    auto name = demangle_func_name(func.first);
    stats.emplace_back(name, func.second);
  }

  stable_sort(stats.begin(), stats.end(),
#ifdef EAUDIT_RECORD_ALL
      [](const pair<string, vector<stats_t> >& a,
         const pair<string, vector<stats_t> >& b){
        stats_t sum_a = accumulate(a.second.begin(), a.second.end(), stats_t{0,0,0},
          [](const stats_t& x, const stats_t& y){ return stats_t{x.package_energy + y.package_energy, 
                                                                 x.pp0_energy + y.pp0_energy,
                                                                 x.time + y.time}; });
        stats_t sum_b = accumulate(b.second.begin(), b.second.end(), stats_t{0,0,0},
          [](const stats_t& x, const stats_t& y){ return stats_t{x.package_energy + y.package_energy, 
                                                                 x.pp0_energy + y.pp0_energy,
                                                                 x.time + y.time}; });
        return sum_a.time > sum_b.time;
      }
#else
      [](const pair<string, stats_t>& a,
         const pair<string, stats_t>& b){ return a.second.time > b.second.time; }
#endif
      );

  double total_time = 0, total_package_energy = 0, total_pp0_energy = 0;
#ifdef EAUDIT_RECORD_ALL
  for(const auto& func : stats){
    for(const auto& stat : func.second){
      total_time += stat.time;
      total_package_energy += stat.package_energy;
      total_pp0_energy += stat.pp0_energy;
    }
  }
#else
  for(const auto& func : stats){
    total_time += func.second.time;
    total_package_energy += func.second.package_energy;
    total_pp0_energy += func.second.pp0_energy;
  }
#endif

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
#ifdef EAUDIT_RECORD_ALL
         << "\t" << "# Calls" 
         << "\t" << "Time Avg" 
         << "\t" << "Time Stddev"
         << "\t" << "Package Energy Avg"
         << "\t" << "PP0 Energy Avg"
         << "\t" << "Package Energy Stddev" 
         << "\t" << "PP0 Energy Stddev" 
#endif
         << endl;
  for(auto& func : stats){
#ifdef EAUDIT_RECORD_ALL
    stats_t sum = accumulate(func.second.begin(), func.second.end(), stats_t{0,0,0},
        [](stats_t& x, stats_t& y){ 
          return stats_t{x.package_energy + y.package_energy, 
                         x.pp0_energy + y.pp0_energy,
                         x.time + y.time}; 
        });
    double package_energy_avg = sum.package_energy / (double) func.second.size();
    double pp0_energy_avg = sum.pp0_energy / (double) func.second.size();
    double time_avg = sum.time / (double) func.second.size();
    double package_energy_dev = 0, pp0_energy_dev = 0, time_dev = 0;
    for(auto& val : func.second){
      package_energy_dev += pow(val.package_energy - package_energy_avg, 2);
      pp0_energy_dev += pow(val.pp0_energy - pp0_energy_avg, 2);
      time_dev += pow(val.time - time_avg, 2);
    }
    package_energy_dev = sqrt(package_energy_dev / func.second.size());
    pp0_energy_dev = sqrt(pp0_energy_dev / func.second.size());
    time_dev = sqrt(time_dev / func.second.size());
#else
    stats_t sum{func.second.package_energy, func.second.pp0_energy, func.second.time};
#endif
    if(sum.time == 0) continue;
    myfile << func.first
           << "\t" << sum.time * kNanoToBase
           << "\t" << sum.package_energy * kNanoToBase
           << "\t" << sum.pp0_energy * kNanoToBase
           << "\t" << sum.time / total_time * 100.0
           << "\t" << sum.package_energy / total_package_energy * 100.0
           << "\t" << sum.pp0_energy / total_pp0_energy * 100.0
           << "\t" << sum.package_energy/(double)sum.time 
           << "\t" << sum.pp0_energy/(double)sum.time 
#ifdef EAUDIT_RECORD_ALL
           << "\t" << func.second.size()
           << "\t" << time_avg * kNanoToBase 
           << "\t" << time_dev * kNanoToBase
           << "\t" << package_energy_avg * kNanoToBase 
           << "\t" << pp0_energy_avg * kNanoToBase 
           << "\t" << package_energy_dev * kNanoToBase
           << "\t" << pp0_energy_dev * kNanoToBase
#endif
           << endl;
  }
  myfile.close();
}

/*void overflow(int eventset, void* address, long long overflow_vector, 
              void* context){
              */
void overflow(int signum){
  if(signum == SIGALRM){
    print("overflow\n");
    read_rapl();
  }
}

