#pragma once
#include <vector>
#include <string>

namespace papi{
struct event_info_t{
  int component;
  int set;
  std::vector<int> codes;
  std::vector<std::string> names;
};

std::vector<event_info_t> init_papi_counters(
    const std::vector<std::string>& event_names) {
  std::vector<event_info_t> eventsets;
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
      std::cerr << "Error: bad PAPI create eventset: ";
      PAPI_perror(NULL);
      exit(-1);
    }
    PAPI_add_events(eventset, &event.codes[0], event.codes.size());
    if (retval != PAPI_OK) {
      std::cerr << "Error: bad PAPI add eventset: ";
      PAPI_perror(NULL);
      exit(-1);
    }
    event.set = eventset;
  }
  return eventsets;
}

void attach_counters_to_core(const std::vector<event_info_t>& counters, int cpu_num) {
  for(auto& counter : counters){
    PAPI_option_t options;
    options.cpu.eventset = counter.set;
    options.cpu.cpu_num = cpu_num;
    int retval = PAPI_set_opt(PAPI_CPU_ATTACH, &options);
    if(retval != PAPI_OK) {
      std::cerr << "Error: unable to CPU_ATTACH core " << cpu_num << ": ";
      PAPI_perror(NULL);
      exit(-1);
    }
  }
}

void start_counters(const std::vector<event_info_t>& counters){
  for(auto& counter : counters){
    auto retval = PAPI_start(counter.set);
    if (retval != PAPI_OK) {
      std::cerr << "Error: bad PAPI start eventset: ";
      PAPI_perror(NULL);
      exit(-1);
    }
  }
}

std::vector<std::vector<long long>> stop_counters(const std::vector<event_info_t>& counters){
  std::vector<std::vector<long long>> results;
  for(const auto& counter : counters){
    std::vector<long long> counter_results(counter.codes.size());
    auto retval = PAPI_stop(counter.set, &counter_results[0]);
    if(retval != PAPI_OK){
      std::cerr << "Error: bad PAPI stop eventset: ";
      PAPI_perror(NULL);
      exit(-1);
    }
    results.push_back(counter_results);
  }
  return results;
}

}
