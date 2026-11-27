#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// Set the calling process's trace mask; zero disables tracing.
uint64
sys_trace(void)
{
  int mask;

  argint(0, &mask);
  myproc()->tracemask = mask;
  return 0;
}

uint64
sys_sysinfo(void)
{
  uint64 addr;

  argaddr(0, &addr);
  if(addr == 0)
    return -1;
  return getsysinfo(addr);
}

uint64
sys_pgaccess(void)
{
  uint64 addr, result;
  int n;
  uint mask = 0;
  pte_t *ptes[32];
  struct proc *p = myproc();

  argaddr(0, &addr);
  argint(1, &n);
  argaddr(2, &result);
  addr = PGROUNDDOWN(addr);
  if(n < 0 || n > 32 || addr >= MAXVA ||
     (uint64)n > (MAXVA - addr) / PGSIZE)
    return -1;

  for(int i = 0; i < n; i++){
    ptes[i] = walk(p->pagetable, addr + i * PGSIZE, 0);
    if(ptes[i] == 0 || (*ptes[i] & (PTE_V | PTE_U)) != (PTE_V | PTE_U))
      return -1;
    if(*ptes[i] & PTE_A)
      mask |= 1U << i;
  }

  // Keep access bits intact if copying the result fails.
  if(copyout(p->pagetable, result, (char *)&mask, sizeof(mask)) < 0)
    return -1;
  // userret flushes the TLB before user mode resumes.
  for(int i = 0; i < n; i++)
    *ptes[i] &= ~PTE_A;
  return 0;
}
