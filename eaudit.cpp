#include <algorithm>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/ptrace.h>
#include <sys/time.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "papi.h"

#ifdef DEBUG
#define print(...) printf(__VA_ARGS__)
#else
#define print(...)
#endif

using namespace std;

namespace{
const unsigned kSleepSecs = 1;
const unsigned kSleepUsecs = 0;
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


volatile bool is_timer_done = false;
volatile bool is_done = false;
mutex profile_mutex;
condition_variable profile_cv;
vector<bool> should_profile;


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
  int set;
  vector<int> codes;
  vector<string> names;
};


void overflow(int signum, siginfo_t* info, void* context){
  (void)info;
  (void)context;
  if(signum == SIGALRM){
    is_timer_done = true;
  }
}


stats_t read_rapl(const vector<event_info_t>& eventsets){
  stats_t res;
  int cntr_offset = 0;
  for(const auto& eventset : eventsets){
    int retval=PAPI_stop(eventset.set, res.counters + cntr_offset);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
    retval = PAPI_start(eventset.set);
    if(retval != PAPI_OK){
      PAPI_perror(NULL);
      exit(-1);
    }
    cntr_offset += eventset.codes.size();
  }
  res.time = kSleepSecs * kBaseToNano + kSleepUsecs * kMicroToNano;
  return res;
}


vector<event_info_t> init_papi_counters() {
  vector<event_info_t> eventsets;
  int retval;
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
      new_info.codes.push_back(event_code);
      new_info.names.emplace_back(event_name);
      eventsets.push_back(new_info);
    } else {
      elem->codes.push_back(event_code);
      elem->names.emplace_back(event_name);
    }
  }

  for (auto& event : eventsets) {
    int eventset = PAPI_NULL;
    PAPI_create_eventset(&eventset);
    if (retval != PAPI_OK) {
      PAPI_perror(NULL);
      exit(-1);
    }
    PAPI_add_events(eventset, &event.codes[0], event.codes.size());
    if (retval != PAPI_OK) {
      PAPI_perror(NULL);
      exit(-1);
    }
    event.set = eventset;
    retval = PAPI_start(eventset);
    if (retval != PAPI_OK) {
      PAPI_perror(NULL);
      exit(-1);
    }
  }
  return eventsets;
}


void profile_core(int id) {
  /* Set affinity */
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

  /* Initialize PAPI counters for this core */
  auto counters = init_papi_counters();

  while(!is_done){
    // wait on condition variable
    {
      unique_lock<mutex> locker(profile_mutex);
      print("TID %d waiting\n", id);
      profile_cv.wait(locker, [&]{ return should_profile[id]; });
    }
    // do profiling
    print("TID %d profiling\n", id);
    // update output (global) data
    // go back to sleep
    {
      lock_guard<mutex> lock(profile_mutex);
      should_profile[id] = false;
    }
  }
  print("TID %d DONE\n", id);
}


