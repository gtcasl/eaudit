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
void overflow(int signum, siginfo_t* info, void* context);
int init_papi();

namespace{
const unsigned kSleepSecs = 0;
const unsigned kSleepUsecs = 10000;
const double kNanoToBase = 1e-9;
const long long kBaseToNano = 1000000000;
const long long kMicroToNano = 1000;
const long long kCounterMax = numeric_limits<unsigned int>::max();
const int kBufSize = 1024;
const int kReadPipe = 0;
const int kWritePipe = 1;
const char* kBufFileName = "eaudit.out";
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
  stats_t& operator+=(const stats_t &rhs){
    time += rhs.time;
    for(int i = 0; i < kNumCounters; ++i){
      counters[i] += rhs.counters[i];
    }
    return *this;
  }
};

stats_t read_rapl();

struct trace_entry{
  void* return_addr;
  stats_t stats;
};

inline stats_t operator+(stats_t lhs, const stats_t& rhs) {
  return lhs += rhs;
}

stats_t& last_stats(){
  static stats_t last_stats_;
  return last_stats_;
}

map<int, vector<int> >& component_events(){
  static map<int, vector<int> > component_events_;
  return component_events_;
}

vector<int>& eventsets(){
  static vector<int> eventsets_;
  return eventsets_;
}

static int inited = init_papi();
static int myfd;

int init_papi(){
  assert(kSleepSecs > 0 || kSleepUsecs > 1000 && "ERROR: must sleep for more than 1ms");
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
    eventsets().push_back(eventset);
  }

  for(auto& eventset : eventsets()){
    retval=PAPI_start(eventset);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
  }

  // set up signal handler
  struct sigaction sa;
  sa.sa_sigaction = overflow;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  if(sigaction(SIGALRM, &sa, nullptr) != 0){
    fprintf(stderr, "Unable to set up signal handler\n");
    exit(-1);
  }

  if((myfd = open(kBufFileName, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)) == -1){
    cerr << "Unable to open file" << endl;
    exit(-1);
  }

  struct itimerval work_time;
  work_time.it_value.tv_sec = kSleepSecs;
  work_time.it_value.tv_usec = kSleepUsecs;
  work_time.it_interval.tv_sec = kSleepSecs;
  work_time.it_interval.tv_usec = kSleepUsecs;
  setitimer(ITIMER_REAL, &work_time, nullptr);
  return 1;
}

stats_t read_rapl(){
  auto esets = eventsets();
  long long cntr_vals[kNumCounters];
  int cntr_offset = 0;
  for(int i = 0; i < esets.size(); ++i){
    int retval=PAPI_read(esets[i], cntr_vals + cntr_offset);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
    cntr_offset += component_events()[esets[i]].size();
  }

  stats_t cur_stats;
  for(int i = 0; i < kNumCounters; ++i){
    long long total;
    if(cntr_vals[i] < last_stats().counters[i]){
      total = kCounterMax - last_stats().counters[i] + cntr_vals[i];
    } else {
      total = cntr_vals[i] - last_stats().counters[i];
    }
    cur_stats.counters[i] = total;
  }

  cur_stats.time = kSleepSecs * kBaseToNano + kSleepUsecs * kMicroToNano;
  stats_t last_stat;
  for(int i = 0; i < kNumCounters; ++i){
    last_stat.counters[i] = cntr_vals[i];
  }
  last_stats() = last_stat;
  return cur_stats;
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

  ssize_t nread;
  trace_entry entry;
  map<string, stats_t> stat_map;
  lseek(myfd, 0, SEEK_SET);
  while((nread = read(myfd, (void*) &entry, sizeof(entry))) == sizeof(entry)){
    auto symbol = backtrace_symbols(&entry.return_addr, 1)[0];
    auto mangled = parse_backtrace_entry(symbol);
    auto name = demangle_func_name(mangled);
    stat_map[name] += entry.stats;
  }

  vector<pair<string, stats_t> > stats;
  for(auto& func : stat_map){
    stats.emplace_back(func.first, func.second);
  }

  stable_sort(stats.begin(), stats.end(),
      [](const pair<string, stats_t>& a,
        const pair<string, stats_t>& b){ return a.second.time > b.second.time; }
      );

  ofstream myfile;
  myfile.open("eaudit.tsv");
  myfile << "Func Name" 
         << "\t" << "Time(s)";
  for(int i = 0; i < kNumCounters; ++i){
    myfile << "\t" << kCounterNames[i];
  }
  myfile << endl;

  for(auto& func : stats){
    myfile << func.first
           << "\t" << func.second.time * kNanoToBase;
    for(int i = 0; i < kNumCounters; ++i){
      myfile << "\t" << func.second.counters[i];
    }
    myfile << endl;
  }
  myfile.close();
}

void overflow(int signum, siginfo_t* info, void* context){
  if(signum == SIGALRM){
    //print("overflow\n");

    ucontext_t* uc = (ucontext_t*) context;
    void* caller = (void*) uc->uc_mcontext.gregs[REG_RIP];
    trace_entry entry;
    entry.return_addr = (void*) uc->uc_mcontext.gregs[REG_RIP];
    entry.stats = read_rapl();
    auto res = write(myfd, &entry, sizeof(entry));
    if(res == -1){
      cerr << "Unable to write to pipe." << endl;
      exit(-1);
    }
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

