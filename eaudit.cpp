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
#include <bfd.h>
#include <cstring>
#include <link.h>
#include <queue>

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


int read_target_byte(char *value, int child_pid, long address) {
	int ret = 0;
	/* Read a word at the target address, word aligned */
  auto word_size = 4;
	long aligned_address = address & ~(word_size - 1);
	int byte = address - aligned_address;
	ret = ptrace(PTRACE_PEEKDATA, child_pid, aligned_address);
	if (errno) {
		ret = errno;
	} else {
		*value = (ret >> (byte*8) ) & 0xff; 
		ret = 0;
	}
	return ret;
}


int read_target_memory(char *value, size_t length, int child_pid, long address) {
	/* We need to read word-aligned, otherwise ptrace blows up */
	int ret = 0;
	for (size_t offset = 0; offset < length; offset++ ) {
		ret = read_target_byte(value+offset, child_pid, address + offset);
		if (errno) {
			ret = errno;
			break;
		} else {
			ret = 0;
		}
	}
	return ret; 
}


int read_target_pointer(long *value, int child_pid, long address)
{
	int ret = 0;
	ret = ptrace(PTRACE_PEEKDATA, child_pid, address, 0);
	if (errno) {
		ret = errno;
    perror(nullptr);
	} else {
		*value = (long) ret;
		ret = 0;
	}
	return ret;
}


int read_target_string(char **value, int child_pid, long address)
{
	int ret = 0;
	int length = 0;
	char byte = 0;
	int offset = 0;
	
	/* First find out the string length */
	do {
		ret = read_target_byte(&byte, child_pid, address + length);	
		if (ret) {
      cerr << "Failed to read string.\n";
			return ret;
		}
		if (byte) {
			length++;
		}
	} while (byte);
	/* Now allocate memory for the string and terminator */
	*value = (char*) malloc(length + 1);
	if (NULL == *value) {
		fprintf(stderr,"Failed to allocate string buffer in read_target_string\n");
		return ENOMEM;
	}

	for (offset = 0; offset < (length + 1); offset ++) {
		ret = read_target_byte((*value + offset), child_pid, address+offset);
		if (ret) {
			fprintf(stderr,"Failed to read string from target: %s\n",strerror(ret));
			break;
		} 
	}
	return ret;
}


using symbol = pair<string,long>;

void add_symbols_from_file(vector<symbol>& symbols, string fname, long base_addr) {
  // 1. initialize bfd symbol table
  cout << " Opening file " << fname << "\n";
	auto file = bfd_openr (fname.c_str(), nullptr);
  if(file == nullptr){
    cerr << "Unable to open bfd.\n";
    exit(-1);
  }
	bfd_check_format(file, bfd_object);
  cout << "Opened bfd file.\n";
  bool is_static = true;
  auto storage_needed = bfd_get_symtab_upper_bound(file);
  if(storage_needed == 0){
    /* Trying dynamic */
    storage_needed = bfd_get_dynamic_symtab_upper_bound(file);
    is_static = false;
  }
  if(storage_needed == 0){
    cerr << "Storage for symbol table 0.\n";
    exit(-1);
  }
  cout << "Need " << storage_needed << " bytes for symbol table.\n";
  auto symbol_table = (asymbol**) malloc(storage_needed);
  cout << "Canonicalizing table.\n";
  auto num_symbols = is_static
                         ? bfd_canonicalize_symtab(file, symbol_table)
                         : bfd_canonicalize_dynamic_symtab(file, symbol_table);
  cout << "Found " << num_symbols << " symbols.\n";
  for(unsigned i = 0; i < num_symbols; ++i){
    symbols.emplace_back(bfd_asymbol_name(symbol_table[i]),
                         bfd_asymbol_value(symbol_table[i]));
  }
  cout << "Closing bfd file.\n";
  bfd_close(file);
}


