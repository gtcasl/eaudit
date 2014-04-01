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
const unsigned kSleepSecs = 30;
const unsigned kSleepUsecs = 0;
const double kNanoToBase = 1e-9;
const int kOverflowThreshold = 10000000;
const long long kOneMS = 1000000;
const long long kCounterMax = numeric_limits<unsigned int>::max();
const char* kCounterNames[] = {
  (char*) "rapl:::PACKAGE_ENERGY:PACKAGE0",
  (char*) "rapl:::PP0_ENERGY:PACKAGE0",
  (char*) "PAPI_TOT_INS",
  (char*) "PAPI_TOT_CYC"
};
constexpr int kNumCounters = sizeof(kCounterNames) / sizeof(kCounterNames[0]);
}

struct stats_t {
  long long time;
  long long counters[kNumCounters];
};

inline stats_t operator+(stats_t lhs, const stats_t& rhs) {
  stats_t sum;
  sum.time = lhs.time + rhs.time;
  for (int i = 0; i < kNumCounters; ++i) {
    sum.counters[i] = lhs.counters[i] + rhs.counters[i];
  }
  return sum;
}

stats_t& last_stats(){
  static stats_t last_stats_;
  return last_stats_;
}

stack<stats_t, vector<stats_t> >& cur_stats(){
  static stack<stats_t, vector<stats_t> > cur_stats_;
  return cur_stats_;
}

#ifdef EAUDIT_RECORD_ALL
map<string, vector<stats_t> >& total_stats(){
  static map<string, vector<stats_t> >* total_stats_ = new map<string, vector<stats_t> >();
  return *total_stats_;
}
#else
map<string, stats_t>& total_stats(){
  static map<string, stats_t>* total_stats_ = new map<string, stats_t>();
  return *total_stats_;
}
#endif

map<int, vector<int> >& component_events(){
  static map<int, vector<int> > component_events_;
  return component_events_;
}

vector<int>& get_eventsets(){
  static vector<int> eventsets;
  static int init = 1;
  if(init == 1){
    init_papi(eventsets);
    init = 0;
  }
  return eventsets;
}

void handler(int eset, void* addr, long long oflowvec, void* context){
  cout << "===========================handler!" << endl;
}

void init_papi(vector<int>& eventsets){
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

  int ecode, eset = PAPI_NULL;
  PAPI_create_eventset(&eset);
  PAPI_event_name_to_code((char*) "rapl:::PACKAGE_ENERGY:PACKAGE0", &ecode);
  PAPI_add_event(eset, ecode);
  retval = PAPI_overflow(eset, ecode, 1000000000, PAPI_OVERFLOW_FORCE_SW, handler);
  switch(retval){
    case PAPI_OK:
      cout << "ok" << endl;
      break;
    case PAPI_EINVAL:
      cout << "inval" << endl;
      break;
    case PAPI_ENOMEM:
      cout << "nomem" << endl;
      break;
    case PAPI_ENOEVST:
      cout << "noevst" << endl;
      break;
    case PAPI_EISRUN:
      cout << "isrun" << endl;
      break;
    case PAPI_ECNFLCT:
      cout << "cnflct" << endl;
      break;
    case PAPI_ENOEVNT:
      cout << "noevnt" << endl;
      break;
  }

/*
  for(auto& event_name : kCounterNames){
    int event_code;
    retval = PAPI_event_name_to_code((char*) event_name, &event_code);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
    int component = PAPI_get_event_component(event_code);
    component_events()[component].push_back(event_code);
  }

  for(auto& component : component_events()){
    int eventset = PAPI_NULL;
    PAPI_create_eventset(&eventset);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
    PAPI_add_events(eventset, &component.second[0], component.second.size());
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
    eventsets.push_back(eventset);
  }

  for(auto& eventset : eventsets){
    retval=PAPI_start(eventset);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
  }

  // set up signal handler
  if(signal(SIGALRM, overflow) == SIG_ERR){
    fprintf(stderr, "Unable to set up signal handler\n");
    exit(-1);
  }

  // Set a dummy first element so that the first do_push has some place to put 
  // the previous function energy.
  cur_stats().emplace();

  struct itimerval work_time;
  work_time.it_value.tv_sec = kSleepSecs;
  work_time.it_value.tv_usec = kSleepUsecs;
  work_time.it_interval.tv_sec = kSleepSecs;
  work_time.it_interval.tv_usec = kSleepUsecs;
  setitimer(ITIMER_REAL, &work_time, nullptr);
  */
}

