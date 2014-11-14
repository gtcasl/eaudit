#include <iostream>
#include <pthread.h>

using namespace std;

struct param{
  long nops;
  int pid;
};

void* test(void* nops) {
  long a = 0, b = 0;
  long nops_l = ((param*)nops)->nops;
  cout << "Running test thread " << ((param*)nops)->pid << "...\n";
  for(long i = 0; i < nops_l; ++i){
    a += i;
    b += i;
  }
  cout << "Done with test thread " << ((param*)nops)->pid << "\n";
  return NULL;
}

int main(){
  cout << "Starting test.cpp...\n";
  pthread_t thread;
  param p1, p2;
  p1.nops = 500000000;
  p1.pid = 1;
  p2.nops = 600000000;
  p2.pid = 2;
  pthread_create(&thread, NULL, test, &p1);
  test(&p2);
  pthread_join(thread, NULL);
  cout << "Joined\n";
  return 0;
}

