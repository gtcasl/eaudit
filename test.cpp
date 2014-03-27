#include <iostream>
#include <unistd.h>

void bar(int x){
  sleep(x);
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

