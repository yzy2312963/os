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



# 练习2：实现 Best-Fit 连续物理内存分配算法


## 1. Best-fit 算法实现过程

### best_fit_init()
- 作用：初始化 PMM 管理器的内部状态。
- 已完成，不需要修改。

---

### best_fit_init_memmap(struct Page *base, size_t n)
- 作用：将从 base 开始、长度为 n 的连续页区加入空闲池。
- 关键操作：
  - 遍历 n 个页，清除 flags、property，set ref = 0（每页初始化）。
  - 将 base 的 page_link 按地址顺序插入 free_list（保持链表有序）。

---

### best_fit_alloc_pages(size_t n)
- 作用：分配连续的 n 页。
- 关键操作：
  - 搜索过程：
    - 新增变量min_size记录当前最佳选择的块大小，初始值为nr_free + 1。
    - 从 free_list 头开始顺序遍历各空闲块首页 p。
    - 找到满足需求(p->property >= n)且比当前最佳选择更合适(p->property < min_size)的块，更新page和min_size。
    -如果找到(p->property = n)的块，提前退出。
  - 分配与分割：
    - 若 p->property > n：分割，返回 p（首页）作为分配区，剩余部分的首页为 p + n，设置其 property  =p->property - n 并重新插入链表（保持有序位置）。


---

### best_fit_free_pages(struct Page *base, size_t n)
- 作用：释放从 base 开始的 n 页并回收到空闲池，尝试合并相邻空闲块。
- 关键操作：
  - 插入前置处理：
    - base->property = n，SetPageProperty(base)，把 base 标记为空闲块首页。
    - nr_free += n。
  - 向前合并：
    - 检查插入位置前一个块 p，若 p + p->property == base，则合并（p->property += base->property），移除 base 的链表节点并清除 base 的 PG_property。


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

# 扩展练习Challenge：buddy system（伙伴系统）分配算法

### 算法简介
伙伴系统是一种高效的内存管理算法，主要用于减少外部碎片。其核心思想是将物理内存划分为大小均为2的幂次方的块，并通过伙伴合并机制来优化内存使用。

算法核心机制：

- 内存分层：将可用内存按2的幂次方大小划分为多个层级（阶），每个阶维护一个空闲链表

- 分配策略：当请求n页内存时，找到满足2^k ≥ n的最小k阶块。如果该阶无空闲块，则分裂更高阶的块

- 伙伴定义：两个块互为伙伴当且仅当：

  - 大小相同（同一阶）

  - 物理地址连续

  - 起始地址对齐到2^(order+1)边界

- 合并机制：释放内存时，检查伙伴块是否空闲，如果是则合并为更大的块，不断递归直到无法继续合并

- 优缺点：

  - 优点：外部碎片极少，分配/释放效率高（O(logN)）

  - 缺点：可能产生内部碎片，最大连续内存受最大阶限制

### 具体函数

buddy_system_init 负责初始化伙伴系统的数据结构。该函数遍历从 0 到 MAX_ORDER 的所有阶，对每个空闲区域（free_area_buddy[i]）调用 list_init() 初始化空链表，并将空闲计数器 nr_free 设置为 0。该过程仅完成结构层面的准备，不涉及具体内存块，是系统启动时的必要步骤，为后续的内存映射初始化奠定基础。

buddy_system_init_memmap 函数将物理内存映射成伙伴系统可管理的形式。它会初始化所有物理页的标志位与引用计数，然后根据内存大小计算可容纳的最大阶，设置基础页面的阶属性，并将其加入相应阶的空闲链表中。如果存在未覆盖的剩余空间，则通过递归方式继续初始化。该函数是伙伴系统的核心初始化阶段，真正完成了物理内存向管理结构的组织映射。

buddy_system_alloc_pages 和 buddy_system_free_pages 实现内存的分配与释放。前者根据请求页数计算最小满足要求的阶，从该阶开始查找可用的空闲块；若块过大，则逐级分裂，将多余的伙伴块放回低阶链表，最终得到所需大小的块并返回。后者则在释放页面时尝试与伙伴块合并：检查伙伴块是否空闲且大小相同，若满足条件，则将两块合并成更高阶的块，继续向上合并直到无法再合并为止。两者共同构成了伙伴系统的核心逻辑，体现了其高效的动态分配与回收机制。

buddy_system_nr_free_pages 与 buddy_system_print_status 完成系统的统计与状态。前者用于计算系统中当前空闲的页面总数，它遍历所有阶的空闲链表，将每个阶的空闲块数量乘以对应的块大小后累加。后者则以更直观的形式展示当前系统状态，打印出总空闲页数及各阶的空闲块数量，用于调试与性能分析。

basic_check 用于进行最基本的功能测试，包括单页分配与释放、引用计数检查及内存守恒验证等，用以保证系统的核心机制正常工作。而 buddy_system_check 则执行更为全面的测试，包括不同大小块的分配、伙伴合并验证、内存不足处理、碎片整理等多个场景，确保系统在复杂环境下依然保持稳定性与一致性。

系统中还包含若干辅助性函数，用于计算与元数据维护。power_of_two 负责计算 2 的阶次幂，用于确定每个阶对应的块大小；log2_ceil 计算满足 2^order ≥ n 的最小阶数，用于确定分配所需的阶；get_page_order 与 set_page_order 分别用于读取和设置页面的阶属性；is_buddy 用于判断两个块是否互为伙伴，其依据是大小相同、地址连续且按 2^(order+1) 对齐；get_buddy 则通过异或运算快速计算出伙伴块的地址。这些函数虽逻辑简单，但在系统运行中承担了关键的支撑作用。

