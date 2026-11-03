// user/sysinfotest.c
// Test suite đầy đủ cho syscall sysinfo().
// Mỗi test dùng mẫu: gọi sysinfo trước → thao tác → gọi sysinfo sau → so sánh.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/sysinfo.h"
#include "user/user.h"
#include "kernel/fcntl.h"

// ----------------------------------------------------------------
// Hàm tiện ích
// ----------------------------------------------------------------

static int total_pass = 0;
static int total_fail = 0;

static void
test_result(const char *name, int cond)
{
  if(cond){
    printf("  [PASS] %s\n", name);
    total_pass++;
  } else {
    printf("  [FAIL] %s\n", name);
    total_fail++;
  }
}

// Tìm entry file trong sysinfo theo type và ref; trả index hoặc -1
static int
find_file(struct sysinfo *si, int ftype, int ref)
{
  for(int i = 0; i < 100; i++){
    if(si->files[i].valid && si->files[i].ftype == ftype && si->files[i].ref == ref)
      return i;
  }
  return -1;
}

// Tìm entry proc theo pid; trả index hoặc -1
static int
find_proc(struct sysinfo *si, int pid)
{
  for(int i = 0; i < 64; i++){
    if(si->procs[i].valid && si->procs[i].pid == pid)
      return i;
  }
  return -1;
}

// Đếm số file entry hợp lệ trong sysinfo (= nfile)
static int
count_valid_files(struct sysinfo *si)
{
  int n = 0;
  for(int i = 0; i < 100; i++)
    if(si->files[i].valid) n++;
  return n;
}

// Kiểm tra pid có xuất hiện trong danh sách pids của file entry không
static int
file_has_pid(struct sysinfo *si, int fidx, int pid)
{
  struct sysinfo_file *fe = &si->files[fidx];
  for(int k = 0; k < fe->npid; k++)
    if(fe->pids[k] == pid) return 1;
  return 0;
}

// ----------------------------------------------------------------
// Test 1: Baseline — phải thấy init (pid 1) và sh (pid 2)
// ----------------------------------------------------------------
static void
test1_baseline(void)
{
  printf("\n[Test 1] Baseline: init, sh, sysinfotest\n");
  struct sysinfo si;
  if(sysinfo(&si) < 0){
    printf("  sysinfo FAILED\n");
    total_fail++;
    return;
  }

  int mypid = getpid();

  test_result("nproc >= 3 (init + sh + self)",  si.nproc >= 3);
  test_result("nfile > 0",                       si.nfile > 0);
  test_result("find init (pid=1)",               find_proc(&si, 1) >= 0);
  test_result("find sh   (pid=2)",               find_proc(&si, 2) >= 0);
  test_result("find self",                       find_proc(&si, mypid) >= 0);

  // nfile == count_valid_files
  test_result("nfile matches valid file entries", si.nfile == count_valid_files(&si));
}

// ----------------------------------------------------------------
// Test 2: open một file — số file +1, ref = 1
// ----------------------------------------------------------------
static void
test2_open(void)
{
  printf("\n[Test 2] open() tăng nfile +1, ref=1\n");
  struct sysinfo before, after;
  if(sysinfo(&before) < 0){ printf("  sysinfo FAILED\n"); total_fail++; return; }

  int fd = open("/README", O_RDONLY);
  if(fd < 0){ printf("  open FAILED (no /README?)\n"); total_fail++; return; }

  if(sysinfo(&after) < 0){ close(fd); printf("  sysinfo FAILED\n"); total_fail++; return; }

  test_result("nfile increased by 1",   after.nfile == before.nfile + 1);

  // Tìm file mới với ref=1, type=FD_INODE(2)
  int fidx = find_file(&after, 2, 1);
  test_result("new file entry exists (type=INODE, ref=1)", fidx >= 0);
  if(fidx >= 0){
    test_result("self pid in file's pids list", file_has_pid(&after, fidx, getpid()));
  }

  close(fd);

  struct sysinfo restored;
  sysinfo(&restored);
  test_result("nfile restored after close", restored.nfile == before.nfile);
}

