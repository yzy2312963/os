## 练习1: 完成读文件操作的实现（需要编码）

### 1. 实验内容

首先了解打开文件的处理流程，然后参考本实验后续的文件读写操作的过程分析，填写在 kern/fs/sfs/sfs_inode.c中 的sfs_io_nolock()函数，实现读文件中数据的代码。

---

### 2. 设计思路

#### （1）按块读取的基本思想

SFS 文件系统以磁盘块（block）为最小存储单位，而用户请求的读操作往往：

- 起始位置不在块边界
- 结束位置不在块边界
- 跨越多个磁盘块

因此，文件读取不能简单地一次完成，而需要分块处理。

---

#### （2）文件读取的三种情况

整个读取过程可以分为三部分：

1. **第一个块（可能未对齐）**  
   - 读取位置位于块中间  
   - 需要计算块内偏移  
   - 只能读取该块的剩余部分  
```
blkoff = pos % SFS_BLKSIZE;
    if (blkoff != 0) {
        size = (endpos / SFS_BLKSIZE != (uint32_t)(pos / SFS_BLKSIZE)) ?
               (SFS_BLKSIZE - blkoff) : (size_t)(endpos - pos);

        if ((ret = sfs_bmap_load_nolock(sfs, sin, (uint32_t)(pos / SFS_BLKSIZE), &ino)) != 0) {
            goto out;
        }
        if ((ret = sfs_buf_op(sfs, p, size, ino, blkoff)) != 0) {
            goto out;
        }

        p   += size;
        pos += size;
        alen += size;
    }
```

2. **中间的完整块**  
   - 完整覆盖磁盘块  
   - 可以直接整块读取，效率最高  
```
while (pos + SFS_BLKSIZE <= endpos) {
        if ((ret = sfs_bmap_load_nolock(sfs, sin, (uint32_t)(pos / SFS_BLKSIZE), &ino)) != 0) {
            goto out;
        }
        if ((ret = sfs_block_op(sfs, p, ino, 1)) != 0) {
            goto out;
        }

        p   += SFS_BLKSIZE;
        pos += SFS_BLKSIZE;
        alen += SFS_BLKSIZE;
    }
```

3. **最后一个块（可能未对齐）**  
   - 剩余数据不足一个完整块  
   - 从块起始位置读取部分数据  
```
if (pos < endpos) {
        size = (size_t)(endpos - pos);

        if ((ret = sfs_bmap_load_nolock(sfs, sin, (uint32_t)(pos / SFS_BLKSIZE), &ino)) != 0) {
            goto out;
        }
        if ((ret = sfs_buf_op(sfs, p, size, ino, 0)) != 0) {
            goto out;
        }

        alen += size;
    }
```

---
## 练习2: 完成基于文件系统的执行程序机制的实现（需要编码）

### 1. 实验内容
改写proc.c中的load_icode函数和其他相关函数，实现基于文件系统的执行程序机制。执行：make qemu。如果能看看到sh用户程序的执行界面，则基本成功了。如果在sh用户界面上可以执行exit, hello（更多用户程序放在user目录下）等其他放置在sfs文件系统中的其他执行程序，则可以认为本实验基本成功。

### 2. 关键实现步骤分析
#### （1）tlb刷新
在lab4的代码部分增加tlb刷新函数
```
if (proc != current)
    {
	bool intr_flag;
        struct proc_struct *prev = current;

        local_intr_save(intr_flag);
        {
            current = proc;
            lsatp(proc->pgdir);
            flush_tlb();
            switch_to(&(prev->context), &(proc->context));
        }
        local_intr_restore(intr_flag); 
    }
```
#### （2）进程地址空间的初始化
在加载用户程序之前，需要确保当前进程没有已有的地址空间：
```
if (current->mm != NULL) {
        panic("load_icode: current->mm must be empty.\n");
    }

    int ret = -E_NO_MEM;
    struct mm_struct *mm;
    
    //(1) Create a new mm for current process
    if ((mm = mm_create()) == NULL) {
        goto bad_mm;
    }
    
    //(2) Create a new PDT, and mm->pgdir = kernel virtual addr of PDT
    if (setup_pgdir(mm) != 0) {
        goto bad_pgdir_cleanup_mm;
    }
```

