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
#include <math.h>
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

#include <boost/numeric/ublas/matrix.hpp>
#include <boost/numeric/ublas/io.hpp>

#include "papi.h"

#include "supereasyjson/json.h"
#include "papi-helpers.hpp"

#ifdef DEBUG
#define print(...) printf(__VA_ARGS__)
#else
#define print(...)
#endif

using namespace std;
using namespace boost::numeric;
using namespace json;
using namespace papi;

namespace{

/*
 * Constants
 */
const unsigned kProcStatIdx = 39;
const long kDefaultSamplePeriodUsecs = 1000;
const long kMicroToBase = 1e6;
const long kNanoToBase = 1e9;
const char* kDefaultOutfile = "eaudit.tsv";
const string kEnergyEventName = "rapl:::PACKAGE_ENERGY:PACKAGE0";
const char* kDefaultModelName = "default.model";
const int kTotalCoreAssignments = 5;

struct stats_t {
  long time; // in microseconds
  vector<long long> counters;
  stats_t& operator+=(const stats_t &rhs){
    time += rhs.time;
    counters.resize(rhs.counters.size());
    for(unsigned int i = 0; i < rhs.counters.size(); ++i){
      counters[i] += rhs.counters[i];
    }
    return *this;
  }
  // initialize time to 0 for new stats
  stats_t() : time{0} {}
};


struct result_stats_t {
  stats_t per_core_stats;
  long long estimated_energy;
  result_stats_t& operator+=(const result_stats_t &rhs){
    per_core_stats += rhs.per_core_stats;
    estimated_energy += rhs.estimated_energy;
    return *this;
  }
};


/*
 * Global variables
 */
volatile bool is_timer_done = false;
} // end unnamed namespace


struct Model {
  Model(const string& model_fname) : model_fname_{model_fname} {
    ifstream in{model_fname_};
    if(!in.is_open()){
      cerr << "Unable to open model file\n";
      exit(-1);
    }
    stringstream buffer;
    buffer << in.rdbuf();
    json::Value modelval = json::Deserialize(buffer.str());
    if(modelval.GetType() == json::NULLVal){
      cerr << "Unable to parse json from model file\n";
      exit(-1);
    }
    json::Object model = modelval.ToObject();

    for(const auto& name : model["metric_names"].ToArray()){
      input_metrics_.push_back(name.ToString());
    }

    const auto means = model["means"].ToArray();
    means_.resize(means.size());
    for(size_t i = 0; i < means.size(); ++i){
      means_[i] = means[i].ToDouble();
    }

    const auto std_devs = model["std_devs"].ToArray();
    std_deviations_.resize(std_devs.size());
    for(size_t i = 0; i < std_devs.size(); ++i){
      std_deviations_[i] = std_devs[i].ToDouble();
    }

    const auto rot_mat = model["rotation_matrix"].ToArray();
    const auto n_rot_mat_rows = rot_mat.size();
    if(n_rot_mat_rows > 0){
      const auto n_rot_mat_cols = rot_mat[0].ToArray().size();
      principal_components_.resize(n_rot_mat_rows, n_rot_mat_cols);
      for(size_t i = 0; i < n_rot_mat_rows; ++i){
        for(size_t j = 0; j < n_rot_mat_cols; ++j){
          principal_components_(i,j) = rot_mat[i][j].ToDouble();
        }
      }
    }

    const auto clusters = model["clusters"].ToArray();
    for(const auto& cluster_val : clusters){
      const auto& cluster = cluster_val.ToObject();
      model_t internalized_model;

      const auto center = cluster["center"].ToArray();
      internalized_model.centroid_.resize(center.size());
      for(size_t i = 0; i < center.size(); ++i){
        internalized_model.centroid_[i] = center[i].ToDouble();
      }

      const auto regressors = cluster["regressors"].ToArray();
      internalized_model.regressors_.resize(regressors.size());
      internalized_model.weights_.resize(regressors.size());
      for(size_t i = 0; i < regressors.size(); ++i){
        // turn function object into a lambda doing the right thing
        auto regressor = regressors[i].ToObject();
        auto regressor_name = regressor["function"].ToString();
        if(regressor_name == "identity"){
          internalized_model.regressors_[i] = function<double(const ublas::vector<double>&)>(
                                   [](ublas::vector<double>){ return 1.0; });
        } else if(regressor_name == "power"){
          auto idx = regressor["index"].ToInt();
          auto exp = regressor["exponent"].ToDouble();
          internalized_model.regressors_[i] = function<double(const ublas::vector<double>&)>(
            [=](const ublas::vector<double>& v) {
              return v(idx) != 0.0 ? pow(fabs(v(idx)), exp) : 1.0;
            });
        } else if(regressor_name == "product"){
          auto first_idx = regressor["first_idx"].ToInt();
          auto second_idx = regressor["second_idx"].ToInt();
          internalized_model.regressors_[i] = function<double(const ublas::vector<double>&)>(
            [=](const ublas::vector<double>& v) {
              return v(first_idx) * v(second_idx);
            });
        } else if(regressor_name == "sqrt"){
          auto idx = regressor["index"].ToInt();
          internalized_model.regressors_[i] = function<double(const ublas::vector<double>&)>(
            [=](const ublas::vector<double>& v) {
              return sqrt(fabs(v(idx)));
            });
        } else if(regressor_name == "log"){
          auto idx = regressor["index"].ToInt();
          internalized_model.regressors_[i] = function<double(const ublas::vector<double>&)>(
            [=](const ublas::vector<double>& v) {
              return idx == 0 ? 1.0 : log(fabs(v(idx)))/log(2);
            });
        } else {
          cerr << "Invalid function name '" << regressor["name"].ToString() << "\n";
          exit(-1);
        }
        internalized_model.weights_[i] = regressor["weight"].ToDouble();
      }
      models_.push_back(internalized_model);
    }
  }

