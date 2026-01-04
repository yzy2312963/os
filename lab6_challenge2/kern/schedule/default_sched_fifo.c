#include <defs.h>
#include <list.h>
#include <proc.h>
#include <assert.h>
#include <default_sched.h>

/*
 * FIFO_init initializes the run-queue rq with correct assignment for
 * member variables, including:
 *
 *   - run_list: should be an empty list after initialization.
 *   - proc_num: set to 0
 *   - max_time_slice: no need here, the variable would be assigned by the caller.
 */
static void
FIFO_init(struct run_queue *rq)
{
    list_init(&(rq->run_list));
    rq->proc_num = 0;
    rq->lab6_run_pool = NULL;
}

/*
 * FIFO_enqueue inserts the process ``proc'' into the tail of run-queue
 * ``rq''. FIFO调度器使用FIFO队列，新进程插入队尾。
 * 注意：FIFO不使用时间片，所以不设置time_slice。
 */
static void
FIFO_enqueue(struct run_queue *rq, struct proc_struct *proc)
{
    assert(list_empty(&(proc->run_link)));
    list_add_before(&(rq->run_list), &(proc->run_link));
    // FIFO不使用时间片，但为了兼容框架，设置一个很大的值
    proc->time_slice = 0x7FFFFFFF;  // 几乎无限的时间片
    proc->rq = rq;
    rq->proc_num++;
}

/*
 * FIFO_dequeue removes the process ``proc'' from the run-queue
 * ``rq''.
 */
static void
FIFO_dequeue(struct run_queue *rq, struct proc_struct *proc)
{
    assert(!list_empty(&(proc->run_link)) && proc->rq == rq);
    list_del_init(&(proc->run_link));
    rq->proc_num--;
}

/*
 * FIFO_pick_next picks the element from the front of ``run-queue'',
 * and returns the corresponding process pointer.
 * FIFO总是选择队列头部的进程（最早进入队列的进程）。
 */
static struct proc_struct *
FIFO_pick_next(struct run_queue *rq)
{
    list_entry_t *le = list_next(&(rq->run_list));
    if (le != &(rq->run_list)) {
        return le2proc(le, run_link);
    }
    return NULL;
}

/*
 * FIFO_proc_tick works with the tick event of current process.
 * FIFO不使用时间片，所以不需要递减time_slice，也不设置need_resched。
 * 进程会一直运行直到主动让出CPU（yield）或阻塞。
 */
static void
FIFO_proc_tick(struct run_queue *rq, struct proc_struct *proc)
{
    // FIFO不使用时间片，所以不做任何处理
    // 进程会一直运行直到完成或主动让出
}

struct sched_class fifo_sched_class = {
    .name = "FIFO_scheduler",
    .init = FIFO_init,
    .enqueue = FIFO_enqueue,
    .dequeue = FIFO_dequeue,
    .pick_next = FIFO_pick_next,
    .proc_tick = FIFO_proc_tick,
};