vector<void*> get_symbol_addresses(const vector<string>& magic_names,
                                   int child_pid) {
  vector<void*> addrs;
  addrs.reserve(magic_names.size());

  stringstream exe_name;
  exe_name << "/proc/" << child_pid << "/exe";
  bfd_init();
  vector<symbol> symbols;
  // Note: starting with just static symbols
  add_symbols_from_file(symbols, exe_name.str(), 0 /* no base */);
  auto dynamic_iter =
      find_if(begin(symbols), end(symbols),
              [&](const symbol& s) { return s.first.compare("_DYNAMIC") == 0; });
  if(dynamic_iter != end(symbols)){
    cout << "Examining dynamic symbols.\n";
    auto dynamic = dynamic_iter->second;
    int keep_looking = 1;
    int found_it = 0;
    long r_debug_address;
    while (keep_looking) {
      ElfW(Dyn) thisdyn;
      auto ret = read_target_memory((char*)&thisdyn, sizeof( ElfW(Dyn) ), child_pid, dynamic);
      if (ret) {
        cerr << "Unable to read from DYNAMIC array.\n";
        exit(-1);
      }
      if (DT_NULL == thisdyn.d_tag) {
        cout << "Found DT_NULL entry.\n";
        keep_looking = 0;
      }
      if (DT_DEBUG == thisdyn.d_tag) {
        cout << "Found r_debug in _DYNAMIC array.\n";
        r_debug_address = thisdyn.d_un.d_ptr;
        cout << "dptr: " << thisdyn.d_un.d_ptr
             << "\ndval: " << thisdyn.d_un.d_val << "\n";
        keep_looking = 0;
        found_it = 1;
      }
      dynamic += sizeof( ElfW(Dyn) );
    }
    /* Found the r_debug symbol, so we keep inspecting */
    if(found_it){
      /* Get the link map head */
      long link_map_address;
      int r_map_offset = offsetof(struct r_debug, r_map);
      /* Now we've found the r_debug structure, get the link map from it. */
      auto ret = read_target_pointer(&link_map_address, child_pid, r_debug_address + r_map_offset );
      if (ret) {
        cerr << "Failed to read link map address.\n";
        exit(-1);
      }
      while(link_map_address){
        struct link_map lm; 
        auto ret = read_target_memory((char*)&lm, sizeof(lm), child_pid, link_map_address);
        if(ret){
          cerr << "Unable to read link map.\n";
          exit(-1);
        }
        char* so_file;
        ret = read_target_string(&so_file, child_pid, (long)lm.l_name);
        auto base_addr = lm.l_addr;
        link_map_address = (long) lm.l_next;

        add_symbols_from_file(symbols, so_file, base_addr);
      }
    } else {
      cout << "Unable to find r_debug symbol.\n";
    }
  }

  for(const auto& symbol : symbols){
    auto name = symbol.first;
    // 2. for each symbol in table, check if bfd_asymbol_name in magic_names
    auto magic_iter = find_if(begin(magic_names), end(magic_names),
                              [&](const string& n) { return n.compare(name) == 0; });
    // 3. if so, set the addr for that that name to bfd_asymbol_value (+ base_addr??)
    if(magic_iter != end(magic_names)){
      cout << "Found symbol " << *magic_iter << "\n";
      auto idx = distance(begin(magic_names), magic_iter);
      addrs[idx] = (void*) symbol.second;
    }
  }
  return addrs;
}


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


void do_profiling(int child, char* child_name) {
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
   * Initialize thread debugging info
   */
  vector<string> magic_names = {
      "__pthread_threads_debug",  "__pthread_handles",
      "__pthread_initial_thread", "__pthread_manager_thread",
      "__pthread_sizeof_handle",  "__pthread_offsetof_descr",
      "__pthread_offsetof_pid",   "__pthread_handles_num"};
  auto magic_addrs = get_symbol_addresses(magic_names, child);

  /*
   * Let the child run, periodically interrupting to collect profile data.
   */
  print("Start profiling.\n");
  ptrace(PTRACE_CONT, child, nullptr, nullptr);
  struct user_regs_struct regs;
  for (;;) {
    usleep(kSleepUsecs);
    kill(child, SIGINT);
    int status;
    wait(&status);
    if (WIFEXITED(status)) {
      break;
    }
    ptrace(PTRACE_GETREGS, child, nullptr, &regs);
    void* rip = (void*)regs.rip;
    auto rapl = read_rapl(eventsets);
    stat_map[rip] += rapl;
    ptrace(PTRACE_CONT, child, nullptr, nullptr);
  }

  /*
   * Done profiling. Convert data to output file.
   */
  print("Finalize profile.\n");
  vector<pair<string, stats_t> > stats;
  // Convert stack IDs into function names.
  for (auto& func : stat_map) {
    stringstream cmd;
    cmd << "addr2line -f -s -C -e " << child_name << " " << func.first;
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
  auto child = fork();
  if(child > 0){ /* parent */
    // Let's do this.
    int status;
    wait(&status);
    if(WIFEXITED(status)){
      cerr << "Child exited too fast.\n";
      return 0;
    }
    do_profiling(child, argv[1]);
  } else if(child == 0){ /* child */
    // prepare for tracing
    ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
    // start up client program
    execve(argv[1], &argv[1], nullptr);
    cerr << "Error: child couldn't start its program!\n";
    exit(-1);
  } else { /* error */
    cerr << "Error: couldn't fork audited program.\n";
    return -1;
  }
}