// ----------------------------------------------------------------
// Test 3: O_RDONLY / O_WRONLY / O_RDWR — mode đúng
// ----------------------------------------------------------------
static void
test3_mode(void)
{
  printf("\n[Test 3] open() mode RDONLY/WRONLY/RDWR\n");
  struct sysinfo si;

  // RDONLY
  int fd = open("/README", O_RDONLY);
  if(fd < 0){ printf("  open RDONLY FAILED\n"); total_fail++; goto wronly; }
  sysinfo(&si);
  {
    int fidx = find_file(&si, 2, 1);
    if(fidx >= 0){
      test_result("RDONLY: readable=1, writable=0",
                  si.files[fidx].readable == 1 && si.files[fidx].writable == 0);
    } else {
      printf("  [FAIL] RDONLY entry not found\n"); total_fail++;
    }
  }
  close(fd);

wronly:
  // WRONLY — ghi vào file tạm
  fd = open("testfile_w", O_WRONLY | O_CREATE);
  if(fd < 0){ printf("  open WRONLY FAILED\n"); total_fail++; goto rdwr; }
  sysinfo(&si);
  {
    int fidx = find_file(&si, 2, 1);
    if(fidx >= 0){
      test_result("WRONLY: readable=0, writable=1",
                  si.files[fidx].readable == 0 && si.files[fidx].writable == 1);
    } else {
      printf("  [FAIL] WRONLY entry not found\n"); total_fail++;
    }
  }
  close(fd);

rdwr:
  // RDWR
  fd = open("testfile_w", O_RDWR);
  if(fd < 0){ printf("  open RDWR FAILED\n"); total_fail++; return; }
  sysinfo(&si);
  {
    int fidx = find_file(&si, 2, 1);
    if(fidx >= 0){
      test_result("RDWR: readable=1, writable=1",
                  si.files[fidx].readable == 1 && si.files[fidx].writable == 1);
    } else {
      printf("  [FAIL] RDWR entry not found\n"); total_fail++;
    }
  }
  close(fd);
  unlink("testfile_w");
}

// ----------------------------------------------------------------
// Test 4: dup — số file không đổi, ref +1
// ----------------------------------------------------------------
static void
test4_dup(void)
{
  printf("\n[Test 4] dup() — nfile không đổi, ref tăng\n");
  struct sysinfo before, after;

  int fd = open("/README", O_RDONLY);
  if(fd < 0){ printf("  open FAILED\n"); total_fail++; return; }

  sysinfo(&before);
  int fd2 = dup(fd);
  sysinfo(&after);

  test_result("nfile unchanged after dup", after.nfile == before.nfile);

  // File trước có ref=1, sau dup có ref=2
  int fidx_b = find_file(&before, 2, 1);
  int fidx_a = find_file(&after,  2, 2);
  test_result("ref increased to 2 after dup", fidx_b >= 0 && fidx_a >= 0);

  close(fd);
  close(fd2);
}

// ----------------------------------------------------------------
// Test 5: close — nfile giảm về trạng thái trước
// ----------------------------------------------------------------
static void
test5_close(void)
{
  printf("\n[Test 5] close() — nfile trở về baseline\n");
  struct sysinfo baseline, opened, closed;

  sysinfo(&baseline);

  int fd = open("/README", O_RDONLY);
  if(fd < 0){ printf("  open FAILED\n"); total_fail++; return; }
  sysinfo(&opened);

  close(fd);
  sysinfo(&closed);

  test_result("nfile += 1 after open",    opened.nfile == baseline.nfile + 1);
  test_result("nfile -= 1 after close",   closed.nfile == baseline.nfile);
}

// ----------------------------------------------------------------
// Test 6: pipe — +2 file kiểu FD_PIPE
// ----------------------------------------------------------------
static void
test6_pipe(void)
{
  printf("\n[Test 6] pipe() — thêm 2 file type PIPE\n");
  struct sysinfo before, after;
  int fds[2];

  sysinfo(&before);
  pipe(fds);
  sysinfo(&after);

  test_result("nfile += 2 after pipe", after.nfile == before.nfile + 2);

  // Đếm FD_PIPE (type=1)
  int pipe_before = 0, pipe_after = 0;
  for(int i = 0; i < 100; i++){
    if(before.files[i].valid && before.files[i].ftype == 1) pipe_before++;
    if(after.files[i].valid  && after.files[i].ftype  == 1) pipe_after++;
  }
  test_result("2 new PIPE entries", pipe_after == pipe_before + 2);

  close(fds[0]);
  close(fds[1]);
}

