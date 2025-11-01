# 练习1：完善中断处理 

## 1.编程实现过程：

### 任务：
我们的主要编程工作是完善时钟中断（case IRQ_S_TIMER）部分，需要完成
- 设置下次时钟中断- clock_set_next_event()
- 计数器（ticks）加一
- 当计数器加到100的时候，我们会输出一个`100ticks`表示我们触发了100次时钟中断，同时打印次数（num）加一
- 判断打印次数，当打印次数为10时，调用<sbi.h>中的关机函数关机

### 主要操作：
- 因为OpenSBI提供的接口一次只能设置一个时钟中断事件，所以在发生时钟中断时，我们需要通过clock_set_next_event()设置下次时钟中断。
- 每次发生时钟中断ticks++。
- 每次中断时判断 ticks % TICK_NUM == 0的真值，为真时，通过print_ticks()输出`100ticks`，同时num++。
- 当num >= 10时通过<sbi.h>中的关机函数sbi_shutdown()关机。
完整代码如下
```
        case IRQ_S_TIMER:
       	    clock_set_next_event();
            if (++ticks % TICK_NUM == 0) {
                print_ticks();
                if (++num >= 10) {
                    sbi_shutdown();
                }
            }
            break;
```
## 2.时钟中断流程：

### 核心数据结构
在分析时钟中断流程前，需要先了解trapframe数据结构

trapframe：
- gpr 存放32个通用寄存器，用于恢复。
- status 保存 CPU 的状态信息：
  - 中断使能位​​（SIE）：记录中断是否被允许。
  - ​​特权级信息​​（SPP）：记录中断前是用户态还是内核态。
- epc 保存中断发生时的pc值。
- badvaddr 提供与异常相关的​​附加信息​​，帮助内核确定异常的具体原因。
- cause 中断或异常的类型与原因：
  - 最高位：1=中断，0=异常。
  - 低位：具体原因代码。

### 调用流程：
- idt_init(void)函数设置了，发生中断或异常时统一跳转到__alltraps入口。
- 进入__alltraps入口后先通过SAVE_ALL获取中断前的信息，然后mov a0，sp（目的在challenge1里说明），最后jal trap，跳转到trap(struct trapframe *tf)函数。
- trap(struct trapframe *tf)函数调用trap_dispatch(struct trapframe *tf)函数，trap_dispatch(struct trapframe *tf)函数又根据tf->cause判断是中断还是异常，如果是中断则调用interrupt_handler(tf)函数来处理中断。
- interrupt_handler(tf)函数又根据(tf->cause << 1) >> 1;来判断中断类型，若为5则是S模式的时钟中断，然后便进入我们上面实现的时钟中断代码。

---

# challenge1：描述与理解中断流程

##中断流程：
- 中断产生，cpu会直接跳转到stevc寄存器指向的位置。
- 在idt_init(void)函数里设置了stevc的值为&__alltraps，中断发生时会直接跳转到__alltraps。
- 进入__alltraps入口后先通过SAVE_ALL获取中断前的信息，包括32个通用寄存器，中断发生时的pc值和中断原因等，把中断前的信息按 trapframe 的布局保存到当前栈。
  - 其中SAVE_ALL中寄寄存器保存在栈中的位置是根据 sp 位置确定的，先执行 addi sp, sp, -36REGBYTES 分配 36 个槽位，sp在栈的最底端，通过 STORE x, N*REGBYTES(sp) 分配槽位，其中N*REGBYTES是偏移量。
  - 当前实现的是“保存全部通用寄存器 + 关键 CSR”，属于保存所有寄存器，优点是简单且从中断里恢复是比较安全。
  - 也可以实现只保留部分寄存器，即只保留和当前有关有效的寄存器，这样更为高效，但判断保留哪些寄存器和调整 tf 数据结构的逻辑较为复杂。
- 接着就是move a0, sp，把把当前的栈指针值复制到寄存器 a0。
  - 目的：由RISCV汇编的通用寄存器别名和含义可知，a0 用作第一个函数参数。sp 指向的是 trapframe 的起始地址，把 sp 放到 a0 就是把 struct trapframe * 作为参数传给后面的 trap() 函数，让 trap(struct trapframe *tf) 能访问刚保存的异常上下文。
