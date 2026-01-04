#include <ulib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define TOTAL 5
#define MAX_TIME 3000

unsigned int acc[TOTAL];
int status[TOTAL];
int pids[TOTAL];

static void
spin_delay(void)
{
    int i;
    volatile int j;
    for (i = 0; i != 200; ++ i)
    {
        j = !j;
    }
}

/*
 * 测试程序：创建多个进程，每个进程有不同的运行时间需求
 * 用于测试不同调度算法的性能指标
 * 
 * 测试方法：
 * 1. 创建5个子进程，每个进程设置不同的估计运行时间（通过lab6_priority）
 * 2. 每个进程运行相同的时间（直到MAX_TIME）
 * 3. 观察不同调度算法下的执行顺序和完成时间
 * 4. 通过acc值（累计计算次数）来间接反映CPU分配情况
 */
int
main(void) {
    int i, time;
    int start_time = gettime_msec();
    memset(pids, 0, sizeof(pids));
    
    // 父进程设置较低的优先级（如果使用优先级调度）
    lab6_setpriority(TOTAL + 1);

    cprintf("=== Scheduling Algorithm Test ===\n");
    cprintf("Test started at %d ms\n", start_time);
    cprintf("Creating %d child processes with different estimated run times...\n", TOTAL);
    
    // 创建子进程，每个进程有不同的估计运行时间
    // 使用lab6_priority来设置估计运行时间（对于SJF）
    // 较小的priority值表示较短的作业
    for (i = 0; i < TOTAL; i ++) {
        acc[i] = 0;
        if ((pids[i] = fork()) == 0) {
            // 子进程：设置不同的优先级/估计运行时间
            // 进程0最短（priority=5），进程4最长（priority=1）
            // 注意：对于SJF，较小的priority值表示较短的作业
            lab6_setpriority(TOTAL - i);  
            acc[i] = 0;
            int child_start = gettime_msec();
            cprintf("Process %d (pid %d) started at %d ms, estimated time: %d\n", 
                    i, getpid(), child_start, TOTAL - i);
            
            while (1) {
                spin_delay();
                ++ acc[i];
                if(acc[i] % 4000 == 0) {
                    if((time = gettime_msec()) > MAX_TIME) {
                        int child_end = time;
                        cprintf("Process %d (pid %d) finished: acc=%d, start=%d, end=%d, duration=%d\n",
                                i, getpid(), acc[i], child_start, child_end, 
                                child_end - child_start);
                        exit(acc[i]);
                    }
                }
            }
        }
        if (pids[i] < 0) {
            goto failed;
        }
    }

    cprintf("main: fork ok, now waiting for pids...\n");

    // 等待所有子进程完成，记录完成顺序
    int finish_order[TOTAL];
    int finish_times[TOTAL];
    for (i = 0; i < TOTAL; i ++) {
        status[i] = 0;
        int wait_start = gettime_msec();
        waitpid(pids[i], &status[i]);
        int wait_end = gettime_msec();
        finish_order[i] = pids[i];
        finish_times[i] = wait_end;
        cprintf("main: pid %d finished (order %d), acc=%d, wait_time=%d, total_time=%d\n", 
                pids[i], i+1, status[i], wait_end - wait_start, wait_end - start_time);
    }
    
    cprintf("main: all pids finished at %d ms\n", gettime_msec());
    
    // 输出性能分析
    cprintf("\n=== Performance Analysis ===\n");
    cprintf("Finish order: ");
    for (i = 0; i < TOTAL; i++) {
        cprintf("P%d ", finish_order[i]);
    }
    cprintf("\n");
    
    cprintf("Accumulated work (acc values, higher = more CPU time):\n");
    for (i = 0; i < TOTAL; i++) {
        cprintf("  Process %d (pid %d): acc=%d\n", i, pids[i], status[i]);
    }
    
    cprintf("\nNote: For FIFO, processes should finish in creation order.\n");
    cprintf("      For SJF, shorter jobs (higher priority value) should finish earlier.\n");
    cprintf("      For RR, all processes should get similar CPU time.\n");
    
    cprintf("\n=== Test Complete ===\n");
    return 0;

failed:
    for (i = 0; i < TOTAL; i ++) {
        if (pids[i] > 0) {
            kill(pids[i]);
        }
    }
    panic("FAIL: T.T\n");
}

