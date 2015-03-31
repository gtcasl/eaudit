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
const vector<string> kDefaultPerCoreEventnames = {
  "PAPI_TOT_INS",
  "PAPI_TOT_CYC",
};
const vector<string> kDefaultGlobalEventnames = {
  "rapl:::PACKAGE_ENERGY:PACKAGE0",
  "rapl:::PP0_ENERGY:PACKAGE0",
};
const char* kDefaultModelName = "default.model";


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

  double poll(const vector<string>& names, const vector<long long>& values) const {
    ublas::vector<double> v = ublas::vector<double>(input_metrics_.size());

    vector<string> missing;

    int i = 0;
    for(const auto& input_metric : input_metrics_){
      auto param = find(begin(names), end(names), input_metric);
      if(param != end(names)){
        v(i) = values[distance(begin(names), param)];
      } else {
        missing.push_back(input_metric);
      }
      ++i;
    }

    if(missing.size() > 0) {
      stringstream ss;
      cerr << "eigermodel: missing input parameters to model \"" << model_fname_ <<
         "\":\n";
      for(const auto& elem : missing){
        cerr << "-->\t" << elem << "\n";
      }
      exit(-1);
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
    i = 0;
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



vector<long long> modelPerCoreEnergies(
    const Model& model, const vector<string>& per_core_event_names,
    const vector<stats_t>& core_stats, long long total_energy) {
  vector<long long> results(core_stats.size());
  //double poll(map<string, double> params) const {
  vector<double> model_vals;
  model_vals.reserve(core_stats.size());
  for(const auto& core_stat : core_stats){
    model_vals.push_back(model.poll(per_core_event_names, core_stat.counters));
  }
  double total = accumulate(begin(model_vals), end(model_vals), double{0});
  for(unsigned i = 0; i < results.size(); ++i){
    results[i] = model_vals[i] / total * total_energy;
  }
  return results;
}


void do_profiling(int profilee_pid, const char* profilee_name,
                  const long period, const char* outfilename,
                  const vector<string>& per_core_event_names,
                  const vector<string>& global_event_names,
                  const Model& model) {
  /*
   * Structures holding profiling data
   */
  vector<int> children_pids;
  vector<map<void*, result_stats_t>> core_stats;
  stats_t global_stats;
  global_stats.counters.resize(global_event_names.size());
  vector<vector<event_info_t>> core_counters;
  auto ncores = thread::hardware_concurrency();
  //auto ncores = 1u;
  core_stats.resize(ncores);
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
  for(unsigned int i = 0; i < ncores; ++i){
    print("Creating per-core counters on core %d\n", i);
    core_counters.emplace_back(init_papi_counters(per_core_event_names));
    auto& counters = core_counters[i];
    attach_counters_to_core(counters, i);
    start_counters(counters);
  }
  print("Creating global counters.\n");
  auto global_counters = init_papi_counters(global_event_names);
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
            model, per_core_event_names, stats, cur_global_stats.counters[0]);

        // read all the children registers
        for(const auto& child : children_pids){
          struct user_regs_struct regs;
          ptrace(PTRACE_GETREGS, child, nullptr, &regs);
          void* rip = (void*)regs.rip;
          auto child_core = children_cores[child];
          auto& results = core_stats[child_core][rip];
          results.per_core_stats += stats[child_core];
          results.estimated_energy += per_core_energies[child_core];
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

  /*
   * Done profiling. Convert data to output file.
   */
  print("Finalize profile.\n");
  vector<vector<pair<string, result_stats_t>>> core_profiles;
  for(unsigned int i = 0; i < ncores; ++i){
    vector<pair<string, result_stats_t> > stats;
    // Convert stack IDs into function names.
    for (auto& func : core_stats[i]) {
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
          [&](const pair<string, result_stats_t>& obj) { return obj.first == line; });
      if (stat == end(stats)) {
        stats.emplace_back(line, func.second);
      } else {
        stat->second += func.second;
      }
    }

    stable_sort(stats.begin(), stats.end(), [](const pair<string, result_stats_t>& a,
                                               const pair<string, result_stats_t>& b) {
      return a.second.estimated_energy > b.second.estimated_energy;
    });
    core_profiles.push_back(stats);
  }

  /*
   * Write profile to file
   */
  ofstream myfile;
  myfile.open(outfilename);
  for(unsigned int i = 0; i < ncores; ++i){
    myfile << "THREAD " << i << "\n";
    myfile << "Func Name"
           << "\t"
           << "Energy (j)"
           << "\t"
           << "Time(s)";
    for(const auto& eventset : core_counters[i]){
      for(const auto& name : eventset.names){
        myfile << "\t" << name;
      }
    }
    myfile << endl;

    for (auto& func : core_profiles[i]) {
      myfile << func.first << "\t" << func.second.estimated_energy / (double)kNanoToBase << "\t" << func.second.per_core_stats.time / (double)kMicroToBase;
      for(const auto& counter : func.second.per_core_stats.counters){
        myfile << "\t" << counter;
      }
      myfile << endl;
    }
    myfile << endl;
  }
  myfile << "GLOBAL\n";
  myfile << "Time(s)";
  for(const auto& eventset : global_counters){
    for(const auto& name : eventset.names){
      myfile << "\t" << name << " (j)";
    }
  }
  myfile << endl;
  myfile << global_stats.time / (double)kMicroToBase;
  for(const auto& counter : global_stats.counters){
    // TODO: assume that one tick of the counter is one nano-unit (ie joules)
    myfile << "\t" << counter / (double)kNanoToBase;
  }
  myfile << endl;

  myfile.close();
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
    " -c <names>          Use comma-separated list <names> for per-core counters\n"
    " -g <names>          Use comma-separated list <names> for global counters\n"
    " -C <filename>       Read line-separated list of per-core counter names from file\n"
    " -G <filename>       Read line-separated list of global counter names from file\n"
    " -m <filename>       Model file name, default 'default.model'\n"
    "\n";

  auto period = kDefaultSamplePeriodUsecs;
  auto outfile = kDefaultOutfile;
  auto per_core_event_names = kDefaultPerCoreEventnames;
  auto global_event_names = kDefaultGlobalEventnames;
  auto model_fname = kDefaultModelName;
  int param;
  while((param = getopt(argc, argv, "+hp:o:c:g:C:G:m:")) != -1){
    switch(param){
      case 'p':
        period = stol(optarg);
        break;
      case 'o':
        outfile = optarg;
        break;
      case 'c':
        {
          istringstream stream{optarg};
          per_core_event_names = split(stream, ',');
        }
        break;
      case 'C':
        {
          ifstream stream{optarg};
          per_core_event_names = split(stream, '\n');
        }
        break;
      case 'g':
        {
          istringstream stream{optarg};
          global_event_names = split(stream, ',');
        }
        break;
      case 'G':
        {
          ifstream stream{optarg};
          global_event_names = split(stream, '\n');
        }
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
    do_profiling(profilee, argv[optind], period, outfile, per_core_event_names,
                 global_event_names, model);
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