void do_profiling(int profilee_pid, char* profilee_name) {
  /*
   * Structures holding profiling data
   */
  vector<int> children_pids;
  map<void*, stats_t> stat_map;
  vector<thread> threads;
  auto nthreads = thread::hardware_concurrency();
  threads.reserve(nthreads);
  {
    lock_guard<mutex> lock(profile_mutex);
    should_profile.resize(nthreads, false);
  }

  /*
   * Initialize PAPI
   */
  print("Init PAPI\n");
  int retval;
  if ((retval = PAPI_library_init(PAPI_VER_CURRENT)) != PAPI_VER_CURRENT) {
    cerr << "Unable to init PAPI library: ";
    switch (retval) {
      case PAPI_EINVAL:
        cerr << "einval\n";
        break;
      case PAPI_ENOMEM:
        cerr << "enomem\n";
        break;
      case PAPI_ESBSTR:
        cerr << "esbstr\n";
        break;
      case PAPI_ESYS:
        cerr << "esys\n";
        break;
      default:
        cerr << "\n";
    }
    exit(-1);
  }

  /*
   * Initialize PAPI measurement threads
   */
  if(PAPI_thread_init(pthread_self) != PAPI_OK){
    cerr << "Unable to init PAPI threads\n";
    exit(-1);
  }
  for(unsigned int i = 0; i < nthreads; ++i){
    print("eaudit Starting thread %d\n", i);
    threads.emplace_back(profile_core, i);
  }

  /*
   * Setup tracing of all profilee threads
   */
  children_pids.push_back(profilee_pid);
  ptrace(PTRACE_SETOPTIONS, profilee_pid, nullptr,
         PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT);

  /*
   * Set up timer
   */
  struct sigaction sa;
  sa.sa_sigaction = overflow;
  sa.sa_flags = SA_SIGINFO;
  if (sigaction(SIGALRM, &sa, nullptr) != 0) {
    cerr << "Unable to set up signal handler\n";
    exit(-1);
  }
  struct itimerval work_time;
  work_time.it_value.tv_sec = kSleepSecs;
  work_time.it_value.tv_usec = kSleepUsecs;
  work_time.it_interval.tv_sec = kSleepSecs;
  work_time.it_interval.tv_usec = kSleepUsecs;
  setitimer(ITIMER_REAL, &work_time, nullptr);

  /*
   * Let the profilee run, periodically interrupting to collect profile data.
   */
  print("Start profiling.\n");
  ptrace(PTRACE_CONT, profilee_pid, nullptr, nullptr);
  struct user_regs_struct regs;
  for (;;) {
    int status;
    //auto wait_res = wait(&status);
    auto wait_res = waitpid(-1, &status, __WALL);
    if(wait_res == -1){ // bad wait!
      if(errno == EINTR && is_timer_done){ // timer expired, do profiling
        // halt timer
        work_time.it_value.tv_sec = 0;
        work_time.it_value.tv_usec = 0;
        setitimer(ITIMER_REAL, &work_time, nullptr);
        
        // kill all the children
        for(const auto& child : children_pids){
          kill(child, SIGSTOP);
        }

        // TODO
        // find last executing core ID for each child
        // send signal requesting updates from each profiling thread associated with a child core id
        {
          lock_guard<mutex> lock(profile_mutex);
          for(unsigned int i = 0; i < nthreads; ++i){
            should_profile[i] = true;
          }
        }
        {
          unique_lock<mutex> locker(profile_mutex);
          print("EAUDIT notifying\n");
          profile_cv.notify_all();
        }
        // wait on output from profiling threads and accumulate


        /*

        // read all the children registers
        for(const auto& child : children_pids){
          ptrace(PTRACE_GETREGS, child, nullptr, &regs);
          void* rip = (void*)regs.rip;
          auto rapl = read_rapl(eventsets);
          stat_map[rip] += rapl;
        }
        */
        // resume all children
        for(const auto& child : children_pids){
          ptrace(PTRACE_CONT, child, nullptr, nullptr);
        }
        is_timer_done = false;
        // resume timer
        work_time.it_value.tv_sec = kSleepSecs;
        work_time.it_value.tv_usec = kSleepUsecs;
        setitimer(ITIMER_REAL, &work_time, nullptr);
        continue; // this isn't really necessary
      } else {
        cerr << "Error: unexpected return from wait.\n";
        exit(-1);
      }
    } else { // good wait, add new thread
      if(status>>8 == (SIGTRAP | (PTRACE_EVENT_CLONE<<8))) { // new thread created
        print("New thread created.\n");
        unsigned long new_pid;
        ptrace(PTRACE_GETEVENTMSG, wait_res, nullptr, &new_pid);
        auto pid_iter = find(begin(children_pids), end(children_pids), new_pid);
        if(pid_iter != end(children_pids)) {
          cerr << "Already have this newly cloned pid: " << new_pid << ".\n";
          exit(-1);
        }
        print("Thread ID %lu created from thread ID %d\n", new_pid, wait_res);
        children_pids.push_back(new_pid);
        ptrace(PTRACE_SETOPTIONS, new_pid, nullptr,
               PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT);
        ptrace(PTRACE_CONT, wait_res, nullptr, nullptr);
      } else {
        if(status>>8 == (SIGTRAP | (PTRACE_EVENT_EXIT<<8))){
          print("Deleting child %d\n", wait_res);
          auto pid_iter = find(begin(children_pids), end(children_pids), wait_res);
          if(pid_iter == end(children_pids)){
            cerr << "Error: Saw exit from pid " << wait_res << ". We haven't seen before!\n";
            exit(-1);
          }
          children_pids.erase(pid_iter);
          if(children_pids.size() == 0){ // All done, not tracking any more threads
            is_done = true;
            {
              lock_guard<mutex> lock(profile_mutex);
              for(unsigned int i = 0; i < nthreads; ++i){
                should_profile[i] = true;
              }
            }
            {
              unique_lock<mutex> locker(profile_mutex);
              profile_cv.notify_all();
            }
            for(auto& thread : threads){
              thread.join();
            }
            break;
          }
          print("%lu children left\n", children_pids.size());
        }
        // always let the stopped tracee continue
        ptrace(PTRACE_CONT, wait_res, nullptr, nullptr);
      }
    }
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
    print("Reporting function %s\n", line.c_str());

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
  /*
  ofstream myfile;
  myfile.open(kOutFileName);
  myfile << "Func Name"
         << "\t"
         << "Time(s)";
  for(const auto& eventset : eventsets){
    for(const auto& name : eventset.names){
      myfile << "\t" << name;
    }
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
  */
}

int main(int argc, char* argv[]) {
  (void)argc;
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

