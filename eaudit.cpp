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
  string name;
  long long package_energy;
  long long pp0_energy;
  long long time;
  stats_t(string n, long long pae, long long ppe, long long t) : name{n}, package_energy{pae}, pp0_energy{ppe}, time{t} {}
  stats_t() : name{""}, package_energy{0}, pp0_energy{0}, time{0} {}
};

vector<stats_t>& total_stats(){
  static vector<stats_t> total_stats_;
  return total_stats_;
}

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
  int found = -1;
  for(auto i = 0; i < total_stats().size(); ++i){
    if(total_stats()[i].name == func_name){
      found = i;
      break;
    }
  }

  stats_t* curstats;
  if(found == -1){
    total_stats().emplace_back(func_name, 0, 0, 0);
    curstats = &total_stats().back();
  } else {
    curstats = &total_stats()[found];
  }

  curstats->package_energy += cur_package_energy().top();
  curstats->pp0_energy += cur_pp0_energy().top();
  curstats->time += cur_time().top();
  cur_package_energy().pop();
  cur_pp0_energy().pop();
  cur_time().pop();
}

void EAUDIT_shutdown(){
  print("shutdown\n");

  auto stats = total_stats();
  stable_sort(stats.begin(), stats.end(),
      [](const stats_t& a,
         const stats_t& b){ return a.time > b.time; }
      );

  double total_time = 0, total_package_energy = 0, total_pp0_energy = 0;
  for(const auto& func : stats){
    total_time += func.time;
    total_package_energy += func.package_energy;
    total_pp0_energy += func.pp0_energy;
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
    if(func.time == 0) continue;
    myfile << func.name
           << "\t" << func.time * kNanoToBase
           << "\t" << func.package_energy * kNanoToBase
           << "\t" << func.pp0_energy * kNanoToBase
           << "\t" << func.time / total_time * 100.0
           << "\t" << func.package_energy / total_package_energy * 100.0
           << "\t" << func.pp0_energy / total_pp0_energy * 100.0
           << "\t" << func.package_energy/(double)func.time 
           << "\t" << func.pp0_energy/(double)func.time 
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