#### （3）从文件中读取 ELF 文件头
与 LAB5 直接访问内存中二进制不同，LAB8 中程序存储在文件系统中，因此：
需要通过文件描述符 fd
使用 load_icode_read() 从文件中读取 ELF Header
读取完成后，通过检查 ELF 魔数来验证文件是否合法，避免加载非 ELF 格式的文件。
```
struct elfhdr __elf, *elf = &__elf;
    if ((ret = load_icode_read(fd, elf, sizeof(struct elfhdr), 0)) != 0) {
        goto bad_elf_cleanup_pgdir;
    }
    
    //(3.2) Check if this is a valid ELF file

    if (elf->e_magic != ELF_MAGIC) {
        ret = -E_INVAL_ELF;
        goto bad_elf_cleanup_pgdir;
    }
    
    //(3.3) Read program headers

    struct proghdr __ph, *ph = &__ph;
    uint32_t vm_flags, perm;
    
    for (int i = 0; i < elf->e_phnum; i++) {
        off_t phoff = elf->e_phoff + i * sizeof(struct proghdr);
        if ((ret = load_icode_read(fd, ph, sizeof(struct proghdr), phoff)) != 0) {
            goto bad_cleanup_mmap;
        }
```

#### （4）逐个加载程序段（Program Header）

ELF 文件中可能包含多个程序段，代码通过遍历 Program Header 表，对每一个段进行处理：
对于每一个可加载段，执行以下操作：
（1）建立虚拟内存区域（VMA）
（2）加载 TEXT / DATA 段内容
（3）构建 BSS 段

```
if (ph->p_type != ELF_PT_LOAD) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz) {
            ret = -E_INVAL_ELF;
            goto bad_cleanup_mmap;
        }
        
        //(3.5) Setup VMA for this segment
        vm_flags = 0;
        perm = PTE_U | PTE_V;
        if (ph->p_flags & ELF_PF_X) vm_flags |= VM_EXEC;
        if (ph->p_flags & ELF_PF_W) vm_flags |= VM_WRITE;
        if (ph->p_flags & ELF_PF_R) vm_flags |= VM_READ;
        
        // Set RISC-V page permissions
        if (vm_flags & VM_READ) perm |= PTE_R;
        if (vm_flags & VM_WRITE) perm |= (PTE_W | PTE_R);
        if (vm_flags & VM_EXEC) perm |= PTE_X;
        
        if ((ret = mm_map(mm, ph->p_va, ph->p_memsz, vm_flags, NULL)) != 0) {
            goto bad_cleanup_mmap;
        }
        
        //(3.6) Allocate memory and load segment content
        size_t off, size;
        uintptr_t start = ph->p_va, end, la = ROUNDDOWN(start, PGSIZE);
        
        ret = -E_NO_MEM;
        
        //(3.6.1) Load TEXT/DATA from file
        end = ph->p_va + ph->p_filesz;
        while (start < end) {
            if ((page = pgdir_alloc_page(mm->pgdir, la, perm)) == NULL) {
                goto bad_cleanup_mmap;
            }
            off = start - la;
            size = PGSIZE - off;
            if (end < la + PGSIZE) {
                size = end - start;
            }
            if ((ret = load_icode_read(fd, page2kva(page) + off, size, ph->p_offset + (start - ph->p_va))) != 0) {
                goto bad_cleanup_mmap;
            }
            start += size;
            la += PGSIZE;
        }
        
        //(3.6.2) Build BSS section (zero-filled)
        end = ph->p_va + ph->p_memsz;
        if (start < la) {
            if (start < end) {
                off = start - (la - PGSIZE);
                size = PGSIZE - off;
                if (end < la) {
                    size = end - start;
                }
                memset(page2kva(page) + off, 0, size);
                start += size;
            }
        }
        while (start < end) {
            if ((page = pgdir_alloc_page(mm->pgdir, la, perm)) == NULL) {
                goto bad_cleanup_mmap;
            }
            off = start - la;
            size = PGSIZE - off;
            if (end < la + PGSIZE) {
                size = end - start;
            }
            memset(page2kva(page) + off, 0, size);
            start += size;
            la += PGSIZE;
        }
    }
```

