#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "sysinfo.h"

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

extern struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

extern struct proc proc[];
extern struct spinlock wait_lock;

uint64
sys_sysinfo(void)
{
  uint64 addr;
  argaddr(0, &addr);

  struct proc *curp = myproc();

  // Validate pointer early: reject null pointer, overflow, or out of process memory bounds
  if(addr == 0 || addr >= curp->sz || addr + sizeof(struct sysinfo) > curp->sz || addr + sizeof(struct sysinfo) < addr)
    return -1;

  uint64 freemem_addr = addr + (uint64)&((struct sysinfo *)0)->freemem;
  uint64 nproc_addr   = addr + (uint64)&((struct sysinfo *)0)->nproc;
  uint64 nfile_addr   = addr + (uint64)&((struct sysinfo *)0)->nfile;
  uint64 files_base   = addr + (uint64)&((struct sysinfo *)0)->files[0];
  uint64 procs_base   = addr + (uint64)&((struct sysinfo *)0)->procs[0];

  int nfile = 0;

  for(int i = 0; i < NFILE; i++){
    struct sysinfo_file entry;
    entry.valid    = 0;
    entry.ftype    = 0;
    entry.readable = 0;
    entry.writable = 0;
    entry.ref      = 0;
    entry.npid     = 0;
    for(int k = 0; k < SYSINFO_MAX_PIDS; k++) entry.pids[k] = 0;

    acquire(&ftable.lock);
    struct file *f = &ftable.file[i];
    if(f->ref > 0){
      nfile++;
      entry.valid    = 1;
      entry.ftype    = (int)f->type;
      entry.readable = f->readable;
      entry.writable = f->writable;
      entry.ref      = f->ref;
    }
    release(&ftable.lock);

    if(entry.valid){
      for(struct proc *p = proc; p < &proc[NPROC]; p++){
        for(int fd = 0; fd < NOFILE; fd++){
          if(p->ofile[fd] == f){
            if(entry.npid < SYSINFO_MAX_PIDS){
              entry.pids[entry.npid++] = p->pid;
            }
            break;
          }
        }
      }
    }

    uint64 dst = files_base + (uint64)i * sizeof(struct sysinfo_file);
    if(copyout(curp->pagetable, dst, (char*)&entry, sizeof(struct sysinfo_file)) < 0)
      return -1;
  }

  if(copyout(curp->pagetable, nfile_addr, (char*)&nfile, sizeof(int)) < 0)
    return -1;

  uint64 nproc = 0;

  for(int i = 0; i < NPROC; i++){
    struct proc *p = &proc[i];
    struct sysinfo_proc entry;
    entry.valid       = 0;
    entry.pid         = 0;
    entry.ppid        = 0;
    entry.state       = 0;
    entry.sched_count = 0;
    entry.nfd         = 0;
    for(int k = 0; k < SYSINFO_MAX_FDS; k++) entry.fds[k] = 0;

    acquire(&p->lock);
    if(p->state != UNUSED){
      nproc++;
      entry.valid       = 1;
      entry.pid         = p->pid;
      entry.state       = (int)p->state;
      entry.sched_count = p->sched_count;
      for(int fd = 0; fd < NOFILE; fd++){
        if(p->ofile[fd] != 0 && entry.nfd < SYSINFO_MAX_FDS){
          entry.fds[entry.nfd++] = fd;
        }
      }
      struct proc *par = p->parent;
      release(&p->lock);

      if(par != 0){
        acquire(&wait_lock);
        entry.ppid = par->pid;
        release(&wait_lock);
      }
    } else {
      release(&p->lock);
    }

    uint64 dst = procs_base + (uint64)i * sizeof(struct sysinfo_proc);
    if(copyout(curp->pagetable, dst, (char*)&entry, sizeof(struct sysinfo_proc)) < 0)
      return -1;
  }

  if(copyout(curp->pagetable, nproc_addr, (char*)&nproc, sizeof(uint64)) < 0)
    return -1;

  uint64 freemem = 0;
  if(copyout(curp->pagetable, freemem_addr, (char*)&freemem, sizeof(uint64)) < 0)
    return -1;

  return 0;
}
