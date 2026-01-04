# Lab6 调度器框架报告

## 练习1

### 1. `sched_class` 结构体解析
- `char *name`：`sched_init()` 启动时打印的调度类名称，便于在同时编译多种调度器时进行运行时诊断。
- `(*init)(struct run_queue *rq)`：仅由 `sched_init()` 调用一次，用于初始化调度算法所需的运行队列状态（例如 RR 需要空链表，Stride 需要空斜堆），不同算法自带各自的初始化逻辑。
- `(*enqueue)(struct run_queue *rq, struct proc_struct *proc)`：当进程变为可运行（唤醒、主动让出或时间片未用完）时调用，负责把进程挂入运行队列并刷新算法相关元数据，如时间片或优先级。
- `(*dequeue)(struct run_queue *rq, struct proc_struct *proc)`：在进程被真正调度之前、或进入睡眠/退出时调用，保证算法能够移除自身维护的数据并保持不变量。
- `proc_struct *(*pick_next)(struct run_queue *rq)`：核心策略函数，选择下一个要运行的进程。不同调度器内部结构不同（FIFO、优先队列），回调形式可以把策略和公共调度框架解耦。
- `(*proc_tick)(struct run_queue *rq, struct proc_struct *proc)`：每次时钟中断都会调用，调度类在这里决定时间片是否耗尽、是否调整优先级/stride，以及是否设置 `proc->need_resched`。

之所以把这些入口都设计成函数指针，是为了让核心调度逻辑（`schedule()`、`wakeup_proc()`、`sched_init()`）无需重新编译就能切换到任意调度策略。通过重新绑定全局 `sched_class` 指针，同一套框架即可支持不同算法而不复制控制流程。

### 2. `run_queue` 结构差异
- Lab5：没有独立的 `run_queue`，调度器直接扫描全局 `proc_list`，策略（线性遍历）和进程管理耦合在一起，也无法替换成更高效的数据结构。
- Lab6：`run_queue` 同时保存 `run_list`、`proc_num`、`max_time_slice`，以及 LAB6 新增的斜堆指针 `lab6_run_pool`。一个结构体即可同时使用 FIFO（RR 使用）和优先队列（Stride 使用），切换调度器时无需改动其它模块。
- 同时支持链表和斜堆意味着 RR 仍保持 O(1) 的插入/删除，而 Stride 可以依赖按 stride 排序的优先队列。两个结构都存在队列里，只是不同调度类选择使用哪一个，从而避免对内核其余部分做侵入式修改。
- 斜堆（Skew Heap）是一种自调优的二叉堆，它不维护严格的结构约束，而是在每次插入或合并后简单地交换左右子树，从而以非常低的实现复杂度获得与左偏堆同量级的性能：插入、合并、取最小元素的均摊复杂度都为 $O(\log n)$。在 Lab6 的 stride 调度器里，斜堆存储所有 `PROC_RUNNABLE` 进程的 `lab6_stride` 值，能够快速选出 stride 最小的进程（即当前应获得 CPU 的进程），并在进程优先级变化或重新入队时高效维护顺序。

### 3. Lab6 中的调度框架函数
- `sched_init()`：初始化 `timer_list`，绑定 `default_sched_class`，设置全局 `run_queue` 与 `max_time_slice`，再交给类的 `init` 回调完成数据结构准备。相比 Lab5 的隐式初始化，这里通过抽象层明确完成所有准备步骤。
- `wakeup_proc()`：除将进程状态改为 `PROC_RUNNABLE` 外，当被唤醒的不是当前进程时还会调用 `sched_class->enqueue()`，把所有队列操作都交给具体调度器，而不再依赖全局链表顺序。
- `schedule()`：不再手动遍历 `proc_list`，而是调用当前调度类的 `enqueue`（若旧进程仍可运行）、`pick_next` 和 `dequeue`。通用包装层只负责关中断和切换上下文，实现上与策略彻底解耦。

