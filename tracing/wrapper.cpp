#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <thread>
#include <fstream>

#include "papi-helpers.hpp"

using namespace std;
using namespace papi;

template<typename T>
void print_comma_separated(std::ofstream& out, const std::vector<T>& things) {
  const auto& last = things.back();
  for(const auto& elem : things) {
    if(&elem != &last){
      out << elem << ",";
    } else {
      out << elem;
    }
  }
  out << "\n";
}

int main(int argc, char* argv[], char* envp[]){

  auto usage = 
    "Usage:\n"
    " eaudit-wrapper [options] <executable>\n"
    "\n"
    "Options:\n"
    " -h                      Show this help\n"
    " -p <counter>            Name of per-core PAPI counter to initialize lwperf with. Can have multiple.\n"
    " -g <counter>            Name of global PAPI counter to initialize lwperf with. Can have multiple.\n"
    " -i <invariant>=<value>  Pass invariant name--value pairs to lwperf to use for this run.\n"
    " -t                      Turn on timing\n"
    "\n";

  vector<string> papi_global_counter_strings, papi_local_counter_strings;
  map<string, double> invariants;
  bool do_timing = false;

  int param;
  while((param = getopt(argc, argv, "+hp:g:i:t")) != -1){
    switch(param){
      case 'p':{
        papi_local_counter_strings.emplace_back(optarg);
      } break;
      case 'g':{
        papi_global_counter_strings.emplace_back(optarg);
      } break;
      case 'i':{
        string val = optarg;
        auto sep_idx = val.find('=');
        string ivar_name = val.substr(0, sep_idx);
        double ivar_val = stod(val.substr(sep_idx + 1));
        invariants[ivar_name] = ivar_val;
      } break;
      case 't':{
        do_timing = true;
      } break;
      case 'h':
      case '?':
        cout << usage;
        return 0;
      default:
        cerr << "Error: bad getopt parse of parameter.\n";
        return -1;
    }
  }

  // initialize papi
  int retval;
  if ((retval = PAPI_library_init(PAPI_VER_CURRENT)) != PAPI_VER_CURRENT) {
    cerr << "Unable to init PAPI library - " << PAPI_strerror(retval) << endl;
    exit(-1);
  }

  auto ncores = thread::hardware_concurrency();
  vector<event_info_t> core_counters;
  for(unsigned i = 0; i < ncores; ++i){
    core_counters.emplace_back(init_papi_counters(papi_local_counter_strings));
    auto& counters = core_counters[i];
    attach_counters_to_core(counters, i);
  }
  auto global_counters = init_papi_counters(papi_global_counter_strings);
  long long start_time = 0;

  auto child_pid = fork();
  if(child_pid == -1){
    cerr << "Error forking new process\n";
    return -1;
  }

  if (child_pid == 0){ // child
    execve(argv[optind], &argv[optind], envp);
  } else { // parent
    for(const auto& counters : core_counters){
      start_counters(counters);
    }
    start_counters(global_counters);
    if(do_timing){
      start_time = PAPI_get_real_usec();
    }
    int status;
    // wait on child to terminate
    while(true){
      wait(&status);
      if(WIFEXITED(status)){
        break;
      }
    }
  }

  // stop counting energy
  vector<long long> results(core_counters[0].codes.size(), 0);
  vector<string> results_names(core_counters[0].names);
  for(const auto& counters : core_counters){
    auto core_result = stop_counters(counters);
    for(unsigned i = 0; i < results.size(); ++i){
      results[i] += core_result[i];
    }
  }
  auto global_results = stop_counters(global_counters);
  results.insert(end(results), begin(global_results), end(global_results));
  results_names.insert(end(results_names), begin(global_counters.names),
                       end(global_counters.names));
  if(do_timing){
    auto elapsed_time = PAPI_get_real_usec() - start_time;

    results.push_back(elapsed_time);
    results_names.push_back("ElapsedUsecs");
  }

  for(const auto& invariant : invariants){
    results_names.push_back(invariant.first);
    results.push_back(invariant.second);
  }

  std::ofstream ofile("wrapped.csv", std::ios::app);
  // only write headers if output file is empty
  if(ofile.tellp() == 0){
    print_comma_separated(ofile, results_names);
  }
  print_comma_separated(ofile, results);

  return 0;
}

