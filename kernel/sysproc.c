#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "proc.h"
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


// ---------------------------------------------------------------
// ftable is defined in file.c; we access it via the extern below.
// proc[] is defined in proc.c.
// ---------------------------------------------------------------
extern struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

extern struct proc proc[];
extern struct spinlock wait_lock;

//
// sys_sysinfo: thu thap thong tin file va process trong he thong.
//
// Chien luoc tranh tran stack kernel (4KB):
//   - Moi vong lap chi dat MOT entry nho (struct sysinfo_file ~84B hoac
//     struct sysinfo_proc ~100B) tren stack, copyout ngay, khong giu
//     toan bo mang snapshot.
//   - Khong giu spinlock khi goi copyout (copyout co the page-fault/sleep).
//   - Voi file: acquire ftable.lock -> doc thong tin 1 file -> release ->
//     tim pid (khong can lock vi chi so sanh pointer, ftable.lock da dam bao
//     ref>0 nen file khong bi giai phong) -> copyout entry.
//   - Voi proc: acquire p->lock -> lay state/pid/sched_count/ofile[] ->
//     release -> lay ppid qua wait_lock -> copyout entry.
//
uint64
sys_sysinfo(void)
{
  uint64 addr;
  argaddr(0, &addr);

  struct proc *curp = myproc();

  // Kiem tra con tro hop le bang cach thu copyout 1 byte
  {
    char dummy = 0;
    if(copyout(curp->pagetable, addr, &dummy, 1) < 0)
      return -1;
  }

  // ----------------------------------------------------------------
  // Phan 1: Thong tin file
  // Offset layout cua struct sysinfo:
  //   [0]                        = nfile  (int)
  //   [4]                        = nproc  (int)
  //   [8 .. 8+100*sizeof(sysinfo_file)-1] = files[]
  //   [8+100*sizeof(sysinfo_file) .. ]    = procs[]
  // ----------------------------------------------------------------
  uint64 nfile_addr  = addr;
  uint64 nproc_addr  = addr + sizeof(int);
  uint64 files_base  = addr + sizeof(int) * 2;
  uint64 procs_base  = files_base + (uint64)100 * sizeof(struct sysinfo_file);

  int nfile = 0;

  for(int i = 0; i < NFILE; i++){
    struct sysinfo_file entry;
    entry.valid    = 0;
    entry.ftype    = 0;
    entry.readable = 0;
    entry.writable = 0;
    entry.ref      = 0;
    entry.npid     = 0;
    for(int _k=0;_k<SYSINFO_MAX_PIDS;_k++) entry.pids[_k]=0;

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
      // Tim cac pid dang giu file nay
      // Khong can lock: f->ref > 0 nen file ton tai; chi doc pointer ofile[fd]
      // la atomic tren RISC-V 64-bit alignment. Neu co race, ket qua
      // chi sai mot chut (khong crash), chap nhan duoc trong debug tool.
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

    // copyout entry (khong giu lock)
    uint64 dst = files_base + (uint64)i * sizeof(struct sysinfo_file);
    if(copyout(curp->pagetable, dst, (char*)&entry, sizeof(struct sysinfo_file)) < 0)
      return -1;
  }

  // copyout nfile
  if(copyout(curp->pagetable, nfile_addr, (char*)&nfile, sizeof(int)) < 0)
    return -1;

  // ----------------------------------------------------------------
  // Phan 2: Thong tin process
  // ----------------------------------------------------------------
  int nproc = 0;

  for(int i = 0; i < NPROC; i++){
    struct proc *p = &proc[i];
    struct sysinfo_proc entry;
    entry.valid       = 0;
    entry.pid         = 0;
    entry.ppid        = 0;
    entry.state       = 0;
    entry.sched_count = 0;
    entry.nfd         = 0;
    for(int _k=0;_k<SYSINFO_MAX_FDS;_k++) entry.fds[_k]=0;

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
      // Luu parent pointer de lay ppid sau khi release p->lock
      struct proc *par = p->parent;
      release(&p->lock);

      if(par != 0){
        // Lay wait_lock de doc par->pid an toan
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

  // copyout nproc
  if(copyout(curp->pagetable, nproc_addr, (char*)&nproc, sizeof(int)) < 0)
    return -1;

  return 0;
}