- 然后jal trap，跳转到trap(struct trapframe *tf)函数，trap(struct trapframe *tf)函数又了调用trap_dispatch(struct trapframe *tf)函数是实际上的异常中断处理函数。
- trap_dispatch(struct trapframe *tf)函数会根据tf->cause判断是中断还是异常。
  - 中断则使用 interrupt_handler(struct trapframe *tf) 函数。
  - 异常则使用 exception_handler(struct trapframe *tf) 函数。
  - 两个函数都会根据 tf->cause 的值判断中断或异常的类型，然后再采取对应的措施。
- 处理异常或中断后，trap() 返回，进入__trapret，RESTORE_ALL会把 trapframe 中保存的信息写回， sp 回到中断前的值。
- 最后执行特权返回指令 sret，sret 会读取 sstatus 中的 SPP 位并切换到以前的特权级，CPU 从中断切换回被中断的执行流，继续执行。

---

# 扩展练习Challenge2 ：理解上下文切换机制

## 1.csrw sscratch, sp；csrrw s0, sscratch, x0实现的操作和目的

首先我们先分析csrw sscratch, sp 。这条操作将当前的栈指针sp写入了CSR寄存器sscratch。而在RISC-V的异常处理机制中，sscratch是一个“陷入前的临时保存寄存器”，trap 向量代码通常用它来区分 trap 是从用户态（user mode）还是从内核态（supervisor mode）发生的。当 trap 从用户态进入内核态时，sscratch 通常事先设为 0，这样陷入处理例程可以知道是从用户态进入的。当 trap 发生在内核态时，此时 sscratch 可能保存内核栈指针或其他上下文。

所以这行代码的作用是暂时把当前栈指针保存到 sscratch，以便在异常入口中有地方保存和区分不同来源的 trap。

然后分析csrrw s0, sscratch, x0。在分析完上一行代码后，这段代码的作用就很显而易见了，首先代码从 sscratch 读出内容（之前写入的 sp）到 s0，同时把x0写入sscratch（x0恒为0）。
翻译成高级语言也就是一段赋值表达式：s0 = sscratch; sscratch = 0。这个操作实现了把陷入前的sp临时保存到s0，同时清空了sscratch，防止在中断嵌套时混淆。

这行代码在实际中的目的是如果在 trap 处理中又发生了新的 trap（比如页错误、定时中断），处理器会再次跳转到陷阱入口。此时通过检查 sscratch 是否为 0，可以区分“trap 来自用户态”（sscratch ≠ 0）还是“trap 来自内核态”（sscratch = 0）。

## 2.保存后不还原的原因和意义

stval，scause等CSR寄存器属于陷入状态寄存器，它们是由硬件自动填充的“异常上下文”，用于告诉内核 trap 发生的原因和位置。而保存他们的目的是为了把硬件自动填写的 trap 原因、出错地址、返回地址等信息 保存到内核栈中，以便之后的 C 函数（如 trap()）可以读取、分析并作出处理。由此可知，我们保存的目的只是为了记录当时出错的状态以便后续的读取和处理，这也就解释了为什么我们不会在restore中恢复他们，因为这些寄存器属于 trap 时刻的状态，是只读信息（或说 trap 现场快照），恢复它们没有意义。而真正有意义且值得恢复的只有sstatus和sepc，这两个寄存器分别决定了返回后是否返回到用户态和返回后的指令地址，恢复他们有助于让系统回到中断发生的位置和状态向后运行。

---

# 扩展练习Challenge3 ：完善异常中断

代码如下：

CAUSE_ILLEGAL_INSTRUCTION中：
```c
        cprintf("Exception type: Illegal instruction\n");
    	cprintf("Illegal instruction caught at 0x%08x\n", tf->epc);
        tf->epc += 4;
        break;
```

CAUSE_BREAKPOINT中
```
        cprintf("Exception type: breakpoint\n");
        cprintf("ebreak caught at 0x%08x\n", tf->epc);
        tf->epc += 2; //ebreak占两个字节
```

在`kern_init`函数中，`intr_enable();`之后写入两行

```c
asm("mret");
asm("ebreak");
```

这两行代码会触发上文中的两个异常，以此来测试。

测试结果如下
```
Exception type: Illegal instruction
Illegal instruction caught at 0xc020009c
Exception type: breakpoint
ebreak caught at 0xc02000a0
```