bool read_rapl(){
  auto eventsets = get_eventsets();
  return true;
  long long curtime = PAPI_get_real_nsec();
  print("timediff: %f\n", (curtime - last_stats().time) * (double) kNanoToBase);
  if(curtime - last_stats().time > kOneMS){
    long long cntr_vals[kNumCounters];
    int cntr_offset = 0;
    for(int i = 0; i < eventsets.size(); ++i){
      int retval=PAPI_read(eventsets[i], cntr_vals + cntr_offset);
      if(retval != PAPI_OK){
        PAPI_perror(NULL);
        exit(-1);
      }
      cntr_offset += component_events()[eventsets[i]].size();
    }

    for(int i = 0; i < kNumCounters; ++i){
      long long total;
      if(cntr_vals[i] < last_stats().counters[i]){
        total = kCounterMax - last_stats().counters[i] + cntr_vals[i];
      } else {
        total = cntr_vals[i] - last_stats().counters[i];
      }
      cur_stats().top().counters[i] += total;
    }

    cur_stats().top().time += curtime - last_stats().time;
    stats_t last_stat;
    last_stat.time = curtime;
    for(int i = 0; i < kNumCounters; ++i){
      last_stat.counters[i] = cntr_vals[i];
    }
    last_stats() = last_stat;
    return true;
  }
  return false;
}

void EAUDIT_push(){
  print("push\n");
  read_rapl();
  cur_stats().emplace();
}

void EAUDIT_pop(const char* func_name){
  print("popping %s\n", func_name);
  auto did_read = read_rapl();
  if(did_read){
    print("good read!\n");
#ifdef EAUDIT_RECORD_ALL
    total_stats()[func_name].emplace_back(cur_stats().top());
#else
    auto& top = cur_stats().top();
    auto& total = total_stats()[func_name];
    total = total + top;
#endif
  } 
  cur_stats().pop();
}

void EAUDIT_shutdown(){
  print("shutdown\n");
  struct itimerval work_time;
  work_time.it_value.tv_sec = 0;
  work_time.it_value.tv_usec = 0;
  work_time.it_interval.tv_sec = 0;
  work_time.it_interval.tv_usec = 0;
  setitimer(ITIMER_REAL, &work_time, nullptr);

#ifdef EAUDIT_RECORD_ALL
  vector<pair<string, vector<stats_t> > > stats;
#else
  vector<pair<string, stats_t> > stats;
#endif
  cout << "size: " << total_stats().size() << endl;
  for(auto& func : total_stats()){
    auto name = demangle_func_name(func.first);
    stats.emplace_back(name, func.second);
  }

  stable_sort(stats.begin(), stats.end(),
#ifdef EAUDIT_RECORD_ALL
      [](const pair<string, vector<stats_t> >& a,
         const pair<string, vector<stats_t> >& b){
        stats_t sum_a = accumulate(a.second.begin(), a.second.end(), stats_t{});
        stats_t sum_b = accumulate(b.second.begin(), b.second.end(), stats_t{});
        return sum_a.time > sum_b.time;
      }
#else
      [](const pair<string, stats_t>& a,
         const pair<string, stats_t>& b){ return a.second.time > b.second.time; }
#endif
      );

  ofstream myfile;
  myfile.open("eaudit.tsv");
  myfile << "Func Name" 
         << "\t" << "Time(s)";
#ifdef EAUDIT_RECORD_ALL
  myfile << "\tavg\tstddev";
#endif
  for(int i = 0; i < kNumCounters; ++i){
    myfile << "\t" << kCounterNames[i];
#ifdef EAUDIT_RECORD_ALL
    myfile << "\tavg\tstddev";
#endif
  }
  myfile << endl;

  for(auto& func : stats){
#ifdef EAUDIT_RECORD_ALL
    stats_t func_sum = accumulate(func.second.begin(), func.second.end(), stats_t{});
    if(func_sum.time == 0) continue;
    stats_t func_dev;
    auto count = func.second.size();
    stats_t func_avg;
    func_avg.time = func_sum.time / (double) count;
    for(int i = 0; i < kNumCounters; ++i){
      func_avg.counters[i] = func_sum.counters[i] / (double) count;
    }
    for(auto& val : func.second){
      func_dev.time += pow(val.time - func_avg.time, 2);
      for(int i = 0; i < kNumCounters; ++i){
        func_dev.counters[i] += pow(val.counters[i] - func_avg.counters[i], 2);
      }
    }
    func_dev.time = sqrt(func_dev.time / count);
    for(int i = 0; i < kNumCounters; ++i){
      func_dev.counters[i] = sqrt(func_dev.counters[i] / count);
    }
    myfile << func.first
           << "\t" << func_sum.time * kNanoToBase
           << "\t" << func_avg.time * kNanoToBase
           << "\t" << func_dev.time * kNanoToBase;
    for(int i = 0; i < kNumCounters; ++i){
      myfile << "\t" << func_sum.counters[i]
             << "\t" << func_avg.counters[i]
             << "\t" << func_dev.counters[i];
    }
#else
    if(func.second.time == 0) continue;
    myfile << func.first
           << "\t" << func.second.time * kNanoToBase;
    for(int i = 0; i < kNumCounters; ++i){
      myfile << "\t" << func.second.counters[i];
    }
#endif
    myfile << endl;
  }
  myfile.close();
}

void overflow(int signum){
  if(signum == SIGALRM){
    print("overflow\n");
    read_rapl();
  }
}

