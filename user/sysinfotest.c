#include "kernel/types.h"
#include "kernel/riscv.h"
#include "kernel/sysinfo.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// Dung bien toan cuc (dat trong BSS) de tranh tran user stack (xv6 user stack chi co 4KB)
static struct sysinfo g_info;
static struct sysinfo g_info2;

void
sinfo(struct sysinfo *info) {
  if (sysinfo(info) < 0) {
    printf("FAIL: sysinfo failed\n");
    exit(1);
  }
}

// In chi tiet danh sach cac file va process de dap ung yeu cau "list of them" trong de bai
void
print_sysinfo(struct sysinfo *info) {
  printf("--- SYSINFO: %d open files, %ld active processes ---\n", info->nfile, info->nproc);
  for(int i = 0; i < 100; i++) {
    if(info->files[i].valid) {
      printf("  [File %d] type=%d ref=%d r=%d w=%d | pids:",
             i, info->files[i].ftype, info->files[i].ref,
             info->files[i].readable, info->files[i].writable);
      for(int k = 0; k < info->files[i].npid; k++)
        printf(" %d", info->files[i].pids[k]);
      printf("\n");
    }
  }
  for(int i = 0; i < 64; i++) {
    if(info->procs[i].valid) {
      printf("  [Proc %d] ppid=%d state=%d sched=%d | fds:",
             info->procs[i].pid, info->procs[i].ppid,
             info->procs[i].state, info->procs[i].sched_count);
      for(int k = 0; k < info->procs[i].nfd; k++)
        printf(" %d", info->procs[i].fds[k]);
      printf("\n");
    }
  }
}

void
testcall(void) {
  if (sysinfo(&g_info) < 0) {
    printf("FAIL: sysinfo failed\n");
    exit(1);
  }

  if (sysinfo((struct sysinfo *) 0xeaeb0b5b00002f5e) !=  0xffffffffffffffff) {
    printf("FAIL: sysinfo succeeded with bad argument\n");
    exit(1);
  }
}

void
testbad(void) {
  int pid = fork();
  int xstatus;
  
  if(pid < 0){
    printf("sysinfotest: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    // truyen con tro null phai bi tu choi hoac kill
    if(sysinfo(0) < 0)
      exit(0);
    exit(1);
  }
  wait(&xstatus);
  if(xstatus != 0) {
    printf("sysinfotest: testbad failed\n");
    exit(1);
  }
}

void
testproc(void) {
  uint64 nproc;
  int status;
  int pid;
  
  sinfo(&g_info);
  nproc = g_info.nproc;

  pid = fork();
  if(pid < 0){
    printf("sysinfotest: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    sinfo(&g_info);
    if(g_info.nproc != nproc+1) {
      printf("sysinfotest: FAIL nproc is %ld instead of %ld\n", g_info.nproc, nproc+1);
      exit(1);
    }
    exit(0);
  }
  wait(&status);
  sinfo(&g_info);
  if(g_info.nproc != nproc) {
    printf("sysinfotest: FAIL nproc is %ld instead of %ld\n", g_info.nproc, nproc);
    exit(1);
  }
}

void
test_file_process(void) {
  printf("sysinfotest: starting file & process management tests\n");

  sinfo(&g_info);
  print_sysinfo(&g_info);

  // 1. Test open & dup
  int fd = open("README", O_RDONLY);
  if(fd < 0){
    printf("FAIL: open README failed\n");
    exit(1);
  }
  int fd2 = dup(fd);
  sinfo(&g_info2);

  if(g_info2.nfile != g_info.nfile + 1) {
    printf("FAIL: nfile did not increase by 1 after open\n");
    exit(1);
  }

  // Kiem tra ref count cua file README vua dup tang len 2
  int found_dup = 0;
  for(int i = 0; i < 100; i++) {
    if(g_info2.files[i].valid && g_info2.files[i].ref >= 2) {
      found_dup = 1;
      break;
    }
  }
  if(!found_dup) {
    printf("FAIL: ref count >= 2 not found after dup\n");
    exit(1);
  }

  close(fd2);
  close(fd);

  // 2. Test pipe
  int pfd[2];
  pipe(pfd);
  sinfo(&g_info2);
  if(g_info2.nfile != g_info.nfile + 2) {
    printf("FAIL: nfile did not increase by 2 after pipe\n");
    exit(1);
  }
  close(pfd[0]);
  close(pfd[1]);

  // 3. Test fork, file sharing & sched_count
  int parent_pid = getpid();
  int test_fd = open("README", O_RDONLY);

  sinfo(&g_info);
  int pid = fork();
  if(pid == 0) {
    // Child: chay mot luc de tang sched_count
    for(volatile int i = 0; i < 3000000; i++);
    exit(0);
  }

  sleep(2);
  sinfo(&g_info2);

  // Kiem tra danh sach pids cua file README chua ca cha va con
  int found_both = 0;
  for(int i = 0; i < 100; i++) {
    if(g_info2.files[i].valid && g_info2.files[i].ref >= 2) {
      int has_p = 0, has_c = 0;
      for(int k = 0; k < g_info2.files[i].npid; k++) {
        if(g_info2.files[i].pids[k] == parent_pid) has_p = 1;
        if(g_info2.files[i].pids[k] == pid) has_c = 1;
      }
      if(has_p && has_c) {
        found_both = 1;
        break;
      }
    }
  }
  if(!found_both) {
    printf("FAIL: file pids do not contain both parent and child\n");
    exit(1);
  }

  // Kiem tra sched_count cua process con > 0
  int child_sched_ok = 0;
  for(int i = 0; i < 64; i++) {
    if(g_info2.procs[i].valid && g_info2.procs[i].pid == pid) {
      if(g_info2.procs[i].sched_count > 0)
        child_sched_ok = 1;
      break;
    }
  }
  if(!child_sched_ok) {
    printf("FAIL: child sched_count was not incremented\n");
    exit(1);
  }

  wait(0);
  close(test_fd);

  printf("sysinfotest: file & process management tests OK\n");
}

int
main(int argc, char *argv[])
{
  printf("sysinfotest: start\n");
  testbad();
  testcall();
  testproc();
  test_file_process();
  printf("sysinfotest: OK\n");
  exit(0);
}
