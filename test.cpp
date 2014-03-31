#include <iostream>
#include <unistd.h>
#include <ctime>
#include <cstdlib>

void foo(int n){
  int* x = (int*) malloc(n * sizeof(int));
  int* y = (int*) malloc(n * sizeof(int));
  int* z = (int*) malloc(n * sizeof(int));

  srand(time(NULL));

  for(int i = 0; i < n; ++i){
    x[i] = rand();
    y[i] = rand();
  }

  for(int j = 0; j < 10; ++j){
    for(int i = 0; i < n; ++i){
      z[i] = x[i] * y[i];
    }
  }

  free(z);
  free(y);
  free(x);
}

void bar(){
  sleep(2);
}

int main(){

  std::cout << "Hello world" << std::endl;
  int x = 0;
  int y = 1;
  int z = y + x;
  //foo(1000000);
  bar();

  return 0;
}