  double poll(const vector<long long>& values) const {
    ublas::vector<double> v = ublas::vector<double>(input_metrics_.size());

    for(unsigned i = 0; i < v.size(); ++i){
        v[i] = values[i];
    }

    ublas::vector<double> inputs;
    inputs = prod(v,principal_components_);
    ublas::vector<double> norm_inputs = element_div(inputs - means_, std_deviations_);

    int closest = 0;
    double min_distance = numeric_limits<double>::max();
    for(unsigned k = 0; k < models_.size(); ++k) {
      //get distance
      double distance = norm_2(norm_inputs - models_[k].centroid_);
      if(distance < min_distance) {
        closest = k;
        min_distance = distance;
      }
    }
    ublas::vector<double> function_vals =
      ublas::vector<double>(models_[closest].regressors_.size());
    int i = 0;
    for(auto it =
          begin(models_[closest].regressors_);
        it != end(models_[closest].regressors_); ++it, ++i) {
      function_vals(i) = (*it)(inputs);
    }
    double final_res = fabs(inner_prod(models_[closest].weights_,function_vals));
    return final_res;
  }

  string model_fname_;

  struct model_t {
    ublas::vector<double> centroid_;
    ublas::vector<double> weights_;
    vector<function<double(const ublas::vector<double>&)>> regressors_;
  };

  ublas::vector<double> means_;
  ublas::vector<double> std_deviations_;
  ublas::matrix<double> principal_components_;
  vector<string> input_metrics_;
  vector<model_t> models_;
};


void overflow(int signum, siginfo_t* info, void* context){
  (void)info;
  (void)context;
  if(signum == SIGALRM){
    is_timer_done = true;
  }
}


