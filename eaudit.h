#ifndef EAUDIT_H
#define EAUDIT_H

void EAUDIT_push();
void EAUDIT_pop(const char* func_name);
void EAUDIT_shutdown();

/*void overflow(int eventset, void* address, long long overflow_vector, 
              void* context);*/
void overflow(int signum);
void init_papi(int* eventset);
void read_rapl();

#endif // EAUDIT_H