#### （5）建立用户栈
程序段加载完成后，需要为用户进程建立运行时栈空间：
```
vm_flags = VM_READ | VM_WRITE | VM_STACK;
    if ((ret = mm_map(mm, USTACKTOP - USTACKSIZE, USTACKSIZE, vm_flags, NULL)) != 0) {
        goto bad_cleanup_mmap;
    }
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP - PGSIZE, PTE_USER) != NULL);
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP - 2 * PGSIZE, PTE_USER) != NULL);
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP - 3 * PGSIZE, PTE_USER) != NULL);
    assert(pgdir_alloc_page(mm->pgdir, USTACKTOP - 4 * PGSIZE, PTE_USER) != NULL);
```
#### （6）切换到新的地址空间
```
mm_count_inc(mm);
    current->mm = mm;
    current->pgdir = PADDR(mm->pgdir);
    lsatp(PADDR(mm->pgdir));
```
#### （7）构建 argc / argv 并放入用户栈
```
uintptr_t stacktop = USTACKTOP;
    
    // Copy argument strings to user stack
    char **uargv = (char **)(stacktop - (argc + 1) * sizeof(char *));
    stacktop = (uintptr_t)uargv - ((stacktop - (uintptr_t)uargv) & 0xF); // 16-byte align
    
    // Reserve space for argv pointers
    uargv = (char **)(stacktop - (argc + 1) * sizeof(char *));
    
    // Copy each argument string
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(kargv[i]) + 1;
        stacktop -= len;
        strcpy((char *)stacktop, kargv[i]);
        uargv[i] = (char *)stacktop;
    }
    uargv[argc] = NULL;
    
    // Align stack pointer
    stacktop = (uintptr_t)uargv;
    stacktop &= ~0xF; // 16-byte alignment for RISC-V
```
#### （8）初始化 trapframe 并进入用户态
```
struct trapframe *tf = current->tf;
    uintptr_t sstatus = tf->status;
    memset(tf, 0, sizeof(struct trapframe));
    
    tf->gpr.sp = stacktop;
    tf->epc = elf->e_entry;
    tf->status = sstatus & ~(SSTATUS_SPP | SSTATUS_SPIE);
    
    // Setup argc and argv in registers (RISC-V calling convention)
    tf->gpr.a0 = argc;
    tf->gpr.a1 = (uintptr_t)uargv;
```
---

## 扩展练习 Challenge1：完成基于"UNIX的PIPE机制"的设计方案

### 1. 概述

UNIX 管道（Pipe）是一种进程间通信（IPC）机制，允许一个进程的输出直接作为另一个进程的输入。在 ucore 中实现管道机制，需要设计相应的数据结构、接口，并妥善处理同步互斥问题。

### 2. 数据结构设计

#### 2.1 管道核心结构体 `pipe_t`

管道可以视为一种特殊的文件，需要定义核心数据结构来管理缓冲区和同步机制：

```c
#define PIPE_BUFFER_SIZE 4096  // 管道缓冲区大小，参考 Linux 默认值

struct pipe {
    char buffer[PIPE_BUFFER_SIZE];  // 环形缓冲区
    size_t head;                    // 写位置（头部）
    size_t tail;                    // 读位置（尾部）
    size_t count;                   // 当前缓冲区中的数据量
    
    // 引用计数
    int read_ref;                   // 读端引用计数
    int write_ref;                  // 写端引用计数
    
    // 同步互斥机制
    struct semaphore sem_read;      // 读信号量（初始值为0）
    struct semaphore sem_write;     // 写信号量（初始值为PIPE_BUFFER_SIZE）
    struct spinlock lock;           // 保护管道结构的自旋锁
    
    // 等待队列（用于阻塞等待）
    struct wait_queue wait_readers; // 等待读取的进程队列
    struct wait_queue wait_writers; // 等待写入的进程队列
    
    // 状态标志
    bool readable;                  // 读端是否打开
    bool writable;                  // 写端是否打开
};
```

