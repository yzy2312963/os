# 练习1：理解first-fit 连续物理内存分配算法


## 1. 数据结构，函数分析：

### 核心数据结构
- free_area_t：包含
  - free_list：双向循环链表，链入每个空闲块的首页（按地址升序）。
  - nr_free：当前空闲页总数。
- struct Page：每个物理页对应的元信息结构：
  - ref：记录该页框被引用的次数，为0时表示页框可被安全释放。
  - property：若为空闲块首页，记录该块页数；否则为0。
  - flags：通过 PG_property 标志位标识首页。
  - page_link：链表节点，用于把首页加入 free_list。

---

### default_init()
- 作用：初始化 PMM 管理器的内部状态。
- 关键操作：
  - list_init(&free_list)：初始化空闲链表为空。
  - nr_free = 0：清零空闲页计数。
- 何时调用：系统启动早期，用于准备管理器。

---

### default_init_memmap(struct Page *base, size_t n)
- 作用：将从 base 开始、长度为 n 的连续页区加入空闲池。
- 关键操作：
  - 遍历 n 个页，清除 flags、property，set ref = 0（每页初始化）。
  - 在 base 页设置 base->property = n 并设置 PG_property（标记为空闲块首页）。
  - nr_free += n（更新全局空闲页数）。
  - 将 base 的 page_link 按地址顺序插入 free_list（保持链表有序）。
- 备注：进入空闲链表的只有每个空闲块的首页。

---

### default_alloc_pages(size_t n)
- 作用：分配连续的 n 页。
- 搜索过程：
  - 从 free_list 头开始顺序遍历各空闲块首页 p。
  - 找到第一个满足 p->property >= n 的块。
- 分配与分割：
  - 若 p->property == n：将该块从 free_list 中移除（整块分配）。
  - 若 p->property > n：分割，返回 p（首页）作为分配区，剩余部分的首页为 p + n，设置其 property = old - n 并重新插入链表（保持有序位置）。
- 更新：
  - nr_free -= n。
  - 清除分配区首页的 PG_property 标志（该页不再作为空闲块首页）。
- 返回：分配到的首页指针；若无满足块则返回 NULL。

---

### default_free_pages(struct Page *base, size_t n)
- 作用：释放从 base 开始的 n 页并回收到空闲池，尝试合并相邻空闲块。
- 插入前置处理：
  - 遍历 n 页，清 flags，set ref = 0。
  - base->property = n，SetPageProperty(base)，把 base 标记为空闲块首页。
  - nr_free += n。
- 有序插入：
  - 在 free_list 中按地址位置插入 base 的 page_link，保持链表有序。
- 合并：
  - 向前合并：检查插入位置前一个块 p，若 p + p->property == base，则合并（p->property += base->property），移除 base 的链表节点并清除 base 的 PG_property。
  - 向后合并：检查插入位置后一个块 q，若 base + base->property == q，则合并（base->property += q->property），移除 q 的链表节点并清除 q 的 PG_property。
- 结果：通过合并减少碎片、形成更大连续空闲区。

---

## 2.分配/释放总体流程：
- 初始化阶段（pmm_init -> init_pmm_manager -> page_init -> init_memmap）：
  - pmm_init() 调用 init_pmm_manager()，在其中选择具体的 pmm 管理器（默认是 default_pmm_manager）并调用 pmm_manager->init()，对应 default_init()。
  - 随后 page_init() 检测内存边界、计算 npage、设置 pages 数组并标记内核占用页为已保留。
  - page_init() 计算可用物理内存区间并调用 init_memmap(pa2page(mem_begin), cnt)，init_memmap 是对 pmm_manager->init_memmap 的封装，最终由具体实现（如 default_init_memmap）建立 free_list 并更新 nr_free。

