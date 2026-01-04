#include <defs.h>
#include <list.h>
#include <proc.h>
#include <assert.h>
#include <default_sched.h>

/*
 * SJF_init initializes the run-queue rq with correct assignment for
 * member variables, including:
 *
 *   - run_list: should be an empty list after initialization.
 *   - proc_num: set to 0
 *   - max_time_slice: no need here, the variable would be assigned by the caller.
 */
static void
SJF_init(struct run_queue *rq)
{
    list_init(&(rq->run_list));
    rq->proc_num = 0;
    rq->lab6_run_pool = NULL;
}

/*
 * SJF_enqueue inserts the process ``proc'' into the run-queue
 * ``rq''. SJF调度器需要维护一个按估计运行时间排序的队列。
 * 使用lab6_priority字段存储估计的运行时间（较小的值表示较短的作业）。
 * 新进程插入到合适的位置以保持队列按估计时间递增排序。
 */
static void
SJF_enqueue(struct run_queue *rq, struct proc_struct *proc)
{
    assert(list_empty(&(proc->run_link)));
    
    // 如果priority为0，设置为默认值1
    if (proc->lab6_priority == 0) {
        proc->lab6_priority = 1;
    }
    
    // 找到合适的位置插入，保持按priority（估计运行时间）递增排序
    list_entry_t *le = &(rq->run_list);
    list_entry_t *next;
    while ((next = list_next(le)) != &(rq->run_list)) {
        struct proc_struct *next_proc = le2proc(next, run_link);
        // 如果当前进程的估计时间小于等于下一个进程，插入到这里
        if (proc->lab6_priority <= next_proc->lab6_priority) {
            break;
        }
        le = next;
    }
    list_add_after(le, &(proc->run_link));
    
    // SJF不使用时间片，但为了兼容框架，设置一个很大的值
    proc->time_slice = 0x7FFFFFFF;
    proc->rq = rq;
    rq->proc_num++;
}

/*
 * SJF_dequeue removes the process ``proc'' from the run-queue
 * ``rq''.
 */
static void
SJF_dequeue(struct run_queue *rq, struct proc_struct *proc)
{
    assert(!list_empty(&(proc->run_link)) && proc->rq == rq);
    list_del_init(&(proc->run_link));
    rq->proc_num--;
}

/*
 * SJF_pick_next picks the element from the front of ``run-queue'',
 * which should be the process with the shortest estimated job time.
 * 由于队列已经按估计时间排序，队首就是最短的作业。
 */
static struct proc_struct *
SJF_pick_next(struct run_queue *rq)
{
    list_entry_t *le = list_next(&(rq->run_list));
    if (le != &(rq->run_list)) {
        return le2proc(le, run_link);
    }
    return NULL;
}

/*
 * SJF_proc_tick works with the tick event of current process.
 * SJF不使用时间片，所以不需要递减time_slice，也不设置need_resched。
 * 进程会一直运行直到完成或主动让出CPU。
 */
static void
SJF_proc_tick(struct run_queue *rq, struct proc_struct *proc)
{
    // SJF不使用时间片，所以不做任何处理
    // 进程会一直运行直到完成或主动让出
}

struct sched_class sjf_sched_class = {
    .name = "SJF_scheduler",
    .init = SJF_init,
    .enqueue = SJF_enqueue,
    .dequeue = SJF_dequeue,
    .pick_next = SJF_pick_next,
    .proc_tick = SJF_proc_tick,
};