**设计说明：**
- `buffer`：使用固定大小的环形缓冲区存储数据
- `head` 和 `tail`：分别表示写位置和读位置，实现环形缓冲
- `count`：记录当前缓冲区中的数据量，用于快速判断缓冲区状态
- `read_ref` 和 `write_ref`：引用计数，支持多个进程共享同一管道端
- `sem_read` 和 `sem_write`：信号量用于同步读写操作
- `lock`：自旋锁保护对管道结构的并发访问
- `wait_readers` 和 `wait_writers`：等待队列，用于实现阻塞式读写

#### 2.2 文件描述符扩展

在 ucore 的现有文件系统中，需要扩展文件结构体以支持管道：

```c
struct file {
    enum {
        FD_NONE,
        FD_INIT,
        FD_OPENED,
        FD_CLOSED,
        FD_PIPE  // 新增：管道类型
    } status;
    
    bool readable;
    bool writable;
    int fd;
    off_t pos;
    struct inode *node;
    atomic_t open_count;
    
    // 管道相关字段
    struct pipe *pipe_ptr;  // 指向关联的管道结构体
    bool is_pipe_read;      // 是否为管道的读端
};
```

#### 2.3 管道 inode 结构（可选）

如果管道作为特殊文件系统的一部分，可以定义：

```c
struct pipe_inode {
    struct inode inode;     // 继承基础 inode
    struct pipe *pipe_data; // 指向管道数据
};
```

### 3. 接口设计

#### 3.1 创建管道 `sys_pipe`

**语义：** 创建一个新的管道，返回两个文件描述符，`pipefd[0]` 用于读，`pipefd[1]` 用于写。

```c
int sys_pipe(int pipefd[2]);
```

**功能说明：**
- 分配并初始化一个新的 `pipe` 结构体
- 创建两个文件描述符，分别绑定到管道的读端和写端
- 初始化所有同步机制（信号量、自旋锁、等待队列）
- 设置引用计数为 1

#### 3.2 读取管道 `pipe_read`

**语义：** 从管道的读端读取最多 `count` 字节的数据到 `buf` 中，返回实际读取的字节数。如果管道为空且写端已关闭，返回 0；如果管道为空但写端未关闭，阻塞等待。

```c
ssize_t pipe_read(struct file *file, void *buf, size_t count);
```

**功能说明：**
- 检查文件描述符是否为管道的读端
- 获取自旋锁保护临界区
- 如果缓冲区为空且写端未关闭，阻塞等待
- 从环形缓冲区读取数据，更新 `tail` 和 `count`
- 唤醒等待写入的进程
- 释放锁并返回读取的字节数

#### 3.3 写入管道 `pipe_write`

**语义：** 向管道的写端写入最多 `count` 字节的数据，返回实际写入的字节数。如果缓冲区已满，阻塞等待；如果读端已关闭，返回错误。

```c
ssize_t pipe_write(struct file *file, const void *buf, size_t count);
```

**功能说明：**
- 检查文件描述符是否为管道的写端
- 检查读端是否已关闭（如果关闭则返回 EPIPE 错误）
- 获取自旋锁保护临界区
- 如果缓冲区已满，阻塞等待
- 向环形缓冲区写入数据，更新 `head` 和 `count`
- 唤醒等待读取的进程
- 释放锁并返回写入的字节数

#### 3.4 关闭管道端 `pipe_close`

**语义：** 关闭管道的读端或写端，减少相应的引用计数。当读端和写端都关闭时，释放管道资源。

```c
int pipe_close(struct file *file);
```