- 分配时（调用 alloc_pages -> pmm_manager->alloc_pages -> default_alloc_pages）：
  - 上层通过 alloc_pages(n) 调用 pmm.c 中的包装函数，该函数直接返回 pmm_manager->alloc_pages(n) 的结果。
  - 在 default_alloc_pages(n)中：顺序遍历 free_list，找到第一个 p->property >= n 的空闲块；
    - 若 p->property == n：从 free_list 中删除该块的首页，设置相应标志并返回该页；
    - 若 p->property > n：将该块分割，返回前 n 页作为分配区，剩余部分调整为新的空闲块，更新其 property 和链表位置。
  - 分配完成后更新 nr_free（减去 n）。

- 释放时（调用 free_pages -> pmm_manager->free_pages -> default_free_pages）：
  - 上层通过 free_pages(base, n) 调用 pmm.c 中的包装函数，该函数转发到 pmm_manager->free_pages(base, n)。
  - 在 default_free_pages(base, n) 中：把释放区初始化为一个新的空闲块（base->property = n，SetPageProperty(base)），并按物理地址有序插入 free_list，更新 nr_free（加上 n）。
  - 插入后尝试与前/后相邻空闲块合并，若相邻则合并并更新合并后块的 property，移除被合并节点，以减少碎片。

## 3. First-fit 算法改进空间

### 链表搜索效率
- 问题：每次分配需从头遍历 free_list，最坏时间复杂度为 O(N)。
- 改进：
  - 维护按大小分组的子链表：将空闲块按大小分类或按区间划分，分配时先定位合适大小组，减少扫描范围。

### 外部碎片
- 问题：频繁分配/释放后会产生大量小碎片，导致无法满足大块请求。
- 改进：
  - 阈值分割：只有当剩余空间大于某个最小分割阈值（如 MIN_SPLIT_SIZE）时才对空闲块进行分割，避免产生过小的碎片块。
  - 碎片整理：在碎片总量达到一定阈值时，移动分配给进程的内存分区，以合并那些被阻隔的碎片。

### 前后合并
- 问题：当前合并函数只会向前后做一次检查。
- 改进：
  - 迭代：通过while实现迭代合并，直到前后块不相连。



# 练习2：实现 Best-Fit 连续物理内存分配算法


## 1. Best-fit 算法实现过程

### default_init()
- 作用：初始化 PMM 管理器的内部状态。
- 已完成，不需要修改。

---

### default_init_memmap(struct Page *base, size_t n)
- 作用：将从 base 开始、长度为 n 的连续页区加入空闲池。
- 关键操作：
  - 遍历 n 个页，清除 flags、property，set ref = 0（每页初始化）。
  - 将 base 的 page_link 按地址顺序插入 free_list（保持链表有序）。

---

### default_alloc_pages(size_t n)
- 作用：分配连续的 n 页。
- 关键操作：
  - 搜索过程：
    - 新增变量min_size记录当前最佳选择的块大小，初始值为nr_free + 1。
    - 从 free_list 头开始顺序遍历各空闲块首页 p。
    - 找到满足需求(p->property >= n)且比当前最佳选择更合适(p->property < min_size)的块，更新page和min_size。
    -如果找到(p->property = n)的块，提前退出。
  - 分配与分割：
    - 若 original_block_size < =n + 5：多余部分没有超过阈值，将该块从 free_list 中移除（整块分配）。
    - 若 p->property > n：分割，返回 p（首页）作为分配区，剩余部分的首页为 p + n，设置其 property  =old - n 并重新插入链表（保持有序位置）。


---

### default_free_pages(struct Page *base, size_t n)
- 作用：释放从 base 开始的 n 页并回收到空闲池，尝试合并相邻空闲块。
- 关键操作：
  - 插入前置处理：
    - base->property = n，SetPageProperty(base)，把 base 标记为空闲块首页。
    - nr_free += n。
  - 合并：
    - 用while实现迭代合并，向前后合并迭代合并，直到前后没有地址连续的空闲页块。


---

