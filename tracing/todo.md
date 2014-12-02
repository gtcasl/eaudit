* Make eaudit take all the params of the function to call.
    ~~fork(), PTRACE_TRACEME, execve()~~
    set timer to interrupt traced process and examine it

* ~~Instead of setting an alarm, we can just usleep() for the appropriate time, then do all the stuff we want.~~
    ~~That way we don't have to worry about how to be perfectly interrupt-safe.~~

* Get a list of all the processes threads
    Elf64_Addr
    * Get magic addresses (statically?)
        
        
* Get each thread's IP
    struct user_regs_struct regs;
    ptrace(PTRACE_GETREGS, thread_pid, nullptr, &regs);
    print(regs.rip)