**功能说明：**
- 根据 `is_pipe_read` 判断关闭的是读端还是写端
- 减少相应的引用计数（`read_ref` 或 `write_ref`）
- 设置相应的标志位（`readable` 或 `writable`）为 false
- 唤醒所有等待的进程（因为管道状态改变）
- 如果两端都关闭（引用计数为 0），释放管道缓冲区

#### 3.5 辅助接口

```c
// 初始化管道结构
int pipe_init(struct pipe *p);

// 销毁管道结构
void pipe_destroy(struct pipe *p);

// 检查管道是否可读
bool pipe_readable(struct pipe *p);

// 检查管道是否可写
bool pipe_writable(struct pipe *p);

// 获取管道中可用数据量
size_t pipe_data_count(struct pipe *p);

// 获取管道中可用空间
size_t pipe_space_count(struct pipe *p);
```

### 4. 同步互斥问题处理

#### 4.1 互斥访问控制

**问题：** 多个进程可能同时访问管道的缓冲区，导致数据竞争和不一致。

**解决方案：**
- 使用自旋锁 `lock` 保护整个管道结构体的访问
- 所有对 `head`、`tail`、`count`、`read_ref`、`write_ref` 等关键字段的修改都必须在持有锁的情况下进行
- 锁的粒度：在读写操作的整个过程中持有锁，确保原子性

**示例代码逻辑：**
```c
// 读操作中的互斥保护
acquire_lock(&p->lock);
// 检查缓冲区状态、读取数据、更新状态
release_lock(&p->lock);
```

#### 4.2 读写同步机制

**问题：** 读进程需要在有数据时才能读取，写进程需要在有空间时才能写入。

**解决方案：**
- **信号量机制：**
  - `sem_read`：初始值为 0，表示可读数据量，每次写入后增加，每次读取后减少
  - `sem_write`：初始值为 `PIPE_BUFFER_SIZE`，表示可用空间，每次读取后增加，每次写入后减少
- **等待队列机制：**
  - `wait_readers`：当缓冲区为空时，读进程加入此队列等待
  - `wait_writers`：当缓冲区满时，写进程加入此队列等待
  - 当条件满足时，唤醒相应的等待进程

**同步流程：**

1. **读操作同步：**
   ```
   - 获取锁
   - 如果 count == 0（缓冲区空）：
     - 如果 write_ref == 0（写端已关闭）：返回 0（EOF）
     - 否则：加入 wait_readers 队列，释放锁，阻塞等待
   - 从缓冲区读取数据
   - 更新 tail, count
   - 唤醒 wait_writers 队列中的进程
   - 释放锁
   ```

2. **写操作同步：**
   ```
   - 获取锁
   - 如果 read_ref == 0（读端已关闭）：返回 EPIPE 错误
   - 如果 count == PIPE_BUFFER_SIZE（缓冲区满）：
     - 加入 wait_writers 队列，释放锁，阻塞等待
   - 向缓冲区写入数据
   - 更新 head, count
   - 唤醒 wait_readers 队列中的进程
   - 释放锁
   ```

#### 4.3 环形缓冲区实现

**问题：** 需要正确处理环形缓冲区的边界情况，避免数据覆盖。

**解决方案：**
- 使用模运算处理环形索引：`(pos + 1) % PIPE_BUFFER_SIZE`
- 在写入时，如果 `head == tail` 且 `count > 0`，说明缓冲区满
- 在读取时，如果 `head == tail` 且 `count == 0`，说明缓冲区空
- 使用 `count` 字段区分满和空的状态

#### 4.4 引用计数管理

**问题：** 多个进程可能共享同一管道的读端或写端，需要正确管理生命周期。

**解决方案：**
- 使用 `read_ref` 和 `write_ref` 分别记录读端和写端的引用计数
- 在 `pipe_close` 中减少相应的引用计数
- 当 `read_ref == 0` 时，所有写操作应返回 EPIPE 错误
- 当 `write_ref == 0` 时，读操作在缓冲区为空后应返回 0（EOF）
- 只有当 `read_ref == 0 && write_ref == 0` 时，才释放管道资源

