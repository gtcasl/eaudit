#include <iostream>
#include <unistd.h>
#include <ctime>
#include <cstdlib>

int bar(int n){
  long a = 0;
  for(int i = 0; i < 1000000000; ++i){
    a += n * i;
  }
  return a;
}

int main(){

  std::cout << "Hello world" << std::endl;
  bar(1);
  //bar(2);
  //bar(3);

  return 0;
}

