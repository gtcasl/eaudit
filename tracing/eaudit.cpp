#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/ptrace.h>
#include <sys/time.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "papi.h"

#ifdef DEBUG
#define print(...) printf(__VA_ARGS__)
#else
#define print(...)
#endif

using namespace std;

namespace{

/*
 * Constants
 */
const unsigned kProcStatIdx = 39;
const unsigned kSleepSecs = 0;
const unsigned kSleepUsecs = 1000;
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


struct event_info_t{
  int component;
  int set;
  vector<int> codes;
  vector<string> names;
};


/*
 * Global variables
 */
volatile bool is_timer_done = false;
} // end unnamed namespace


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
      cerr << "Error: bad PAPI stop: ";
      PAPI_perror(NULL);
      terminate();
    }
    retval = PAPI_start(eventset.set);
    if(retval != PAPI_OK){
      cerr << "Error: bad PAPI stop: ";
      PAPI_perror(NULL);
      terminate();
    }
    cntr_offset += eventset.codes.size();
  }
  res.time = kSleepSecs * kBaseToNano + kSleepUsecs * kMicroToNano;
  return res;
}


vector<event_info_t> init_papi_counters(int cpu_num) {
  vector<event_info_t> eventsets;
  int retval;
  for (auto& event_name : kCounterNames) {
    int event_code;
    retval = PAPI_event_name_to_code((char*)event_name, &event_code);
    if (retval != PAPI_OK) {
      cerr << "Error: bad PAPI event name to code: ";
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
      cerr << "Error: bad PAPI create eventset: ";
      PAPI_perror(NULL);
      exit(-1);
    }
    PAPI_add_events(eventset, &event.codes[0], event.codes.size());
    if (retval != PAPI_OK) {
      cerr << "Error: bad PAPI add eventset: ";
      PAPI_perror(NULL);
      exit(-1);
    }
    PAPI_option_t options;
    options.cpu.eventset = eventset;
    options.cpu.cpu_num = cpu_num;
    retval = PAPI_set_opt(PAPI_CPU_ATTACH, &options);
    if(retval != PAPI_OK) {
      cerr << "Error: unable to CPU_ATTACH core " << cpu_num << endl;
      exit(-1);
    }

    event.set = eventset;
    retval = PAPI_start(eventset);
    if (retval != PAPI_OK) {
      cerr << "Error: bad PAPI start eventset: ";
      PAPI_perror(NULL);
      exit(-1);
    }
  }
  return eventsets;
}


void do_profiling(int profilee_pid, char* profilee_name) {
  /*
   * Structures holding profiling data
   */
  vector<int> children_pids;
  vector<map<void*, stats_t>> thread_stats;
  vector<vector<event_info_t>> thread_counters;
  auto nthreads = thread::hardware_concurrency();
  //auto nthreads = 1u;
  thread_stats.resize(nthreads);
  thread_counters.reserve(nthreads);

  /*
   * Initialize PAPI
   */
  print("Init PAPI\n");
  int retval;
  if ((retval = PAPI_library_init(PAPI_VER_CURRENT)) != PAPI_VER_CURRENT) {
    cerr << "Unable to init PAPI library - " << PAPI_strerror(retval) << endl;
    exit(-1);
  }

  /*
   * Initialize PAPI measurement of all cores
   */
  for(unsigned int i = 0; i < nthreads; ++i){
    print("eaudit Starting cpu core %d\n", i);
    thread_counters.emplace_back(init_papi_counters(i));
  }

  /*
   * Setup tracing of all profilee threads
   */
  children_pids.push_back(profilee_pid);

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
  ptrace(PTRACE_CONT, profilee_pid, nullptr, nullptr); // Allow child to fork
  int status;
  wait(&status); // wait for child to begin executing
  print("Start profiling.\n");
  /* Reassert that we want the profilee to stop when it clones */
  ptrace(PTRACE_SETOPTIONS, profilee_pid, nullptr,
         PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT);
  ptrace(PTRACE_CONT, profilee_pid, nullptr, nullptr); // Allow child to run!
  for (;;) {
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

        // find last executing core ID for each child
        map<int, int> children_cores;
        for(const auto& child : children_pids){
          stringstream proc_fname;
          proc_fname << "/proc/" << child << "/stat";
          ifstream procfile(proc_fname.str());
          if(!procfile.is_open()){
            cerr << "Error: couldn't open proc file!\n";
            exit(-1);
          }
          string line;
          // ignore the first kProcStatIdx lines
          for(unsigned int i = 0; i < kProcStatIdx; ++i){
            getline(procfile, line, ' ');
          }
          children_cores[child] = stoi(line);
        }
        
        // collect stats from threads
        print("EAUDIT collating stats\n");
        // read all rapl counters
        vector<stats_t> stats;
        for(unsigned int i = 0; i < nthreads; ++i){
          stats.emplace_back(read_rapl(thread_counters[i]));
        }
        // read all the children registers
        for(const auto& child : children_pids){
          struct user_regs_struct regs;
          ptrace(PTRACE_GETREGS, child, nullptr, &regs);
          void* rip = (void*)regs.rip;
          auto child_core = children_cores[child];
          thread_stats[child_core][rip] += stats[child_core];
        }
        // resume all children
        for(const auto& child : children_pids){
          ptrace(PTRACE_CONT, child, nullptr, nullptr);
        }
        is_timer_done = false;
        // resume timer
        work_time.it_value.tv_sec = kSleepSecs;
        work_time.it_value.tv_usec = kSleepUsecs;
        setitimer(ITIMER_REAL, &work_time, nullptr);
      } else {
        cerr << "Error: unexpected return from wait - " << strerror(errno) << "\n";
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
  vector<vector<pair<string, stats_t>>> thread_profiles;
  for(unsigned int i = 0; i < nthreads; ++i){
    vector<pair<string, stats_t> > stats;
    // Convert stack IDs into function names.
    for (auto& func : thread_stats[i]) {
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
    thread_profiles.push_back(stats);
  }

  /*
   * Write profile to file
   */
  ofstream myfile;
  myfile.open(kOutFileName);
  for(unsigned int i = 0; i < nthreads; ++i){
    myfile << "===THREAD " << i << "\n";
    myfile << "Func Name"
           << "\t"
           << "Time(s)";
    for(const auto& eventset : thread_counters[i]){
      for(const auto& name : eventset.names){
        myfile << "\t" << name;
      }
    }
    myfile << endl;

    for (auto& func : thread_profiles[i]) {
      myfile << func.first << "\t" << func.second.time* kNanoToBase;
      for (int i = 0; i < kNumCounters; ++i) {
        myfile << "\t" << func.second.counters[i];
      }
      myfile << endl;
    }
    myfile << endl;
  }
  myfile.close();
}

int main(int argc, char* argv[]) {
  (void)argc;
  /*
   * Fork a process to run the profiled application
   */
  auto profilee = fork();
  if(profilee > 0){ /* parent */
    // Let's do this.
    ptrace(PTRACE_SETOPTIONS, profilee, nullptr,
           PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT);
    do_profiling(profilee, argv[1]);
  } else if(profilee == 0){ /* profilee */
    // prepare for tracing
    ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
    raise(SIGSTOP);
    // start up client program
    execve(argv[1], &argv[1], nullptr);
    cerr << "Error: profilee couldn't start its program!\n";
    exit(-1);
  } else { /* error */
    cerr << "Error: couldn't fork audited program.\n";
    return -1;
  }
}