### 算法测试

#### 测试1：基本分配与释放测试

**测试目的**  
验证伙伴系统最基础的分配与释放机制是否正常工作。重点考察单页分配的正确性、释放后内存状态的恢复能力，以及空闲链表的计数变化是否符合预期。该测试确保系统的最小分配单元操作正确，是后续复杂功能的基础。

**测试过程**  
执行一次单页（1页）的分配操作，确认返回的页面指针有效，并验证系统空闲页数量减少。随后释放该页面，检查其重新回到 `order=0` 的空闲链表中，并确认空闲页数量恢复到初始值。

**预期结果**  
系统从 `order=0` 的空闲链表中分配一个页面，链表节点数量减少 1；释放后，该页面应重新插入回原阶链表，系统状态完全恢复，无残留引用或计数错误。

---

#### 测试2：多阶块分配测试

**测试目的**  
验证系统能否同时正确处理不同大小（2页、4页、8页）的分配请求，检查多阶并发分配时的逻辑一致性和资源管理能力。测试重点在于系统的多阶分配机制是否能灵活调度空闲块。

**测试过程**  
依次分配 2 页、4 页和 8 页大小的内存块，记录返回的页面指针并确认每次分配成功。随后释放所有已分配的块，检查系统是否恢复原状。

**预期结果**  
系统应分别从 `order=1`、`order=2` 和 `order=3` 的空闲链表中分配块；若某阶链表为空，应自动向上分裂高阶块以满足需求。释放后，各块应完整返回原阶链表，空闲页数量恢复初始状态。

---

#### 测试3：伙伴块自动合并测试

**测试目的**  
验证伙伴系统在释放操作中自动检测并合并伙伴块的能力。测试的重点是确认系统能识别相邻空闲块，并在满足条件时进行正确的合并操作。

**测试过程**  
分配一个 2 页大小的内存块（`order=1`），观察系统对内部两个单页的管理。随后释放该块，检查释放后是否能触发伙伴合并机制。

**预期结果**  
当释放时，系统应检测到两个相邻的单页（`order=0`）均处于空闲状态，执行合并操作生成一个新的 2 页块，并将其插入到 `order=1` 的空闲链表中，完成自动合并过程。

---

#### 测试4：内存不足异常处理测试

**测试目的**  
验证系统在内存资源不足时的稳定性与容错性。测试重点是检查当分配请求超出可用内存范围时，系统能否安全地拒绝分配，而不会造成数据结构损坏或系统崩溃。

**测试过程**  
尝试分配一个超过系统总空闲页数的大块（如整片内存的两倍大小），观察函数返回值与系统状态。

**预期结果**  
系统应在遍历所有阶的空闲链表后发现无可用块，返回空指针（NULL），并保持所有链表、页标志与计数器完全不变，确保系统数据结构的完整性。

---

#### 测试5：精确阶分配测试

**测试目的**  
验证系统在处理恰好为 2 的幂次方大小请求时的效率与正确性，确保不会执行多余的块分裂或合并操作，从而评估系统在理想分配场景下的性能表现。

**测试过程**  
请求分配 16 页（`order=4`）大小的内存块，并记录系统空闲链表的变化情况。随后释放该块，检查系统状态是否正确恢复。

**预期结果**  
系统应直接从 `order=4` 的空闲链表中分配整个块，不进行任何分裂操作。释放后，该块应完整返回原链表，无需进行伙伴合并，空闲计数恢复正常。

---

#### 测试6：复杂混合分配测试

**测试目的**  
测试系统在多种大小混合分配与释放场景下的行为，评估伙伴系统的动态适应能力。重点在于验证多阶分配与释放过程中，链表管理和空闲计数的准确性。

**测试过程**  
依次分配 1 页、2 页和 4 页的内存块，并打印分配后系统状态。随后依次释放所有块，再次打印状态，检查各阶链表是否恢复初始状态。

**预期结果**  
分配时，系统应在多个阶之间执行分裂操作以满足需求，状态输出应显示对应阶空闲块数量减少。释放后，各块应正确回收至原阶链表，系统恢复到初始平衡状态。

---

#### 测试7：非 2 的幂次分配测试

**测试目的**  
验证系统在处理非 2 的幂大小请求时，能否正确地执行“向上取整”机制，确保分配的实际块大小满足伙伴系统的对齐与合并要求。

**测试过程**  
分别请求分配 3 页、5 页和 7 页大小的内存块，记录系统为每次请求选择的实际阶数。随后释放所有块，检查系统是否正确恢复。

**预期结果**  
系统应将 3 页请求取整到 4 页（`order=2`），5 页和 7 页请求取整到 8 页（`order=3`）。若对应阶链表为空，应触发上阶块分裂。释放后系统应能正确地回收并合并这些块，保证空闲状态一致。

---

#### 测试8：碎片合并与整理测试

**测试目的**  
模拟内存碎片化场景，检验伙伴系统在长期运行后通过自动合并机制恢复内存连续性的能力。该测试关注系统的碎片整理与空间再利用效果。

**测试过程**  
循环交替分配不同大小的块（1页、2页、4页），然后以不同顺序释放部分块，制造内存碎片。最后尝试分配一个大块（如 8 页），观察系统是否能通过合并机制完成分配。

**预期结果**  
在交错释放后系统应出现碎片化现象，但当分配大块时，伙伴系统应自动检测可合并的相邻空闲块，合并形成更高阶块，从而成功完成大块分配。这验证了系统在碎片化场景下的自修复与内存整合能力。