#### 4.5 死锁预防

**问题：** 多个进程同时等待可能导致死锁。

**解决方案：**
- 在阻塞等待前必须释放锁，避免持有锁时阻塞
- 使用等待队列的原子操作，确保唤醒和等待的正确性
- 避免嵌套锁，管道操作只使用一个锁

### 5. Linux 实现参考

#### 5.1 Linux 内核中的管道实现

Linux 内核中管道的主要数据结构是 `pipe_inode_info`，包含：

- **缓冲区：** 使用 `pipe_buffer` 结构数组，支持多页缓冲区
- **同步机制：** 使用 `wait_queue_head_t` 等待队列和自旋锁
- **引用计数：** 使用 `readers` 和 `writers` 字段

#### 5.2 设计差异

- **缓冲区管理：** Linux 使用页缓存机制，支持更大的数据量；ucore 可以使用固定大小的环形缓冲区
- **等待机制：** Linux 使用内核等待队列；ucore 可以使用信号量和等待队列的组合
- **文件系统集成：** Linux 管道通过特殊的 inode 实现；ucore 可以在现有文件系统框架上扩展

### 6. 总结

1. **数据结构：** 核心是 `pipe` 结构体，包含环形缓冲区、同步机制和状态信息
2. **接口设计：** 提供 `sys_pipe`、`pipe_read`、`pipe_write`、`pipe_close` 等标准接口
3. **同步互斥：** 使用自旋锁保证互斥，使用信号量和等待队列实现同步
4. **边界处理：** 正确处理缓冲区满/空、管道端关闭等边界情况
5. **资源管理：** 通过引用计数正确管理管道的生命周期

通过以上设计，可以在 ucore 中实现功能完整、安全可靠的 UNIX 管道机制。

## 扩展练习 Challenge2：完成基于"UNIX的软连接和硬连接机制"的设计方案

### 1. 需求概述与 Linux 参考

- **硬链接**：多个目录项直接引用同一个 inode，共享数据块与权限；仅允许指向同一文件系统内的普通文件。参考 Linux VFS 中的 `i_nlink` 语义（fs/namei.c）以管理引用计数。
- **软链接（符号链接）**：特殊文件，数据块中保存目标路径字符串；可以跨文件系统及指向不存在的文件。Linux 使用 `S_IFLNK` 类型并在路径解析阶段调用 `link_path_walk()` 跟踪链接，本设计沿用“延迟解析 + 最大深度”策略。
- **一致性目标**：所有链接操作必须保持目录项、inode 和页缓存三者一致，并正确处理并发修改、循环引用以及异常回滚。

### 2. 数据结构设计

```c
/* 目录项扩展：记录类型和链接元信息 */
struct sfs_dentry {
    char name[SFS_MAXNAMELEN];
    uint32_t ino;
    uint16_t type;        /* SFS_DT_REG / DIR / LNK */
    uint16_t link_kind;   /* SFS_LINK_NONE / HARD / SOFT */
};

/* inode 扩展：兼容硬/软链接语义 */
struct sfs_inode {
    uint32_t ino;
    uint32_t mode;        /* 含 S_IFLNK 标志 */
    uint32_t nlink;       /* 硬链接计数，语义同 Linux i_nlink */
    off_t size;
    uint32_t blocks[SFS_NDIRECT + SFS_NINDIRECT];
    struct semaphore i_sem;   /* 保护 inode 级别的互斥 */
    union {
        struct {
            uint32_t target_ino;   /* 硬链接共享 inode，无需额外字段 */
        } hard;
        struct {
            uint16_t path_len;
            char target_path[SFS_MAX_SYMLINK_LEN];
        } soft;
    } link_info;
};

/* 名字解析上下文：跟踪软链接深度与缓存 */
struct link_walk_ctx {
    struct inode *cwd;
    int depth;                 /* 防止循环，<= SFS_MAX_SYMLINKS */
    struct inode *stack[SFS_MAX_SYMLINKS];
};
```