// ----------------------------------------------------------------
// Test 7: fork — số proc +1, file ref tăng, cả cha lẫn con thấy nhau
// ----------------------------------------------------------------
static void
test7_fork(void)
{
  printf("\n[Test 7] fork() — nproc +1, ref file tăng\n");
  struct sysinfo before, after;

  // Mở một file để kiểm tra ref tăng sau fork
  int fd = open("/README", O_RDONLY);
  if(fd < 0){ printf("  open FAILED\n"); total_fail++; return; }

  sysinfo(&before);
  int parentpid = getpid();

  int pid = fork();
  if(pid == 0){
    // Con: gọi sysinfo và thoát
    struct sysinfo child_si;
    sysinfo(&child_si);
    // Kiểm tra trong con: thấy cha
    int found_parent = find_proc(&child_si, parentpid) >= 0;
    // Báo kết quả qua exit code (1 = ok, 0 = fail)
    exit(found_parent ? 0 : 1);
  }

  // Cha: chờ đủ để con đã được schedule, rồi gọi sysinfo
  // (sleep ngắn để con fork xong)
  sleep(1);
  sysinfo(&after);

  int child_exit;
  wait(&child_exit);

  test_result("nproc += 1 after fork (con còn sống)",   after.nproc >= before.nproc + 1);
  test_result("find child pid in sysinfo",               find_proc(&after, pid) >= 0);
  test_result("child sees parent (exit 0)",              child_exit == 0);

  // ref của file /README phải tăng (cha + con đều mở)
  int fidx_b = -1, fidx_a = -1;
  // Trước fork: tìm entry có ref=1 và self pid
  for(int i = 0; i < 100; i++){
    if(before.files[i].valid && before.files[i].ftype == 2 &&
       file_has_pid(&before, i, parentpid)){
      fidx_b = i;
      break;
    }
  }
  // Sau fork: ref tăng lên 2
  for(int i = 0; i < 100; i++){
    if(after.files[i].valid && after.files[i].ftype == 2 && after.files[i].ref >= 2){
      fidx_a = i;
      break;
    }
  }
  test_result("file ref >= 2 after fork", fidx_b >= 0 && fidx_a >= 0);

  close(fd);
}

// ----------------------------------------------------------------
// Test 8: Con exit, cha wait — nproc trở về
// ----------------------------------------------------------------
static void
test8_wait(void)
{
  printf("\n[Test 8] child exit + wait — nproc trở về\n");
  struct sysinfo before, after_fork, after_wait;

  sysinfo(&before);

  int pid = fork();
  if(pid == 0){
    sleep(2);
    exit(0);
  }

  sleep(1);
  sysinfo(&after_fork);

  int xstate;
  wait(&xstate);
  sysinfo(&after_wait);

  test_result("nproc +1 after fork",      after_fork.nproc >= before.nproc + 1);
  test_result("nproc restored after wait", after_wait.nproc == before.nproc);
  test_result("child gone from sysinfo",   find_proc(&after_wait, pid) < 0);
}

// ----------------------------------------------------------------
// Test 9: sched_count tăng sau vòng lặp CPU-bound
// ----------------------------------------------------------------
static void
test9_schedcount(void)
{
  printf("\n[Test 9] sched_count tăng sau CPU-busy loop\n");
  struct sysinfo before, after;
  int mypid = getpid();

  sysinfo(&before);
  int idx_b = find_proc(&before, mypid);

  // Vòng lặp tốn CPU để bị yield nhiều lần
  volatile long sum = 0;
  for(long i = 0; i < 5000000L; i++) sum += i;

  sysinfo(&after);
  int idx_a = find_proc(&after, mypid);

  if(idx_b < 0 || idx_a < 0){
    printf("  [FAIL] self not found in sysinfo\n"); total_fail++; return;
  }

  int sc_b = before.procs[idx_b].sched_count;
  int sc_a = after.procs[idx_a].sched_count;
  printf("    sched_count before=%d, after=%d\n", sc_b, sc_a);
  test_result("sched_count increased", sc_a > sc_b);
}

// ----------------------------------------------------------------
// Test 10: Con trỏ sai — trả -1, kernel không crash
// ----------------------------------------------------------------
static void
test10_badptr(void)
{
  printf("\n[Test 10] Bad pointer — sysinfo trả -1\n");
  int ret = sysinfo((struct sysinfo*)0xFFFFFFFFFFFFF000UL);
  test_result("sysinfo returns -1 for bad ptr", ret == -1);

  // Kernel vẫn còn sống
  struct sysinfo si;
  ret = sysinfo(&si);
  test_result("sysinfo still works after bad ptr", ret == 0);
}

// ----------------------------------------------------------------
// main
// ----------------------------------------------------------------
int
main(void)
{
  printf("=== sysinfotest start ===\n");

  test1_baseline();
  test2_open();
  test3_mode();
  test4_dup();
  test5_close();
  test6_pipe();
  test7_fork();
  test8_wait();
  test9_schedcount();
  test10_badptr();

  printf("\n=== Kết quả: %d PASS, %d FAIL ===\n", total_pass, total_fail);

  if(total_fail == 0)
    printf("ALL TESTS PASSED\n");
  else
    printf("SOME TESTS FAILED\n");

  exit(0);
}

