#include <iostream>
#include <unistd.h>
#include <ctime>
#include <cstdlib>
#include <pthread.h>

using namespace std;

void* test(void* unused) {
  long a = 0, b = 0;
  cout << "Running test...\n";
  for(long i = 0; i < 500000000; ++i){
    a += i;
    b += i;
  }
  return NULL;
}

int main(){
  pthread_t thread;
  pthread_create(&thread, NULL, test, NULL);
  test(NULL);
}