stats_t read_rapl(const vector<event_info_t>& eventsets, long period){
  stats_t res;
  int ncounters = 0;
  for(const auto& e : eventsets){
    ncounters += e.codes.size();
  }
  res.counters.resize(ncounters);
  int cntr_offset = 0;
  for(const auto& eventset : eventsets){
    int retval=PAPI_stop(eventset.set, &res.counters[cntr_offset]);
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
  res.time = period;
  return res;
}



vector<long long> modelPerCoreEnergies(const Model& model,
                                       const vector<stats_t>& core_stats,
                                       long long total_energy) {
  vector<long long> results(core_stats.size());
  //double poll(map<string, double> params) const {
  vector<double> model_vals;
  model_vals.reserve(core_stats.size());
  for(const auto& core_stat : core_stats){
    model_vals.push_back(model.poll(core_stat.counters));
  }
  double total = accumulate(begin(model_vals), end(model_vals), double{0});
  for(unsigned i = 0; i < results.size(); ++i){
    results[i] = model_vals[i] / total * total_energy;
  }
  return results;
}

struct ProfileValue{
  double energy;
  double time;
  double instructions;
  ProfileValue() : energy{0}, time{0}, instructions{0} {}
};

struct ProfileEntry{
  string name;
  vector<ProfileValue> values; // one per core
};


void do_profiling(int profilee_pid, const char* profilee_name,
                  const long period, const char* outfilename,
                  const Model& model) {
  /*
   * Structures holding profiling data
   */
  vector<int> children_pids;
  vector<map<void*, ProfileValue>> core_profiles;
  stats_t global_stats;
  global_stats.counters.resize(1);
  vector<vector<event_info_t>> core_counters;
  // TODO: assumption here is that hyperthreading is turned on, and that there
  // are two hardware threads per physical core. We have to make sure we only 
  // run the correct number of threads during auditing, since this assumption
  // is made.
  auto ncores = thread::hardware_concurrency() / 2;
  //auto ncores = 1u;
  core_profiles.resize(ncores);
  core_counters.reserve(ncores);

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
  int inst_counter_idx = 0;
  for(unsigned int i = 0; i < ncores; ++i){
    print("Creating per-core counters on core %d\n", i);
    auto per_core_counter_names = model.input_metrics_;
    auto inst_iter = find(begin(per_core_counter_names), end(per_core_counter_names), "PAPI_TOT_INS");  
    if(inst_iter == end(per_core_counter_names)){
      inst_counter_idx = per_core_counter_names.size();
      per_core_counter_names.push_back("PAPI_TOT_INS");
    } else {
      inst_counter_idx = distance(begin(per_core_counter_names), inst_iter);
    }
    core_counters.emplace_back(init_papi_counters(per_core_counter_names));
    auto& counters = core_counters[i];
    attach_counters_to_core(counters, i);
    start_counters(counters);
  }
  print("Creating global counters.\n");
  auto global_counters = init_papi_counters(vector<string>{kEnergyEventName});
  start_counters(global_counters);

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
  time_t sleep_secs = period / kMicroToBase;
  suseconds_t sleep_usecs = period % kMicroToBase; 
  work_time.it_value.tv_sec = sleep_secs;
  work_time.it_value.tv_usec = sleep_usecs;
  work_time.it_interval.tv_sec = sleep_secs;
  work_time.it_interval.tv_usec = sleep_usecs;
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
  using core_id_t = int;
  using assignments_left_t = int;
  map<int, pair<core_id_t, assignments_left_t>> children_cores;
  // We only want to match child PIDs to processors infrequently, since it 
  // requires a filesystem read. The assumption here is that threads are bound
  // to cores, and only get created at the beginning of the function
  // 50 is a magic number derived by running some apps a bunch of times.
  auto start_time = PAPI_get_real_usec();
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
        for(const auto& child : children_pids){
          auto core_iter = children_cores.find(child);
          if(core_iter == end(children_cores)){
            auto& elem = children_cores[child];
            elem.second = kTotalCoreAssignments;
            core_iter = children_cores.find(child);
          }
          if(core_iter->second.second > 0){
            core_iter->second.second--;
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
            core_iter->second.first = stoi(line);
          }
        }
        
        // collect stats from cores
        print("EAUDIT collating stats\n");
        // read all rapl counters
        vector<stats_t> stats(ncores);
        for(unsigned int i = 0; i < ncores; ++i){
          stats[i] = read_rapl(core_counters[i], period);
        }
        auto cur_global_stats = read_rapl(global_counters, period);
        global_stats += cur_global_stats;

        // TODO: poll model and update per-core counts appropriately
        // TODO: cur_global_stats[0] is hardwired as the total energy to be modeled
        auto per_core_energies = modelPerCoreEnergies(
            model, stats, cur_global_stats.counters[0]);

        // read all the children registers
        for(const auto& child : children_pids){
          struct user_regs_struct regs;
          ptrace(PTRACE_GETREGS, child, nullptr, &regs);
          void* rip = (void*)regs.rip;
          auto child_core = children_cores[child].first;
          // TODO This is a hack to avoid attempting to log threads that 
          // miraculously end up on the second hardware thread of a core.
          // We try to make sure things are bound appropriately, but there's
          // no guarantee, so we just ignore things that happen where we don't
          // want them.
          if((unsigned)child_core >= ncores) { continue; }
          auto& profile = core_profiles[child_core][rip];
          profile.energy += per_core_energies[child_core];
          profile.time += stats[child_core].time;
          profile.instructions += stats[child_core].counters[inst_counter_idx];
        }
        
        // resume all children
        for(const auto& child : children_pids){
          ptrace(PTRACE_CONT, child, nullptr, nullptr);
        }
        is_timer_done = false;
        // resume timer
        work_time.it_value.tv_sec = sleep_secs;
        work_time.it_value.tv_usec = sleep_usecs;
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
  auto elapsed_time = PAPI_get_real_usec() - start_time;

  /*
   * Done profiling. Convert data to output file.
   */
  print("Finalize profile.\n");
  auto profile_start_time = PAPI_get_real_usec();
  vector<ProfileEntry> profile;
  vector<ProfileValue> per_core_sums;
  ProfileValue total_sums;
  for(unsigned int i = 0; i < ncores; ++i){
    // Convert stack IDs into function names.
    ProfileValue core_sum;
    for (auto& core_profile : core_profiles[i]) {
      stringstream cmd;
      cmd << "addr2line -f -s -C -e " << profilee_name << " " << core_profile.first;
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
      string func_name, file_name;
      getline(resultstream, func_name);
      getline(resultstream, file_name);
      // NOTE: remove the trailing function annotation that says that this 
      // function has been used/called by different threads
      if(func_name.back() == ']'){
        auto last_open_bracket_pos = func_name.find_last_of('[');
        func_name.erase(last_open_bracket_pos - 1);
      }
      file_name.erase(file_name.find(':'));
      string entry_name = func_name + " at " + file_name;
      print("Reporting function %s\n", entry_name.c_str());

      auto gprofile_iter =
          find_if(begin(profile), end(profile),
                  [&](const ProfileEntry& e) { return e.name == entry_name; });
      if(gprofile_iter == end(profile)){
        ProfileEntry entry;
        entry.name = entry_name;
        entry.values.resize(ncores);
        profile.push_back(entry);
        gprofile_iter = end(profile) - 1;
      }
      auto& gprofile = gprofile_iter->values[i];
      gprofile.energy += core_profile.second.energy;
      gprofile.time += core_profile.second.time;
      gprofile.instructions += core_profile.second.instructions;
      core_sum.energy += core_profile.second.energy;
      core_sum.time += core_profile.second.time;
      core_sum.instructions += core_profile.second.instructions;
    }
    per_core_sums.push_back(core_sum);
    total_sums.energy += core_sum.energy;
    total_sums.time += core_sum.time;
    total_sums.instructions += core_sum.instructions;
  }

  /*
   * Write profile to file
   */
  ofstream myfile;
  myfile.open(outfilename);
  myfile << "----------------------------------------\n"
            "----       Per-function profile     ----\n"
            "----------------------------------------\n";
  myfile << "\t\t\t\t";
  for(unsigned i = 0; i < ncores; ++i){
    myfile << "Core " << i << "\t\t\t\t\t\t";
  }
  myfile << "\nFunc Name\tTotal Energy(j)\tTotal Time(s)\tTotal Energy Efficiency(inst/joule)\t";
  for(unsigned i = 0; i < ncores; ++i){
    myfile << "Energy(j)\tTime(s)\tEnergy Efficiency(inst/joule)\t% of Total Function Energy\t%of Total Function Time\t% difference in Energy Efficiency\t";
  }
  myfile << "\n";
  sort(begin(profile), end(profile),
       [&](const ProfileEntry& a, const ProfileEntry& b){
         auto a_sum = accumulate(begin(a.values), end(a.values), double{0},
                                 [](double acc, const ProfileValue& b){
                                   return acc + b.energy;
                                 });
         auto b_sum = accumulate(begin(b.values), end(b.values), double{0},
                                 [](double acc, const ProfileValue& b){
                                   return acc + b.energy;
                                 });
         return a_sum > b_sum;
       });
  for(auto& entry : profile){
    auto profile_sum = accumulate(begin(entry.values), end(entry.values), ProfileValue{},
                                  [](ProfileValue& acc, const ProfileValue& e){
                                    acc.energy += e.energy;
                                    acc.time += e.time;
                                    acc.instructions += e.instructions;
                                    return acc;
                                  });
    auto profile_energy_eff =
        profile_sum.energy == 0
            ? double{0}
            : profile_sum.instructions / (profile_sum.energy / kNanoToBase);
    myfile << entry.name << "\t"
           << profile_sum.energy / kNanoToBase << "\t"
           << profile_sum.time / kMicroToBase << "\t"
           << profile_energy_eff << "\t";
    for(unsigned i = 0; i < ncores; ++i){
      auto energy = entry.values[i].energy / kNanoToBase;
      auto time = entry.values[i].time / kMicroToBase;
      auto energy_eff =
          energy == 0 ? double{0} : entry.values[i].instructions / energy;
      myfile << energy << "\t"
             << time << "\t"
             << energy_eff << "\t"
             << energy / (profile_sum.energy / kNanoToBase) * 100 << "\t"
             << time / (profile_sum.time / kMicroToBase) * 100 << "\t"
             << (energy_eff == 0 ? double{0} : fabs((energy_eff - profile_energy_eff) /
                                                    ((energy_eff + profile_energy_eff) / 2)) * 100)
             << "\t";
    }
    myfile << "\n";
  }
  myfile << "\n";

  myfile << "----------------------------------------\n"
            "----       Per-thread profile       ----\n"
            "----------------------------------------\n";
  for(unsigned int i = 0; i < ncores; ++i){
    myfile << "THREAD " << i << "\n";
    myfile << "Func Name\tTotal Energy(j)\tTotal Time(s)\tEnergy Efficiency(inst/joule)\t% Core Energy\t% Core Time\t% Tot Energy\t% Tot Time\n";

    // sort the profile by decending energy for this core
    sort(begin(profile), end(profile),
         [&](const ProfileEntry& a, const ProfileEntry& b) {
           return a.values[i].energy > b.values[i].energy;
         });
    for(auto& entry : profile) {
      /* don't print functions we never execute */
      if(entry.values[i].energy == 0){ continue; }
      auto energy = entry.values[i].energy / kNanoToBase;
      auto time = entry.values[i].time / kMicroToBase;
      auto instructions = double{entry.values[i].instructions};
      myfile << entry.name << "\t" 
             << energy << "\t" 
             << time << "\t"
             << instructions / energy << "\t"
             << energy / (per_core_sums[i].energy / kNanoToBase) * 100 << "\t"
             << time / (per_core_sums[i].time / kMicroToBase) * 100 << "\t"
             << energy / (total_sums.energy / kNanoToBase) * 100 << "\t"
             << time / (total_sums.time / kMicroToBase) * 100 << "\n";
    }
    myfile << "\n";
  }
  myfile << "GLOBAL\n";
  myfile << "\t\tTotal Energy(j)\tTotal Time(s)\tEnergy Efficiency(inst/joule)\tElapsed Time(s)\n";
  myfile << "\t\t"
         << total_sums.energy / kNanoToBase << "\t"
         << total_sums.time / kMicroToBase << "\t"
         << (double)total_sums.instructions / (total_sums.energy / kNanoToBase) << "\t"
         << elapsed_time / (double)kMicroToBase << "\n";

  myfile.close();
  auto profile_elapsed = PAPI_get_real_usec() - profile_start_time;
  cout << "Profile creation time: " << profile_elapsed / (double) kMicroToBase << " seconds\n";
}


vector<string> split(istream& stream, char delimeter) {
  string line;
  vector<string> names;
  while (getline(stream, line, delimeter)) {
    names.emplace_back(line);
  }
  return names;
}


int main(int argc, char* argv[], char* envp[]) {
  /*
   * Check params
   */
  auto usage = 
    "Usage:\n"
    " eaudit [options] executable\n"
    "\n"
    "Options:\n"
    " -h                  Show this help\n"
    " -p <microseconds>   Sample period in microseconds, default 1000\n"
    " -o <filename>       File to write profile, default eaudit.tsv\n"
    " -m <filename>       Model file name, default 'default.model'\n"
    "\n";

  auto period = kDefaultSamplePeriodUsecs;
  auto outfile = kDefaultOutfile;
  auto model_fname = kDefaultModelName;
  int param;
  while((param = getopt(argc, argv, "+hp:o:m:")) != -1){
    switch(param){
      case 'p':
        period = stol(optarg);
        break;
      case 'o':
        outfile = optarg;
        break;
      case 'm':
        {
          model_fname = optarg;
        }
        break;
      case 'h':
      case '?':
        cout << usage;
        exit(0);
        break;
      default:
        cerr << "Error: bad getopt parse of parameter.\n";
        exit(-1);
        break;
    }
  }

  /*
   * Make our model
   */
  Model model{model_fname};

  /*
   * Fork a process to run the profiled application
   */
  auto profilee = fork();
  if(profilee > 0){ /* parent */
    // Let's do this.
    ptrace(PTRACE_SETOPTIONS, profilee, nullptr,
           PTRACE_O_EXITKILL | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT);
    do_profiling(profilee, argv[optind], period, outfile, model);
  } else if(profilee == 0){ /* profilee */
    // prepare for tracing
    ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
    raise(SIGSTOP);
    // start up client program
    execve(argv[optind], &argv[optind], envp);
    cerr << "Error: profilee couldn't start its program!\n";
    perror(nullptr);
    exit(-1);
  } else { /* error */
    cerr << "Error: couldn't fork audited program.\n";
    return -1;
  }
}