### 测试结果：
```
os@yzy13502629353:~/lab2$ make grade
>>>>>>>>>> here_make>>>>>>>>>>>
make[1]: Entering directory '/home/os/lab2' + cc kern/init/entry.S + cc kern/init/init.c + cc kern/libs/stdio.c + cc kern/debug/panic.c + cc kern/driver/dtb.c + cc kern/driver/console.c + cc kern/mm/pmm.c + cc kern/mm/default_pmm.c + cc kern/mm/best_fit_pmm.c + cc libs/printfmt.c + cc libs/sbi.c + cc libs/readline.c + cc libs/string.c + ld bin/kernel riscv64-unknown-elf-objcopy bin/kernel --strip-all -O binary bin/ucore.img make[1]: Leaving directory '/home/os/lab2'
>>>>>>>>>> here_make>>>>>>>>>>>
<<<<<<<<<<<<<<< here_run_qemu <<<<<<<<<<<<<<<<<<
try to run qemu
qemu pid=57357
<<<<<<<<<<<<<<< here_run_check <<<<<<<<<<<<<<<<<<
  -check physical_memory_map_information:    OK
  -check_best_fit:                           OK
Total Score: 25/25
```

## 2. 分配/释放总体流程（Best-fit 实现）：

- 初始化阶段（pmm_init -> init_pmm_manager -> page_init -> init_memmap）：
  - pmm_init() 调用 init_pmm_manager()，在其中选择具体的 pmm 管理器（此处为 best_fit_pmm_manager）并调用 pmm_manager->init()，对应 best_fit_init()，它会初始化空闲链表 free_list 并清零 nr_free。
  - 随后 page_init() 检测物理内存，并为可用内存区域调用 init_memmap()。
  - init_memmap() 最终调用 best_fit_init_memmap()，该函数将新的空闲内存块按物理地址顺序插入到 free_list 中，以保证链表始终有序。

- 分配时（调用 alloc_pages -> pmm_manager->alloc_pages -> best_fit_alloc_pages）：
  - 上层通过 alloc_pages(n) 调用，最终转发至 best_fit_alloc_pages(n)。
  - 搜索：该函数会遍历整个 free_list，寻找一个大小 p->property >= n 且最接近 n 的空闲块（即 p->property 最小）。如果找到大小正好为 n 的块，则提前结束搜索。
  - 分割与分配：
    - 如果找到的最佳块大小 original_block_size 与请求大小 n 的差值大于阈值（代码中为 > n + 5），则进行分割。前 n 页被分配，剩余部分形成一个新的、更小的空闲块，并被重新插入到链表中。nr_free 净减少 n。
    - 如果不满足分割条件，则为了避免产生过小的碎片，将整个块全部分配出去，导致内部碎片。nr_free 减少 original_block_size。
    - 返回分配到的内存块的首页指针。

- 释放时（调用 free_pages -> pmm_manager->free_pages -> best_fit_free_pages）：
  - 上层通过 free_pages(base, n) 调用，最终转发至 best_fit_free_pages(base, n)。
  - 插入：函数首先将要释放的 n 页内存作为一个新的空闲块，按物理地址有序插入到 free_list 中，并更新 nr_free。
  - 迭代合并：
    - 向前合并：循环检查新插入块前面的块是否与其物理地址连续。如果连续，则合并它们（更新前块的 property，删除当前块的节点），并继续向前检查，直到无法合并为止。
    - 向后合并：循环检查合并后块后面的块是否与其物理地址连续。如果连续，则合并它们（更新当前块的 property，删除后块的节点），并继续向后检查，直到无法合并为止。这个过程能有效地将相邻的小碎片合并成大块。


## 3. First-fit 算法改进空间：

### 链表搜索效率
- 问题：每次分配需从头遍历 free_list，最坏时间复杂度为 O(N)。
- 改进：
  - 维护按大小分组的子链表：将空闲块按大小分类或按区间划分，分配时先定位合适大小组，减少扫描范围。



