#ifndef EAUDIT_H
#define EAUDIT_H

#include <signal.h>

void EAUDIT_push();
void EAUDIT_pop(const char* func_name);
void EAUDIT_shutdown();

#endif // EAUDIT_H