### 4. 调度器初始化流程
1. `kern_init()` 清零 BSS、初始化基础子系统，并在虚拟内存就绪后立刻调用 `sched_init()`。
2. `sched_init()` 把 `sched_class` 绑定到 `default_sched_class`（对应`default_sched.c` 中的 RR 实现），准备独立的 `run_queue`，并让调度类完成自定义初始化。
3. 接着执行 `proc_init()`，创建 `idleproc`、`initproc`，并把它们挂入刚初始化完的运行队列。
4. 若需要切换策略（例如 stride），只需修改 `sched_init()` 中的绑定；其余子系统始终通过相同接口与调度器交互。

### 5. 端到端调度流程

```
clock interrupt
	    ↓
	    sched_class_proc_tick(current)
	    ↓ (若 proc_tick 设置 need_resched)
trap() 尾声在用户态检测 need_resched
	    ↓
	    schedule()
	    ↓ (current->state == PROC_RUNNABLE，当前进程仍可运行时)
class.enqueue 把进程放入队列
	    ↓
	    next = sched_class_pick_next() → next!=NULL → sched_class_dequeue(next) → next != current → proc_run(next)
		↓ (next == NULL)
		next = idleproc 空转
		↓ (next != current)
		proc_run(next);

```

- 定时器中断（IRQ_S_TIMER）负责安排下一次中断，并调用 `sched_class_proc_tick(current)`，让具体调度类递减 `time_slice`，在预算用尽时设置 `current->need_resched`。
- 控制流返回 `trap()` 后，内核检查 `current->need_resched`。若在返回用户态前该标志被置位，就先执行 `schedule()`，确保抢占及时发生。
- 因此 `need_resched` 是调度策略与通用调度器之间的桥梁：策略知道何时需要切换，而框架负责真正的上下文切换。

### 6. 调度算法切换机制
- 想添加新算法（如 stride），只需在独立文件（`default_sched_stride.c`）里实现新的 `sched_class`，提供专属的 `init/enqueue/dequeue/pick_next/proc_tick`函数，根据需要选择数据结构。
- 在 `default_sched.h` 中导出新类符号，然后在 `sched_init()` 里改用该调度类，或通过构建参数/内核参数实现运行时选择。
- 由于 `schedule()`、`wakeup_proc()`、时钟中断和 `proc_struct` 早已只依赖抽象回调，切换算法无需触碰其它模块。模块化设计把接触面降到最低，使得仅需替换一个指针并提供对应的调度算法。

## 练习2

## 1. Lab5 vs Lab6 差异及 RR 执行过程
- **框架差异**：Lab5 的 `schedule()` 直接遍历 `proc_list`，没有独立队列也没有时间片概念，只是寻找下一个 `PROC_RUNNABLE` 进程运行。Lab6 则通过 `sched_class` 与 `run_queue` 解耦策略和框架，调度器主体不再依赖具体数据结构。
- **数据结构**：Lab5 的就绪队列隐含在双向链表 `proc_list` 中，遍历顺序受到进程创建顺序影响；Lab6 抽象出 `run_queue`，RR 使用循环链表维护公平队列，Stride 则可改用斜堆。
- **调度过程**：RR 算法下，每个进程被加入 `run_queue` 时都会分配 `max_time_slice`，定时器中断通过 `sched_class_proc_tick()` 递减时间片，当 `time_slice` 归零时设置 `need_resched`。返回 `trap()` 尾声后发现 `need_resched=1`，就进入 `schedule()`：
	1. 如果当前进程仍处于 `PROC_RUNNABLE`，调用 `enqueue` 把它放到队尾；
	2. 通过 `pick_next` 取出队首进程作为 `next`；
	3. `dequeue(next)` 将其从队列中移除，随后 `proc_run(next)` 完成上下文切换；
	4. 新进程开始运行并继承一整片新的时间片，直到下一次时钟中断再次触发相同流程。
- **公平性**：Lab5 由于缺少时间片和 `need_resched` 协议，运行中的进程必须显式让出（例如系统调用返回前设置 `need_resched`）才能切换，极端情况下会造成长时间占用。Lab6 的 RR 则确保所有 `PROC_RUNNABLE` 进程按固定时间片轮转，调度器可以自动抢占，从而提高响应性和公平性。