设计要点：

- 硬链接仅修改 `nlink` 并复用原数据块；软链接设置 `mode=S_IFLNK`，`size=path_len`，并把目标路径存入数据块或 `target_path` 内联缓存（短链接快捷路径，类似 Linux fast symlink）。
- `link_walk_ctx` 在路径解析阶段传递，用于检测递归或深度超限（默认 $SFS\_MAX\_SYMLINKS=8$）。
- `i_sem` 保护 `nlink`、`mode`、`size` 等字段；目录操作使用 `dir->lock`（读写锁）配合完成。

### 3. 接口与语义

1. `int sys_link(const char *oldpath, const char *newpath);`
   - 语义：创建硬链接；解析 `oldpath` 得到目标 inode，验证类型与跨 FS 限制，调用 `sfs_link_inc()` 更新 `nlink`，向新目录写入指向同一 inode 的 `sfs_dentry`。
2. `int sys_symlink(const char *target, const char *linkpath);`
   - 语义：创建软链接；分配新 inode、标记 `S_IFLNK`，把 `target` 写入链接 inode 的数据块或内联缓存，并在父目录插入 `linkpath` 目录项。
3. `ssize_t sys_readlink(const char *path, char *buf, size_t bufsz);`
   - 语义：读取软链接内容而不跟随目标，用于用户态解析。
4. `int sys_unlink(const char *path);`
   - 语义：删除目录项；若 inode 为软链接或普通文件则 `sfs_link_dec()`，当 `nlink` 为 0 时回收数据块；目录 unlink 仍需空目录检查。
5. `int sfs_follow_link(struct link_walk_ctx *ctx, struct inode **inode);`
   - 语义：路径解析阶段调用；若命中软链接且 `ctx->depth` 未超限，则解析其 `target_path` 拼接剩余路径并继续 namei；若超限返回 `-ELOOP`。
6. `int sfs_link_inc(struct sfs_inode *inode); / int sfs_link_dec(struct sfs_inode *inode);`
   - 语义：封装 `nlink` 的安全增减，包含日志记录与崩溃回滚；当 `nlink` 变为 0 时触发 `sfs_truncate()` 回收数据块。

接口实现需与现有 VFS 钩子（`inode_ops->link`, `inode_ops->unlink`, `inode_ops->follow_link`）对齐，方便后续扩展到多种文件系统。

### 4. 同步互斥与一致性处理

- **目录与 inode 双锁顺序**：遵循“父目录锁 → 子 inode `i_sem`”的顺序，避免死锁；对于跨目录的硬链接，需要对两个目录加有序锁，借鉴 Linux `lock_two_inodes()` 的编号排序策略。
- **引用计数更新**：`nlink` 操作在持有 `i_sem` 的情况下进行，并通过日志（或 write-ahead redo 记录）确保失败回滚；对于软链接，仅在创建和删除时影响引用计数。
- **路径解析并发**：`link_walk_ctx` 访问 symlink 数据时，需要短期持有 inode 的页缓存锁，防止同时写（理论上 symlink 内容只在创建时写入，后续只读，可采用 `RCU + seqlock` 方案，但 ucore 中先用自旋锁即可）。
- **循环与深度限制**：解析时递增 `ctx->depth` 并记录访问过的 inode 指针，若同一 inode 再次出现或深度超过阈值，返回 `-ELOOP` 防止死循环。
- **并发 unlink/link**：目录项写操作应复用 SFS 的日志或双写机制，确保在崩溃后 `nlink` 与目录项状态一致；若在 `sys_link` 过程中目标 inode 被删除，需要使用引用计数（`inode->ref`）保证 inode 在事务结束前不会被回收。

通过以上数据结构与接口设计，并参考 Linux VFS 在 `fs/namei.c`、`fs/ext2/namei.c` 中的实现策略，可以在 ucore 中加入行为与 POSIX 兼容的软/硬链接支持，同时在多核场景下保证同步互斥与文件系统元数据的一致性。
