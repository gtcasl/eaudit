#include <iostream>
#include <pthread.h>
#include <sched.h>

using namespace std;

struct param{
  long nops;
  int pid;
};

void* test(void* nops) {
  long a = 0;
  long nops_l = ((param*)nops)->nops;
  int id = ((param*)nops)->pid;
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(id, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
  cout << "Running test thread " << ((param*)nops)->pid << "...\n";
  for(long i = 0; i < nops_l; ++i){
    a += i;
  }
  cout << "Done with test thread " << ((param*)nops)->pid << "\n";
  return NULL;
}

int main(){
  cout << "Starting test.cpp...\n";
  pthread_t thread;
  param p1, p2;
  p1.nops = 300000000;
  p1.pid = 1;
  p2.nops = 600000000;
  p2.pid = 2;
  pthread_create(&thread, NULL, test, &p1);
  test(&p2);
  pthread_join(thread, NULL);
  cout << "Joined\n";
  return 0;
}

