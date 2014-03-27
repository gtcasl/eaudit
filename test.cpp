#include <iostream>

void baz(){
  while(1);
}

void bar(){
  baz();
}

void foo(){
  bar();
}

int main(){

  std::cout << "Hello world" << std::endl;
  int x = 0;
  int y = 1;
  int z = y + x;
  foo();

  return 0;
}