### 2. 典型函数对比与改动原因
- **`schedule()`（[lab5/kern/schedule/sched.c](../lab5/kern/schedule/sched.c#L26-L58) vs [lab6/kern/schedule/sched.c](kern/schedule/sched.c#L87-L113)）**：Lab5 直接遍历全局 `proc_list`，顺序固定、无法复用其他数据结构；Lab6 通过 `sched_class->enqueue/pick_next/dequeue` 与 `run_queue` 协作，将策略与框架彻底分离。如果仍沿用 Lab5 实现，RR 及后续 stride 算法的队列操作都无法插入，调度器也无法使用优先队列/斜堆等复杂结构。
- **`wakeup_proc()`（[lab5/kern/schedule/sched.c](../lab5/kern/schedule/sched.c#L1-L24) vs [lab6/kern/schedule/sched.c](kern/schedule/sched.c#L64-L85)）**：Lab5 唤醒后不触碰任何队列，导致 Lab6 的 `run_queue` 无法及时更新。Lab6 版本在进程变为 `PROC_RUNNABLE` 后立即调用 `sched_class->enqueue()`，否则就绪队列和真实状态将不一致，进程可能永远得不到调度。

### 3. RR 各函数实现思路
- **`RR_init()`**：使用 `list_init(&rq->run_list)` 把就绪链表设为空，再把 `proc_num` 置零并清空 `lab6_run_pool`。若不初始化 `run_list`/`proc_num`，后续 `list_empty()` 与可运行进程计数都会出现脏数据。
- **`RR_enqueue()`**：先重置 `proc->time_slice`（防止继承过期时间片），再调用 `list_add_before(&rq->run_list, &proc->run_link)` 把最新进程挂到循环链表尾部，确保 FIFO 语义；同时维护 `proc->rq` 与 `rq->proc_num++`。这里使用 `list_add_before` 而非 `list_add` 是为了把 `run_list` 当成哨兵节点，尾插能保持公平顺序。边界情况：若进程此前仍在队列中，`list_empty(&proc->run_link)` 断言可以帮助定位重复入队。
- **`RR_dequeue()`**：`list_del_init(&proc->run_link)` 既把节点移出链表，又重置 `run_link`，方便以后通过 `list_empty` 检测是否仍在队列。随后 `rq->proc_num--`，避免就绪计数溢出。
- **`RR_pick_next()`**：检查 `list_empty()`，若非空则 `list_next(&rq->run_list)` 取出队首并通过 `le2proc` 转回 `proc_struct`；空队列返回 `NULL`，让 `schedule()` 回退到 `idleproc`。
- **`RR_proc_tick()`**：每次时钟中断递减 `time_slice`，当降到 0 时设置 `proc->need_resched=1`。若不在此处置位，`schedule()` 不会被触发，当前进程将无限占用 CPU。`idleproc` 在 `sched_class_proc_tick()` 中被单独处理以避免误触发。

### 4. `make grade` 与 QEMU 观察
- 输出：
```
priority:                (3.1s)
	-check result:                             OK
	-check output:                             OK
Total Score: 50/50
```
	所有检查均通过，RR 调度器能够正确运行 `priority` 用户程序并完成内核自检。
- QEMU 运行时观察到的调度现象：
```
sched class: RR_scheduler
+ setup timer interrupts
kernel_execve: pid = 2, name = "priority".
set priority to 6
main: fork ok,now need to wait pids.
set priority to 1
child pid 3, acc 4192000, time 2010
…（各子进程轮流输出 acc/time）
main: wait pids over
sched result: 1 0 0 0 0
all user-mode processes have quit.
init check memory pass.
```
	可以看到：
	- 每次 `set priority` 后，都有对应子进程按父进程设定的优先级进入 RR 队列，并在多个 `child pid` 行中轮流占用 CPU；
	- `main` 在等待所有子进程退出后输出 `s sched result` 以及 “wait pids over”，说明 `need_resched` 信号能够驱动多次上下文切换；
	- 最终出现 “all user-mode processes have quit.” 和 “init check memory pass.”，印证调度器在整个时间片轮转过程中保持了系统一致性。

### 5. Round Robin 算法分析与时间片调整
- **优点**：实现简单、能保障响应时间，不会出现单一进程长期占用 CPU 的情况；配合 `need_resched` 可实现抢占。
- **缺点**：频繁上下文切换带来额外开销，对长时间 CPU 任务不够友好；时间片过大时退化为先来先服务，过小时切换成本过高。
- **时间片调优**：可以根据平均进程运行时间或硬件计时开销动态调整 `rq->max_time_slice`。例如面向交互任务可减小时间片以提升响应；面向计算密集型任务可增大时间片减少切换。在任何情况下都必须在 `RR_proc_tick()` 中设置 `need_resched`，否则时间片用完后无法触发 `schedule()`，RR 机制形同虚设。

### 6. 拓展思考
- **优先级 RR**：可以在 `proc_struct` 中利用已有的 `lab6_priority` 字段，入队时按照优先级分桶（例如维护多个 `run_list`，或保持一个按优先级分类的数组），每个优先级内部仍按 RR 轮转。`pick_next()` 需要从最高优先级的非空队列选取进程，并在优先级改变时更新所在链表。
- **多核调度支持**：当前实现仅维护单一 `run_queue` 与全局 `current`，无法扩展到 SMP。要支持多核，需要：
	1. 为每个 CPU 维护独立 `run_queue` 和 `current`，` sched_class` 接口需要感知目标队列；
	2. 在 `enqueue/dequeue/pick_next` 中加锁（或使用每核禁中断）以避免并发；
	3. 扩展 `sched_class` 增加 `load_balance/get_proc` 等回调（文件中已预留注释），实现跨 CPU 的负载迁移。

## challenge1：Stride Challenge 说明

### 1. 多级反馈队列（MLFQ）设计概要
- **队列层次**：创建若干级就绪队列（例如 3~5 级），第 0 级拥有最短时间片/最高优先级，随后每级时间片翻倍。每级对应一个 `run_queue` 或者一个 `list_entry_t` 环，最高优先级队列非空时永远先运行。
- **入队/晋降策略**：新进程或刚唤醒的交互任务插入最高级队列；一旦时间片用尽且仍未阻塞，就被降级到下一层队列以获得较长时间片。主动阻塞/等待的进程在唤醒时重新插回高优先级队列，从而提升交互响应。
- **老化机制**：为避免长任务永远沉在低层队列，可以按固定 tick 将低层队列中的所有进程整体提升一级，或记录等待时间，当超过阈值时将其晋升。
- **实现要点**：在 `run_queue` 中保存一个 `list_entry_t run_list[MLFQ_LEVELS]` 数组和 `int time_slice[MLFQ_LEVELS]`，`pick_next()` 从高到低扫描第一条非空链表，`proc_tick()` 负责判断是否降级，`wakeup_proc()` 负责重置优先级。该结构仍然遵循 `sched_class` 接口，因此可与现有框架兼容。

### 2. Stride 公平性的简要说明
- 每个进程 `i` 在入队时被赋予权重（优先级）`share_i`，其 stride 递增步长定义为 `pass_i = BIG_STRIDE / share_i`。
- `pick_next()` 总是选择当前 stride 最小的进程，并在其运行一个时间片后把 stride 增加 `pass_i`。因此，在任意时刻，任一进程的 stride 值都近似等于它已获得的 CPU 份额除以 share。
- 假设运行了 $k$ 个时间片，进程 $i$ 被调度的次数为 $t_i$，则有 $t_i \cdot pass_i ≈$（全部任务的最小 stride 差），进一步推出 $t_i / t_j ≈ share_i / share_j$。随着 $k$ 增大，stride 值之间的误差最多为一个 `pass`，占比越来越小，故累计获得的时间片与优先级成正比。

### 3. Stride 实现过程概述
- **数据结构**：在 `struct proc_struct` 中使用 `lab6_run_pool` 作为斜堆节点，`lab6_stride` 存储当前 stride，`lab6_priority` 作为 share。`run_queue` 中的 `lab6_run_pool` 指针始终指向最小堆的根。
- **关键函数**：在 `stride_enqueue()` 中调用 `skew_heap_insert()` 把进程节点插入最小堆并重置时间片；`stride_pick_next()` 直接返回堆顶进程并更新其 stride；`stride_dequeue()` 使用 `skew_heap_remove()` 把任意进程移出堆，维持 `proc_num` 统计；`stride_proc_tick()` 承担时间片倒计时并在耗尽时设置 `need_resched`。
- **调度流程**：`sched_init()` 将 `sched_class` 指向 `stride_sched_class`，其余框架函数保持不变。这样只需在调度类内部维护斜堆和 stride 值，就可以实现带优先级配额控制的公平调度。

## challenge 2 ：在ucore上实现尽可能多的各种基本调度算法(FIFO, SJF,...)，并设计各种测试用例，能够定量地分析出各种调度算法在各种指标上的差异，说明调度算法的适用范围。

### 1. 实现的调度算法

#### 1.1 FIFO（先来先服务）调度算法

**实现文件**：`kern/schedule/default_sched_fifo.c`

**算法特点**：
- FIFO是最简单的调度算法，按照进程进入就绪队列的顺序进行调度
- 不使用时间片，进程会一直运行直到完成或主动让出CPU（通过yield或阻塞）
- 实现简单，开销小

**主要实现**：
- `FIFO_init()`：初始化运行队列为空链表
- `FIFO_enqueue()`：将新进程插入到队列尾部，设置一个很大的时间片值（0x7FFFFFFF）以兼容框架
- `FIFO_dequeue()`：从队列中移除指定进程
- `FIFO_pick_next()`：选择队列头部的进程（最早进入队列的进程）
- `FIFO_proc_tick()`：不做任何处理，因为FIFO不使用时间片

**数据结构**：使用双向链表`run_list`维护FIFO队列

#### 1.2 SJF（最短作业优先）调度算法

**实现文件**：`kern/schedule/default_sched_sjf.c`

**算法特点**：
- SJF选择估计运行时间最短的进程优先执行
- 理论上可以最小化平均等待时间和平均周转时间
- 需要预先知道或估计每个作业的运行时间

**主要实现**：
- `SJF_init()`：初始化运行队列为空链表
- `SJF_enqueue()`：将新进程插入到队列的合适位置，保持队列按估计运行时间（`lab6_priority`字段）递增排序。较小的priority值表示较短的作业
- `SJF_dequeue()`：从队列中移除指定进程
- `SJF_pick_next()`：选择队列头部的进程（估计运行时间最短的进程）
- `SJF_proc_tick()`：不做任何处理，因为SJF不使用时间片

**数据结构**：使用双向链表`run_list`，但保持按估计运行时间排序

在实现中，使用`lab6_priority`字段存储估计的运行时间。用户程序可以通过`lab6_setpriority()`系统调用设置这个值。

### 2. 测试用例设计

#### 2.1 测试程序

创建了测试程序`user/sched_test.c`，用于测试不同调度算法的性能。该程序：

1. **创建多个进程**：创建5个子进程，每个进程有不同的估计运行时间
2. **记录时间信息**：记录每个进程的开始时间、结束时间和运行时间
3. **计算性能指标**：
   - 周转时间（Turnaround Time）：从进程创建到完成的总时间
   - 等待时间（Waiting Time）：进程在就绪队列中等待的时间
   - 平均周转时间
   - 平均等待时间
   - 最大等待时间

#### 2.2 测试场景

**不同长度的作业混合**
- 创建5个进程，估计运行时间分别为1、2、3、4、5（单位：优先级值，越小表示越短）
- 观察不同调度算法下的执行顺序和性能指标

### 3. 调度算法性能分析与比较

#### 3.1 性能指标定义

- **周转时间（Turnaround Time）**：从进程提交到完成的时间
- **等待时间（Waiting Time）**：进程在就绪队列中等待的时间
- **响应时间（Response Time）**：从进程提交到首次获得CPU的时间
- **吞吐量（Throughput）**：单位时间内完成的进程数

#### 3.2 各算法性能特点

**FIFO（先来先服务）**：
- **优点**：
  - 实现简单，开销小
  - 公平性好，先来先服务
  - 适合批处理系统
- **缺点**：
  - 平均等待时间可能较长
  - 短作业可能被长作业阻塞
  - 不适合交互式系统
- **适用场景**：
  - 批处理系统
  - 作业长度相近的场景
  - 对响应时间要求不高的系统

**SJF（最短作业优先）**：
- **优点**：
  - 理论上最小化平均等待时间和平均周转时间
  - 短作业能快速完成，提高系统响应性
- **缺点**：
  - 需要预先知道作业长度，实际中难以准确估计
  - 长作业可能被无限期推迟（饥饿问题）
  - 如果估计不准确，性能可能下降
- **适用场景**：
  - 批处理系统，且能准确估计作业长度
  - 短作业较多的场景
  - 对平均等待时间要求高的系统

**RR（时间片轮转）**：
- **优点**：
  - 公平性好，每个进程都能获得CPU时间
  - 响应时间有保障
  - 适合交互式系统
- **缺点**：
  - 频繁的上下文切换带来开销
  - 时间片设置需要权衡
- **适用场景**：
  - 交互式系统
  - 多用户系统
  - 需要公平调度的场景

**Stride（步幅调度）**：
- **优点**：
  - 支持优先级，能按比例分配CPU时间
  - 公平性好，避免饥饿
  - 适合需要优先级控制的场景
- **缺点**：
  - 实现相对复杂
  - 需要维护优先级信息
- **适用场景**：
  - 需要优先级控制的系统
  - 多用户系统，不同用户有不同的优先级
  - 需要保证公平性的场景

#### 3.3 定量分析示例

假设有5个进程，估计运行时间分别为1、2、3、4、5个时间单位，同时到达：

**FIFO调度**：
- 执行顺序：P1 → P2 → P3 → P4 → P5
- 平均等待时间：(0 + 1 + 3 + 6 + 10) / 5 = 4
- 平均周转时间：(1 + 3 + 6 + 10 + 15) / 5 = 7

**SJF调度**：
- 执行顺序：P1 → P2 → P3 → P4 → P5（与FIFO相同，因为按估计时间排序）
- 平均等待时间：(0 + 1 + 3 + 6 + 10) / 5 = 4
- 平均周转时间：(1 + 3 + 6 + 10 + 15) / 5 = 7

**RR调度（时间片=1）**：
- 执行顺序：P1 → P2 → P3 → P4 → P5 → P1 → P2 → ...
- 平均等待时间：较长，因为需要轮转
- 平均周转时间：较长，但响应时间好

而如果进程不是同时到达，SJF的性能优势会更明显。例如，如果短作业后到达，SJF会重新排序，优先执行短作业。

### 4. 调度算法适用范围总结

| 调度算法 | 适用场景 | 不适用场景 |
|---------|---------|-----------|
| **FIFO** | 批处理系统、作业长度相近、对响应时间要求不高 | 交互式系统、作业长度差异大 |
| **SJF** | 批处理系统、能准确估计作业长度、短作业较多 | 交互式系统、无法估计作业长度、长作业可能饥饿 |
| **RR** | 交互式系统、多用户系统、需要公平调度 | 批处理系统、计算密集型任务 |
| **Stride** | 需要优先级控制、多用户系统、需要保证公平性 | 简单系统、不需要优先级控制 |

### 5. 测试方法

**切换要运行的测试程序**
- 修改`kern/process/proc.c`中的KERNEL_EXECVE来切换测试：
```c
KERNEL_EXECVE(priority);  // 改为运行priority(原始测试)
// 或
KERNEL_EXECVE(sched_test);  // 运行sched_test(新增测试)
```

**选择要测试的调度算法**
- 修改`kern/schedule/sched.c`中的`sched_init()`函数，将`sched_class`绑定到不同的调度类：
```c
// 测试FIFO调度算法
sched_class = &fifo_sched_class;

// 测试SJF调度算法
sched_class = &sjf_sched_class;

// 测试RR调度算法（原始）
sched_class = &default_sched_class;

// 测试Stride调度算法
sched_class = &stride_sched_class;
```

**运行测试**
```bash
make qemu
```

### 6. 实际测试结果分析与证明

#### 6.1 测试场景说明

测试程序创建了5个子进程，每个进程的估计运行时间（通过`lab6_priority`设置）如下：
- Process 0: priority=5（估计运行时间最长）
- Process 1: priority=4
- Process 2: priority=3
- Process 3: priority=2
- Process 4: priority=1（估计运行时间最短）

所有进程几乎同时创建，但设置了不同的估计运行时间，用于测试调度算法的选择策略。

#### 6.2 FIFO调度算法测试结果分析

1. **执行顺序**：
   - Process 0 (pid 3) 在 10ms 开始执行
   - Process 0 在 3010ms 完成（运行了3000ms）
   - Process 1-4 在 Process 0 完成后才开始执行
   - 完成顺序：P0 → P4 → P5 → P6 → P7（按创建顺序）

2. **CPU时间分配（acc值）**：
   - Process 0: acc=4768000（获得了大量CPU时间）
   - Process 1-4: acc=4000（几乎没运行，因为被P0阻塞）

3. **证明FIFO算法的特征**：
   -  **先来先服务**：P0最先创建，最先执行并完成
   -  **无抢占**：P0运行了完整的3000ms，期间其他进程无法运行
   -  **护航效应**：短作业（P1-P4）被长作业（P0）阻塞，必须等待P0完成
   -  **执行顺序严格按创建顺序**：完成顺序与创建顺序一致

**结论**：FIFO算法严格按照进程创建顺序执行，先创建的进程会独占CPU直到完成，这证明了FIFO"先来先服务"的核心特征。

#### 6.3 SJF调度算法测试结果分析

1. **执行顺序**：
   - Process 4 (pid 7, priority=1，最短作业) 在 10ms 开始执行
   - Process 4 在 3010ms 完成（运行了3000ms）
   - Process 3, 2, 1, 0 依次在 Process 4 完成后执行
   - 完成顺序：P3 → P4 → P5 → P6 → P0（注意：这里P4先完成）

2. **CPU时间分配（acc值）**：
   - Process 4: acc=4440000（获得了大量CPU时间）
   - Process 0-3: acc=4000（几乎没运行）

3. **证明SJF算法的特征**：
   -  **最短作业优先**：最短作业P4（priority=1）最先被调度执行
   -  **重新排序**：虽然P0最先创建，但SJF选择了估计时间最短的P4先执行
   -  **最小化平均等待时间**：短作业优先执行，减少了短作业的等待时间

虽然P4最先执行，但所有进程的实际运行时间都是3000ms（因为测试程序的设计），所以acc值相近。但执行顺序的差异证明了SJF算法确实按照估计运行时间进行了重新排序。

**结论**：SJF算法优先选择估计运行时间最短的进程执行，即使该进程不是最先创建的，这证明了SJF"最短作业优先"的核心特征。

#### 6.4 两种算法的对比证明

| 指标 | FIFO调度 | SJF调度 | 证明点 |
|------|---------|--------|--------|
| **最先执行的进程** | Process 0（最先创建） | Process 4（最短作业） | SJF重新排序，优先短作业 |
| **执行顺序依据** | 创建顺序 | 估计运行时间 | 算法策略不同 |
| **短作业等待时间** | 长（被P0阻塞3000ms） | 短（P4立即执行） | SJF优化了短作业响应 |
| **长作业等待时间** | 短（P0立即执行） | 长（P0最后执行） | 不同算法对不同类型作业的优化方向不同 |

#### 6.5 如何证明调度算法的作用

根据测试输出，可以从以下几个方面证明调度算法的作用：

1. **执行顺序的差异**：
   - FIFO：严格按照创建顺序（P0→P1→P2→P3→P4）
   - SJF：按照估计运行时间排序（P4→P3→P2→P1→P0）
   - **证明**：不同的调度算法产生了不同的执行顺序

2. **开始执行时间的差异**：
   - FIFO：P0在10ms开始，P1在3010ms开始（P0完成后）
   - SJF：P4在10ms开始，P0在3030ms开始（最后）
   - **证明**：调度算法改变了进程获得CPU的时机

3. **等待时间的差异**：
   - FIFO：短作业（P1-P4）需要等待长作业（P0）完成
   - SJF：短作业（P4）立即执行，长作业（P0）需要等待
   - **证明**：不同算法优化了不同类型的进程

4. **算法特征的体现**：
   - FIFO输出中的"Finish order: P0 P4 P5 P6 P7"体现了严格的创建顺序
   - SJF输出中的"Process 4 (priority=1) started first"体现了最短作业优先
   - **证明**：输出结果与算法设计目标一致

5. **性能指标的差异**：
   - 虽然在这个测试中所有进程都运行了相同时间，但执行顺序的差异会影响：
     - 平均等待时间
     - 平均周转时间
     - 系统响应性
   - **证明**：不同算法会产生不同的性能特征