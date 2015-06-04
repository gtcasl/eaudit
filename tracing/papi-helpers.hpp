#pragma once
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>

#include "papi.h"

namespace papi{
struct event_info_t{
  int set;
  std::vector<int> codes;
  std::vector<std::string> names;
};

event_info_t init_papi_counters(const std::vector<std::string>& event_names) {
  event_info_t result;
  int retval;
  for (auto& event_name : event_names) {
    int event_code;
    retval = PAPI_event_name_to_code(const_cast<char*>(event_name.c_str()), 
                                     &event_code);
    if (retval != PAPI_OK) {
      std::cerr << "Error: bad PAPI event name \"" << event_name << "\" to code: ";
      PAPI_perror(NULL);
      exit(-1);
    }
    result.codes.push_back(event_code);
    result.names.push_back(event_name);
  }

  result.set = PAPI_NULL;
  retval = PAPI_create_eventset(&result.set);
  if (retval != PAPI_OK) {
    std::cerr << "Error: bad PAPI create eventset: ";
    PAPI_perror(NULL);
    exit(-1);
  }
  retval = PAPI_add_events(result.set, &result.codes[0], result.codes.size());
  if (retval != PAPI_OK) {
    std::cerr << "Error: bad PAPI add eventset: ";
    PAPI_perror(NULL);
    exit(-1);
  }

  return result;
}

void attach_counters_to_core(const event_info_t& counters, int cpu_num) {
  PAPI_option_t options;
  options.cpu.eventset = counters.set;
  options.cpu.cpu_num = cpu_num;
  int retval = PAPI_set_opt(PAPI_CPU_ATTACH, &options);
  if(retval != PAPI_OK) {
    std::cerr << "Error: unable to CPU_ATTACH core " << cpu_num << ": ";
    PAPI_perror(NULL);
    exit(-1);
  }
}

void start_counters(const event_info_t& counters){
  auto retval = PAPI_start(counters.set);
  if (retval != PAPI_OK) {
    std::cerr << "Error: bad PAPI start eventset: ";
    PAPI_perror(NULL);
    exit(-1);
  }
}

std::vector<long long> stop_counters(const event_info_t& counters){
  std::vector<long long> results(counters.codes.size());
  auto retval = PAPI_stop(counters.set, &results[0]);
  if(retval != PAPI_OK){
    std::cerr << "Error: bad PAPI stop eventset: ";
    PAPI_perror(NULL);
    exit(-1);
  }
  return results;
}

}
