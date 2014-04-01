#include <iostream>
#include <unistd.h>

int bar(int x){
  int a = 0;
  for(long i = 0; i < 1000000000; ++i){
    a += x * i;
  }
  return a;
}

int main(){

  std::cout << "Hello world" << std::endl;
  int x = 0;
  int y = 1;
  int z = y + x;
  bar(1);
  bar(2);
  bar(3);

  return 0;
}

