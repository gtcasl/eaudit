#include <sstream>
#include <cxxabi.h>
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
#include <thread>
#include <mutex>

#include <sys/user.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include "papi.h"

#ifdef DEBUG
#define print(...) printf(__VA_ARGS__)
#else
#define print(...)
#endif

using namespace std;

namespace{
const unsigned kSleepSecs = 0;
const unsigned kSleepUsecs = 500;
const double kNanoToBase = 1e-9;
const long long kBaseToNano = 1000000000;
const long long kMicroToNano = 1000;
const long long kCounterMax = numeric_limits<unsigned int>::max();
const char* kOutFileName = "eaudit.tsv";
const char* kCounterNames[] = {
  //(char*) "rapl:::PACKAGE_ENERGY:PACKAGE0",
  //(char*) "rapl:::PP0_ENERGY:PACKAGE0",
  (char*) "PAPI_TOT_INS",
  (char*) "PAPI_TOT_CYC",
};
constexpr int kNumCounters = sizeof(kCounterNames) / sizeof(kCounterNames[0]);
}


vector<int> children_pids;
mutex pid_mutex;


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


inline stats_t operator+(stats_t lhs, const stats_t& rhs) {
  return lhs += rhs;
}


struct event_info_t{
  int component;
  int eventset;
  vector<int> eventcodes;
};


stats_t read_rapl(const vector<event_info_t>& eventsets){
  stats_t res;
  int cntr_offset = 0;
  for(const auto& eventset : eventsets){
    int retval=PAPI_stop(eventset.eventset, res.counters + cntr_offset);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
    retval = PAPI_start(eventset.eventset);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
    cntr_offset += eventset.eventcodes.size();
  }
  res.time = kSleepSecs * kBaseToNano + kSleepUsecs * kMicroToNano;
  return res;
}


void listen_for_threading_ops() {
  int status;
  cout << "Starting to listen for new threads...\n";
  for(;;){
    auto halted_pid = waitpid(-1, &status, __WALL); // wait for the children processes to make a move
    if(status>>8 == (SIGTRAP | (PTRACE_EVENT_CLONE<<8))) { // new thread created
      cout << "New thread created.\n";
      lock_guard<mutex> lock{pid_mutex};
      unsigned long new_pid;
      ptrace(PTRACE_GETEVENTMSG, halted_pid, nullptr, &new_pid);
      auto pid_iter = find(begin(children_pids), end(children_pids), new_pid);
      if(pid_iter != end(children_pids)) {
        cerr << "Already have this newly cloned pid: " << new_pid << ".\n";
        exit(-1);
      }
      cout << "Thread ID " << new_pid << " created from thread ID " << halted_pid << "\n";
      children_pids.push_back(new_pid);
    } else if(WIFSTOPPED(status)){
      cout << "Halted thread " << halted_pid << "\n";
      ptrace(PTRACE_CONT, halted_pid, nullptr, nullptr);
    } else {
      cout << "Something else??\n";
      ptrace(PTRACE_CONT, halted_pid, nullptr, nullptr);
    }
  }
}


