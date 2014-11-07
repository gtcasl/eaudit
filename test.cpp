#include <iostream>
#include <unistd.h>
#include <ctime>
#include <cstdlib>

void test() {
  long a = 0, b = 0;
  for(long i = 0; i < 100000000; ++i){
    a += i;
    b += i;
  }
}

int main(){
  test();
}

