#ifndef EAUDIT_H
#define EAUDIT_H

#include <signal.h>

void EAUDIT_shutdown();

void overflow(int signum, siginfo_t* info, void* context);
void init_papi(int* eventset);
void read_rapl();

#endif // EAUDIT_H