void do_profiling(int profilee_pid, char* profilee_name) {
  /*
   * Structures holding profiling data
   */
  vector<event_info_t> eventsets;
  vector<string> counter_names;
  map<void*, stats_t> stat_map;

  /*
   * Initialize PAPI
   */
  print("Init PAPI\n");
  int retval;
  if ((retval = PAPI_library_init(PAPI_VER_CURRENT)) != PAPI_VER_CURRENT) {
    fprintf(stderr, "Unable to init PAPI library.\n");
    switch (retval) {
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

  for (auto& event_name : kCounterNames) {
    int event_code;
    retval = PAPI_event_name_to_code((char*)event_name, &event_code);
    if (retval != PAPI_OK) {
      PAPI_perror(NULL);
      exit(-1);
    }
    int component = PAPI_get_event_component(event_code);
    auto elem = find_if(
        begin(eventsets), end(eventsets),
        [&](const event_info_t& c) { return c.component == component; });
    if (elem == end(eventsets)) {
      event_info_t new_info;
      new_info.component = component;
      new_info.eventcodes.push_back(event_code);
      eventsets.push_back(new_info);
    } else {
      elem->eventcodes.push_back(event_code);
    }
  }

  for (auto& event : eventsets) {
    int eventset = PAPI_NULL;
    PAPI_create_eventset(&eventset);
    if (retval != PAPI_OK) {
      PAPI_perror(NULL);
      exit(-1);
    }
    for (const auto& eventcode : event.eventcodes) {
      char name[PAPI_MAX_STR_LEN];
      PAPI_event_code_to_name(eventcode, name);
      counter_names.emplace_back(name);
    }
    PAPI_add_events(eventset, &event.eventcodes[0], event.eventcodes.size());
    if (retval != PAPI_OK) {
      PAPI_perror(NULL);
      exit(-1);
    }
    event.eventset = eventset;
    retval = PAPI_start(eventset);
    if (retval != PAPI_OK) {
      PAPI_perror(NULL);
      exit(-1);
    }
  }

  /*
   * Setup tracing of all profilee threads
   */
  // 0. Add profilee's pid to list of children pids, since it's technically
  // a thread too.
  children_pids.push_back(profilee_pid);
  // 1. Tell ptrace to notify us when a new thread is created
  // ptrace(PTRACE_CONT, profilee_pid, nullptr, nullptr);
  ptrace(PTRACE_SETOPTIONS, profilee_pid, nullptr,
         PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE);
  // 2. Start up listener thread for monitoring thread creation/destruction
  thread listener_thread{listen_for_threading_ops};

  /*
   * Let the profilee run, periodically interrupting to collect profile data.
   */
  print("Start profiling.\n");
  ptrace(PTRACE_CONT, profilee_pid, nullptr, nullptr);
  struct user_regs_struct regs;
  for (;;) {
    usleep(kSleepUsecs);
    pid_mutex.lock();
    // if we have no children, quit
    if(children_pids.size() == 0){
      break;
    }
    // kill all the children
    for(const auto& child : children_pids){
      kill(child, SIGINT);
    }
    // wait for all threads to halt
    for(const auto& child : children_pids){
      int status;
      cout << "waiting on pid " << child << "\n";
      auto waitret = waitpid(child, &status, __WALL);
      if(waitret == -1){
        cerr << "Bad wait!\n";
        exit(-1);
      }
    }
    // read all the children registers
    for(const auto& child : children_pids){
      // TODO make sure we wait for SIGINT handler, not clone()
      ptrace(PTRACE_GETREGS, child, nullptr, &regs);
      void* rip = (void*)regs.rip;
      auto rapl = read_rapl(eventsets);
      stat_map[rip] += rapl;
    }
    // resume all children
    for(const auto& child : children_pids){
      ptrace(PTRACE_CONT, child, nullptr, nullptr);
    }
    pid_mutex.unlock();
  }

  /*
   * Done profiling. Convert data to output file.
   */
  print("Finalize profile.\n");
  vector<pair<string, stats_t> > stats;
  // Convert stack IDs into function names.
  for (auto& func : stat_map) {
    stringstream cmd;
    cmd << "addr2line -f -s -C -e " << profilee_name << " " << func.first;
    auto pipe = popen(cmd.str().c_str(), "r");  // call command and read output
    if (!pipe) {
      cerr << "Unable to open pipe to call addr2line.\n";
      return;
    }
    char buffer[128];
    string result = "";
    while (!feof(pipe)) {
      if (fgets(buffer, 128, pipe) != nullptr) {
        result += buffer;
      }
    }
    pclose(pipe);
    stringstream resultstream{result};
    string line;
    getline(resultstream, line);

    auto stat = find_if(
        begin(stats), end(stats),
        [&](const pair<string, stats_t>& obj) { return obj.first == line; });
    if (stat == end(stats)) {
      stats.emplace_back(line, func.second);
    } else {
      stat->second += func.second;
    }
  }

  stable_sort(stats.begin(), stats.end(), [](const pair<string, stats_t>& a,
                                             const pair<string, stats_t>& b) {
    return a.second.time > b.second.time;
  });

  /*
   * Write profile to file
   */
  ofstream myfile;
  myfile.open(kOutFileName);
  myfile << "Func Name"
         << "\t"
         << "Time(s)";
  for (const auto& name : counter_names) {
    myfile << "\t" << name;
  }
  myfile << endl;

  for (auto& func : stats) {
    myfile << func.first << "\t" << func.second.time* kNanoToBase;
    for (int i = 0; i < kNumCounters; ++i) {
      myfile << "\t" << func.second.counters[i];
    }
    myfile << endl;
  }
  myfile.close();
}

int main(int argc, char* argv[]) {
  /*
   * Fork a process to run the profiled application
   */
  auto profilee = fork();
  if(profilee > 0){ /* parent */
    // Let's do this.
    int status;
    wait(&status);
    if(WIFEXITED(status)){
      cerr << "Child exited too fast.\n";
      return 0;
    }
    do_profiling(profilee, argv[1]);
  } else if(profilee == 0){ /* profilee */
    // prepare for tracing
    ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
    // start up client program
    execve(argv[1], &argv[1], nullptr);
    cerr << "Error: profilee couldn't start its program!\n";
    exit(-1);
  } else { /* error */
    cerr << "Error: couldn't fork audited program.\n";
    return -1;
  }
}

