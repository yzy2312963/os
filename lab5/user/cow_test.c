#include <stdio.h>
#include <unistd.h>

// 将 cow_var 定义为全局变量，使其位于 .data 段
volatile int cow_var = 100;

int main(void) {
    // 1. 变量现在是全局的

    cprintf("----------------------------------------\n");
    cprintf("COW Test: Initial value of cow_var = %d\n", cow_var);
    cprintf("COW Test: Address of cow_var = 0x%x\n", &cow_var);
    cprintf("----------------------------------------\n");

    int pid = fork();

    if (pid < 0) {
        cprintf("fork failed, error %d\n", pid);
        return pid;
    }

    // ------------------- 子进程逻辑 -------------------
    if (pid == 0) {
        cprintf("[Child] PID: %d\n", getpid());
        cprintf("[Child] After fork, cow_var = %d at address 0x%x\n", cow_var, &cow_var);
        
        cprintf("[Child] Reading cow_var: %d\n", cow_var);
        
        cprintf("[Child] About to modify cow_var to 200. This should trigger a page fault if COW is set up.\n");
        cow_var = 200;
        cprintf("[Child] Successfully modified cow_var to %d\n", cow_var);
        
        cprintf("[Child] Exiting now.\n");
        exit(0);
    }
    // ------------------- 父进程逻辑 -------------------
    else {
        cprintf("[Parent] PID: %d, Child PID: %d\n", getpid(), pid);
        cprintf("[Parent] Before wait, cow_var = %d at address 0x%x\n", cow_var, &cow_var);
        
        cprintf("[Parent] Waiting for child to finish...\n");
        int exit_code;
        waitpid(pid, &exit_code);
        cprintf("[Parent] Child has finished.\n");
        
        cprintf("----------------------------------------\n");
        cprintf("[Parent] Checking my cow_var value now: %d at address 0x%x\n", cow_var, &cow_var);
        cprintf("----------------------------------------\n");
        
        if (cow_var == 100) {
            cprintf("SUCCESS: Parent's variable was not changed. COW works!\n");
        } else {
            cprintf("FAILED: Parent's variable was changed to %d. COW failed!\n", cow_var);
        }
    }

    return 0;
}