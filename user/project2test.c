// Project 2 syscall checks: sysinfo/free memory and pgaccess.
// Run project2test in xv6; project2test.py checks vmprint separately from Linux.
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/riscv.h"
#include "kernel/memlayout.h"
#include "kernel/sysinfo.h"
#include "user/user.h"

#define MAXMEM (PHYSTOP - KERNBASE)

// The result spans multiple pages; keep it off the one-page user stack.
static struct sysinfo info;
static char *part;
static int completed;
static int table_open;

// xv6 printf has no field-width support; keep column formatting local to this test.
static void
cell(char *text, int width)
{
  printf("%s", text);
  for(int i = strlen(text); i < width; i++)
    printf(" ");
}

static void
number(long value, int width)
{
  uint64 magnitude = value;
  int digits = value < 0;
  if(value < 0)
    magnitude = 0 - magnitude;
  do {
    digits++;
    magnitude /= 10;
  } while(magnitude);
  for(int i = digits; i < width; i++)
    printf(" ");
  printf("%ld", value);
}

static void
hex(uint value)
{
  int digits = 1;
  for(uint n = value; n >= 16; n /= 16)
    digits++;
  printf("0x");
  for(int i = digits; i < 8; i++)
    printf("0");
  printf("%x", value);
}

static void
comparison(void)
{
  if(!table_open){
    printf("  │ Check / context                                  │   Observed │   Expected │\n");
    printf("  ├──────────────────────────────────────────────────┼────────────┼────────────┤\n");
    table_open = 1;
  }
}

static void
row(char *label)
{
  comparison();
  printf("  │ ");
  cell(label, 48);
  printf(" │ ");
}

static void
record(char *label, long value)
{
  row(label);
  number(value, 10);
  printf(" │          - │\n");
}

static void
check(int ok, char *message)
{
  if(!ok){
    fprintf(2, "  \033[41;97m ✗ \033[0m FAIL [%s]: %s\n", part, message);
    exit(1);
  }
}

static void
group(char *label)
{
  part = label;
  table_open = 0;
  printf("\n%s\n", label);
}

static void
passed(void)
{
  completed++;
  table_open = 0;
  printf("  \033[42;97m ✓ \033[0m PASS\n");
}

static void
note(char *message)
{
  printf("  \033[48;5;208;97m i \033[0m %s\n", message);
}

static void
expect(char *label, long actual, long expected)
{
  row(label);
  number(actual, 10);
  printf(" │ ");
  number(expected, 10);
  printf(" │\n");
  check(actual == expected, label);
}

static void
range(char *label, int actual, int low, int high)
{
  row(label);
  number(actual, 10);
  printf(" │ ");
  number(low, 4);
  printf("..");
  number(high, 4);
  printf(" │\n");
  check(actual >= low && actual <= high, label);
}

static struct procinfo *
findproc(int pid)
{
  for(int i = 0; i < info.nproc; i++)
    if(info.procs[i].pid == pid)
      return &info.procs[i];
  return 0;
}

static struct procinfo *
process(int pid)
{
  struct procinfo *p = findproc(pid);
  check(p != 0, "missing process");
  return p;
}

static void
snapshot(void)
{
  int refs[NFILE] = {0};
  int nfile = 0;

  check(sysinfo(&info) == 0, "valid sysinfo call");
  check(info.nfile >= 0 && info.nfile <= NFILE, "file count bounds");
  check(info.nproc > 0 && info.nproc <= NPROC, "process count bounds");
  check(info.freemem <= MAXMEM && info.freemem % PGSIZE == 0,
        "free memory bounds and page alignment");
  for(int i = 0; i < info.nproc; i++){
    struct procinfo *p = &info.procs[i];
    check(p->pid > 0, "invalid PID");
    check(p->ppid >= 0 && p->ppid != p->pid, "invalid parent PID");
    check(p->state >= USED && p->state <= ZOMBIE, "invalid process state");
    for(int j = 0; j < i; j++)
      check(info.procs[j].pid != p->pid, "duplicate PID");
    for(int fd = 0; fd < NOFILE; fd++){
      int id = p->file_id[fd];
      check(id >= 0 && id <= NFILE, "file ID bounds");
      if(id){
        refs[id - 1]++;
        check(info.files[id - 1].type != FD_NONE, "descriptor has no file type");
      }
    }
  }
  for(int i = 0; i < NFILE; i++){
    struct fileinfo *f = &info.files[i];
    check(f->ref >= refs[i], "reference count below descriptor count");
    if(f->ref == 0)
      continue;
    nfile++;
    check(f->type >= FD_NONE && f->type <= FD_DEVICE, "invalid file type");
    check((f->readable == 0 || f->readable == 1) &&
          (f->writable == 0 || f->writable == 1), "invalid file mode");
  }
  check(info.nfile == nfile, "file total does not match list");
  struct procinfo *self = process(getpid());
  check(self->state == RUNNING && self->nsched > 0, "caller state or schedule count");
}

static int
fileid(int fd)
{
  check(fd >= 0 && fd < NOFILE, "invalid test descriptor");
  int id = process(getpid())->file_id[fd];
  check(id > 0 && id <= NFILE, "missing open descriptor");
  return id;
}

static int
holds(struct procinfo *p, int id)
{
  for(int fd = 0; fd < NOFILE; fd++)
    if(p->file_id[fd] == id)
      return 1;
  return 0;
}

static int
owners(int id)
{
  int n = 0;
  for(int i = 0; i < info.nproc; i++)
    n += holds(&info.procs[i], id);
  return n;
}

static void
testfiles(void)
{
  group("[1.3] open README, dup, independent open, close and access modes");
  char first[3], second[3];
  snapshot();
  int baseline = info.nfile;
  int fd = open("README", O_RDONLY);
  check(fd >= 0, "open README");
  snapshot();
  int id = fileid(fd);
  record("open README: fd", fd);
  record("open README: file ID", id);
  record("open README: type", info.files[id - 1].type);
  record("open README: readable", info.files[id - 1].readable);
  record("open README: writable", info.files[id - 1].writable);
  expect("open objects", info.nfile, baseline + 1);
  check(info.files[id - 1].type == FD_INODE, "new file type");
  expect("new file references", info.files[id - 1].ref, 1);
  check(info.files[id - 1].readable && !info.files[id - 1].writable, "read-only mode");

  int copy = dup(fd);
  check(copy >= 0, "dup README");
  snapshot();
  expect("dup file ID", fileid(copy), id);
  expect("open objects after dup", info.nfile, baseline + 1);
  expect("references", info.files[id - 1].ref, 2);
  expect("unique owner PIDs", owners(id), 1);

  int separate = open("README", O_RDONLY);
  check(separate >= 0, "independent open");
  snapshot();
  record("independent open: file ID (must differ)", fileid(separate));
  record("previous open: file ID", id);
  check(fileid(separate) != id, "open has a separate object");
  expect("open objects", info.nfile, baseline + 2);
  check(read(fd, first, 2) == 2 && read(copy, first + 2, 1) == 1 &&
        read(separate, second, 3) == 3, "read README");
  check(memcmp(first, second, 3) == 0, "dup shares offset; open has its own offset");
  close(fd);
  snapshot();
  expect("closed descriptor mapping", process(getpid())->file_id[fd], 0);
  expect("remaining references", info.files[id - 1].ref, 1);
  expect("read through remaining descriptor", read(copy, first, 1), 1);
  close(copy);
  close(separate);
  snapshot();
  expect("open objects", info.nfile, baseline);

  int modes[] = {O_WRONLY, O_RDWR};
  for(int i = 0; i < 2; i++){
    fd = open("README", modes[i]);
    check(fd >= 0, "open access mode");
    snapshot();
    struct fileinfo *f = &info.files[fileid(fd) - 1];
    record("open mode (O_WRONLY=1, O_RDWR=2)", modes[i]);
    expect("mode: readable", f->readable, i == 1);
    expect("mode: writable", f->writable, 1);
    check(f->writable && f->readable == (i == 1), "write-only or read-write mode");
    close(fd);
  }
  fd = open(".", O_RDONLY);
  check(fd >= 0, "open directory");
  snapshot();
  check(info.files[fileid(fd) - 1].type == FD_INODE, "directory file type");
  close(fd);
  fd = open("console", O_RDWR);
  check(fd >= 0, "open console");
  snapshot();
  struct fileinfo *f = &info.files[fileid(fd) - 1];
  check(f->type == FD_DEVICE && f->readable && f->writable, "device type and mode");
  close(fd);
  snapshot();
  expect("open objects after cleanup", info.nfile, baseline);
  passed();
}

static void
testpipe(void)
{
  group("[1.4] pipe endpoints, byte transfer and cleanup");
  int p[2];
  char c;
  snapshot();
  int baseline = info.nfile;
  check(pipe(p) == 0, "create pipe");
  snapshot();
  int r = fileid(p[0]), w = fileid(p[1]);
  check(r != w, "pipe has two file objects");
  check(info.files[r - 1].type == FD_PIPE && info.files[w - 1].type == FD_PIPE,
        "pipe type");
  check(info.files[r - 1].readable && !info.files[r - 1].writable &&
        !info.files[w - 1].readable && info.files[w - 1].writable, "pipe modes");
  expect("open objects", info.nfile, baseline + 2);
  check(write(p[1], "x", 1) == 1 && read(p[0], &c, 1) == 1 && c == 'x', "pipe I/O");
  close(p[0]);
  close(p[1]);
  snapshot();
  expect("open objects after cleanup", info.nfile, baseline);
  passed();
}

static void
testlimits(void)
{
  group("[1.5] fill descriptor table; reject dup/pipe without leaking files");
  int fds[NOFILE], p[2], n = 0, used = 0;
  snapshot();
  int baseline = info.nfile;
  for(int fd = 0; fd < NOFILE; fd++)
    used += process(getpid())->file_id[fd] != 0;
  while(n < NOFILE && (fds[n] = open("README", O_RDONLY)) >= 0)
    n++;
  expect("successful opens", n, NOFILE - used);
  check(n > 0, "no free descriptors at test start");
  expect("dup on full table", dup(fds[0]), -1);
  expect("pipe on full table", pipe(p), -1);
  snapshot();
  expect("open objects (no leaked allocations)", info.nfile, baseline + n);
  int freed = fds[--n];
  close(freed);
  expect("pipe with one free slot", pipe(p), -1);
  snapshot();
  expect("freed descriptor mapping", process(getpid())->file_id[freed], 0);
  expect("open objects after failed pipe", info.nfile, baseline + n);
  while(n > 0)
    close(fds[--n]);
  snapshot();
  expect("open objects after cleanup", info.nfile, baseline);
  passed();
}

static struct procinfo *
awaitstate(int pid, enum procstate state)
{
  // Poll a reported state with a deadline, rather than guessing when the child runs.
  for(int i = 0; i < 100; i++){
    snapshot();
    struct procinfo *p = process(pid);
    if(p->state == state)
      return p;
    sleep(1);
  }
  check(0, "timed out waiting for process state");
  return 0;
}

static void
testfork(void)
{
  group("[1.6] fork: shared descriptors, wakeup, zombie and wait");
  int ready[2], go[2], status;
  char c;
  snapshot();
  int nproc = info.nproc, nfile = info.nfile, parent = getpid();
  int fd = open("README", O_RDONLY), copy = dup(fd);
  check(fd >= 0 && copy >= 0 && pipe(ready) == 0 && pipe(go) == 0, "fork setup");
  int pid = fork();
  check(pid >= 0, "fork");
  if(pid == 0){
    close(ready[0]);
    close(go[1]);
    snapshot();
    check(process(getpid())->ppid == parent, "child parent PID");
    for(int i = 0; i < 2; i++){
      check(write(ready[1], "r", 1) == 1, "child ready");
      check(read(go[0], &c, 1) == 1, "child release");
    }
    exit(23);
  }
  close(ready[1]);
  close(go[0]);
  check(read(ready[0], &c, 1) == 1, "parent receives ready");
  struct procinfo *child = awaitstate(pid, SLEEPING);
  int id = fileid(fd);
  expect("process count", info.nproc, nproc + 1);
  expect("child's parent PID", child->ppid, parent);
  expect("child state (SLEEPING)", child->state, SLEEPING);
  check(child->file_id[fd] == id && child->file_id[copy] == id && fileid(copy) == id,
        "fork inherits descriptors");
  uint64 nsched = child->nsched;
  record("child PID", pid);
  record("shared file ID", id);
  expect("references (two descriptors per process)", info.files[id - 1].ref, 4);
  expect("unique owner PIDs", owners(id), 2);
  check(write(go[1], "g", 1) == 1 && read(ready[0], &c, 1) == 1, "wake child");
  child = awaitstate(pid, SLEEPING);
  record("nsched before wakeup", nsched);
  record("nsched after wakeup (must increase)", child->nsched);
  check(child->nsched > nsched, "schedule count grows after wakeup");
  check(write(go[1], "g", 1) == 1, "let child exit");
  child = awaitstate(pid, ZOMBIE);
  expect("child state (ZOMBIE)", child->state, ZOMBIE);
  expect("process count before wait", info.nproc, nproc + 1);
  expect("zombie's parent PID", child->ppid, parent);
  int childfds = 0;
  for(int i = 0; i < NOFILE; i++)
    childfds += child->file_id[i] != 0;
  expect("zombie's open descriptors", childfds, 0);
  expect("file references after child exit", info.files[id - 1].ref, 2);
  expect("wait returned PID", wait(&status), pid);
  expect("exit status", status, 23);
  close(ready[0]);
  close(go[1]);
  close(fd);
  close(copy);
  snapshot();
  expect("process count after wait", info.nproc, nproc);
  expect("open objects after cleanup", info.nfile, nfile);
  check(findproc(pid) == 0, "reaped child is absent");
  passed();
}

static void
testowners(void)
{
  group("[1.7] open once; share the file across parent and 16 children");
  int gate[2], pids[16], status;
  char tokens[16] = {0}, c;
  snapshot();
  int nproc = info.nproc, nfile = info.nfile;
  int fd = open("README", O_RDONLY);
  check(fd >= 0 && pipe(gate) == 0, "owner test setup");
  // One open followed by fork shares one object; independent opens would not.
  for(int i = 0; i < 16; i++){
    pids[i] = fork();
    check(pids[i] >= 0, "fork file owner");
    if(pids[i] == 0){
      close(gate[1]);
      check(read(gate[0], &c, 1) == 1, "owner release");
      exit(0);
    }
  }
  close(gate[0]);
  snapshot();
  int id = fileid(fd);
  expect("process count", info.nproc, nproc + 16);
  expect("unique owner PIDs", owners(id), 17);
  expect("file references", info.files[id - 1].ref, 17);
  for(int i = 0; i < 16; i++)
    check(process(pids[i])->file_id[fd] == id && process(pids[i])->ppid == getpid(),
          "owner PID and inherited file");
  check(write(gate[1], tokens, sizeof(tokens)) == sizeof(tokens), "release owners");
  close(gate[1]);
  for(int i = 0; i < 16; i++)
    check(wait(&status) > 0 && status == 0, "wait file owner");
  close(fd);
  snapshot();
  expect("process count after wait", info.nproc, nproc);
  expect("open objects after cleanup", info.nfile, nfile);
  passed();
}

static void
testconcurrent(void)
{
  group("[1.8] snapshots during concurrent open/dup/pipe/close");
  snapshot();
  int nproc = info.nproc, nfile = info.nfile;
  int pid = fork(), status;
  check(pid >= 0, "fork concurrent worker");
  if(pid == 0){
    for(int i = 0; i < 100; i++){
      int fd = open("README", O_RDONLY), copy = dup(fd), p[2];
      check(fd >= 0 && copy >= 0 && pipe(p) == 0, "concurrent open/dup/pipe");
      snapshot();
      close(fd);
      close(copy);
      close(p[0]);
      close(p[1]);
    }
    exit(0);
  }
  printf("    input: parent snapshots=200; worker pid=%d cycles=100\n", pid);
  for(int i = 0; i < 200; i++){
    snapshot();
  }
  expect("wait returned worker PID", wait(&status), pid);
  expect("worker exit status", status, 0);
  snapshot();
  expect("process count after wait", info.nproc, nproc);
  expect("open objects after cleanup", info.nfile, nfile);
  passed();
}

static void
testfiletable(void)
{
  group("[1.9] fill global file table; check allocation failure and cleanup");
  int ready[2], gate[2], pids[NPROC], counts[NPROC];
  int workers = 0, opened = 0, status;
  char tokens[NPROC] = {0}, c;

  snapshot();
  int nfile = info.nfile, nproc = info.nproc;
  check(pipe(ready) == 0 && pipe(gate) == 0, "file table test pipes");
  snapshot();
  int baseline = info.nfile;
  // Workers retain their opens until the parent finishes inspecting the full table.
  while(info.nfile < NFILE){
    check(workers < NPROC, "too many file table workers");
    int pid = fork();
    check(pid >= 0, "fork file table worker");
    if(pid == 0){
      close(ready[0]);
      close(gate[1]);
      int n = 0;
      while(open("README", O_RDONLY) >= 0)
        n++;
      check(write(ready[1], &n, sizeof(n)) == sizeof(n), "report worker opens");
      check(read(gate[0], &c, 1) == 1, "release file table worker");
      exit(0);
    }
    pids[workers] = pid;
    check(read(ready[0], &counts[workers], sizeof(int)) == sizeof(int), "receive worker opens");
    opened += counts[workers++];
    snapshot();
    expect("global count matches completed opens", info.nfile, baseline + opened);
    check(counts[workers - 1] > 0, "worker made no progress toward NFILE");
  }
  snapshot();
  expect("global open objects", info.nfile, NFILE);
  expect("process count after worker forks", info.nproc, nproc + workers);
  for(int i = 0; i < workers; i++){
    struct procinfo *p = process(pids[i]);
    int listed = 0;
    check(p->ppid == getpid(), "file worker parent PID");
    for(int fd = 0; fd < NOFILE; fd++){
      int id = p->file_id[fd];
      if(id == 0 || holds(process(getpid()), id))
        continue;
      struct fileinfo *f = &info.files[id - 1];
      check(f->type == FD_INODE && f->readable && !f->writable &&
            f->ref == 1 && owners(id) == 1, "worker file metadata or owner");
      listed++;
    }
    expect("worker's listed independent opens", listed, counts[i]);
  }
  int used = 0;
  for(int fd = 0; fd < NOFILE; fd++)
    used += process(getpid())->file_id[fd] != 0;
  check(used < NOFILE, "parent needs a free descriptor for global limit test");
  expect("open with full global table", open("README", O_RDONLY), -1);
  int extra[2];
  expect("pipe with full global table", pipe(extra), -1);
  int id = fileid(ready[0]), refs = info.files[id - 1].ref;
  int copy = dup(ready[0]);
  check(copy >= 0, "dup must work with full global file table");
  snapshot();
  expect("dup file ID", fileid(copy), id);
  expect("pipe references after dup", info.files[id - 1].ref, refs + 1);
  expect("global objects after failed allocations and dup", info.nfile, NFILE);
  close(copy);
  check(write(gate[1], tokens, workers) == workers, "release file table workers");
  close(gate[1]);
  for(int i = 0; i < workers; i++)
    check(wait(&status) > 0 && status == 0, "wait file table worker");
  close(ready[0]);
  close(ready[1]);
  close(gate[0]);
  snapshot();
  expect("open objects after cleanup", info.nfile, nfile);
  expect("process count after cleanup", info.nproc, nproc);
  passed();
}

static void
testproctable(void)
{
  group("[1.10] fill process table; reap one child and reuse its capacity");
  int gate[2], pids[NPROC], n = 0, status;
  char tokens[NPROC] = {0}, c;

  snapshot();
  int nproc = info.nproc, nfile = info.nfile;
  check(pipe(gate) == 0, "process table gate");
  while(n < NPROC){
    int pid = fork();
    if(pid < 0)
      break;
    if(pid == 0){
      close(gate[1]);
      check(read(gate[0], &c, 1) == 1, "release process table child");
      exit(0);
    }
    pids[n++] = pid;
  }
  snapshot();
  expect("successful forks", n, NPROC - nproc);
  expect("occupied process slots", info.nproc, NPROC);
  expect("new file objects (one shared pipe)", info.nfile, nfile + 2);
  expect("fork with all slots occupied", fork(), -1);
  int id = fileid(gate[0]);
  expect("PIDs holding the shared read end", owners(id), n + 1);
  for(int i = 0; i < n; i++){
    struct procinfo *p = process(pids[i]);
    check(p->ppid == getpid() && p->file_id[gate[0]] == id, "full table child details");
  }
  check(write(gate[1], "g", 1) == 1, "release one process slot");
  int reaped = wait(&status);
  check(reaped > 0 && status == 0, "wait one process table child");
  snapshot();
  expect("processes after one wait", info.nproc, NPROC - 1);
  check(findproc(reaped) == 0, "reaped PID absent from full table");
  int replacement = fork();
  check(replacement >= 0, "fork after freeing one process slot");
  if(replacement == 0){
    close(gate[1]);
    check(read(gate[0], &c, 1) == 1, "release replacement child");
    exit(0);
  }
  snapshot();
  expect("occupied process slots after replacement", info.nproc, NPROC);
  expect("replacement's parent PID", process(replacement)->ppid, getpid());
  check(replacement != reaped, "replacement has a new PID");
  check(write(gate[1], tokens, n) == n, "release remaining process table children");
  close(gate[1]);
  for(int i = 0; i < n; i++)
    check(wait(&status) > 0 && status == 0, "wait process table child");
  close(gate[0]);
  snapshot();
  expect("process count after cleanup", info.nproc, nproc);
  expect("open objects after cleanup", info.nfile, nfile);
  passed();
}

static void
testreparent(void)
{
  group("[1.11] parent exits; observe child adoption by init");
  int report[2], leave[2], hold[2], orphan, status;
  char c;

  snapshot();
  int nproc = info.nproc, nfile = info.nfile;
  check(pipe(report) == 0 && pipe(leave) == 0 && pipe(hold) == 0, "reparent test pipes");
  int parent = fork();
  check(parent >= 0, "fork orphan's parent");
  if(parent == 0){
    close(report[0]);
    close(leave[1]);
    close(hold[1]);
    int pid = fork();
    check(pid >= 0, "fork future orphan");
    if(pid == 0){
      close(leave[0]);
      check(read(hold[0], &c, 1) == 1, "release adopted child");
      snapshot();
      int ppid = process(getpid())->ppid;
      check(ppid == 1, "adopted child's own parent PID");
      check(write(report[1], &ppid, sizeof(ppid)) == sizeof(ppid), "report adopted parent PID");
      exit(0);
    }
    close(hold[0]);
    check(write(report[1], &pid, sizeof(pid)) == sizeof(pid), "report future orphan PID");
    close(report[1]);
    check(read(leave[0], &c, 1) == 1, "release orphan's parent");
    exit(0);
  }
  close(report[1]);
  close(leave[0]);
  close(hold[0]);
  check(read(report[0], &orphan, sizeof(orphan)) == sizeof(orphan), "receive future orphan PID");
  struct procinfo *child = awaitstate(orphan, SLEEPING);
  expect("process count before parent exit", info.nproc, nproc + 2);
  expect("child's original parent PID", child->ppid, parent);
  expect("intermediate parent's parent PID", process(parent)->ppid, getpid());
  check(write(leave[1], "g", 1) == 1, "let orphan's parent exit");
  close(leave[1]);
  expect("wait returned parent PID", wait(&status), parent);
  expect("parent exit status", status, 0);
  child = awaitstate(orphan, SLEEPING);
  expect("orphan's new parent PID", child->ppid, 1);
  expect("process count after parent's wait", info.nproc, nproc + 1);
  check(findproc(parent) == 0, "old parent has been reaped");
  int replacement = fork();
  check(replacement >= 0, "fork after orphan's parent was reaped");
  if(replacement == 0)
    exit(0);
  snapshot();
  expect("orphan's parent stays init", process(orphan)->ppid, 1);
  expect("new child's parent PID", process(replacement)->ppid, getpid());
  expect("wait returned new child PID", wait(&status), replacement);
  expect("new child exit status", status, 0);
  check(write(hold[1], "g", 1) == 1, "release orphan");
  close(hold[1]);
  int ppid;
  check(read(report[0], &ppid, sizeof(ppid)) == sizeof(ppid), "receive adopted parent PID");
  expect("parent PID reported by orphan", ppid, 1);
  close(report[0]);
  // Only init can wait for the adopted child; observe its removal instead.
  for(int i = 0; i < 100; i++){
    snapshot();
    if(findproc(orphan) == 0)
      break;
    sleep(1);
  }
  check(findproc(orphan) == 0, "init did not reap adopted child before deadline");
  expect("process count after init's wait", info.nproc, nproc);
  expect("open objects after cleanup", info.nfile, nfile);
  passed();
}

static int
freepages(void)
{
  snapshot();
  return info.freemem / PGSIZE;
}

static void
testmem(void)
{
  group("[1.2] free memory: sbrk, pipe, fork/wait and exhaustion");
  enum
  {
    N = 16
  };
  int p[2], gate[2], status;
  char c;

  int base = freepages();

  char *mem = sbrk(N * PGSIZE);
  check(mem != (char *)-1, "sbrk grow");
  for (int i = 0; i < N; i++)
    mem[i * PGSIZE] = 1;
  int grown = freepages();
  // Page-table pages may be needed too, so allow a few extra pages.
  record("sbrk grow: requested pages", N);
  record("sbrk grow: free pages before", base);
  record("sbrk grow: free pages after", grown);
  range("sbrk grow: pages consumed", base - grown, N, N + 3);

  check(sbrk(-N * PGSIZE) != (char *)-1, "sbrk shrink");
  int shrunk = freepages();
  expect("pages returned by sbrk shrink", shrunk - grown, N);

  check(pipe(p) == 0, "create pipe");
  expect("pages used by pipe", shrunk - freepages(), 1);
  close(p[0]);
  close(p[1]);
  expect("free pages after closing pipe", freepages(), shrunk);

  int before = freepages();
  check(pipe(gate) == 0, "memory test pipe");
  int pid = fork();
  check(pid >= 0, "fork memory child");
  if (pid == 0)
  {
    close(gate[1]);
    check(read(gate[0], &c, 1) == 1, "release memory child");
    exit(0);
  }
  close(gate[0]);
  int withchild = freepages();
  record("fork + pipe: child PID", pid);
  record("fork + pipe: free pages before", before);
  record("fork + pipe: free pages after", withchild);
  record("fork + pipe: pages consumed (>=4)", before - withchild);
  check(before - withchild >= 4, "child and pipe consume memory");
  check(write(gate[1], "g", 1) == 1, "release memory child");
  close(gate[1]);
  expect("wait returned PID", wait(&status), pid);
  expect("exit status", status, 0);
  expect("free pages after child is reaped (no leak)", freepages(), before);

  table_open = 0;
  pid = fork();
  check(pid >= 0, "fork memory exhaustion child");
  if(pid == 0){
    int pages = 0;
    while(sbrk(PGSIZE) != (char *)-1)
      pages++;
    snapshot();
    record("exhaustion: child PID", getpid());
    record("exhaustion: allocated pages", pages);
    expect("exhaustion: free bytes", info.freemem, 0);
    check(info.freemem == 0, "zero free bytes after memory exhaustion");
    check(sbrk(-pages * PGSIZE) != (char *)-1, "release exhausted memory");
    snapshot();
    record("release: returned pages", pages);
    record("release: free bytes (must be >0)", info.freemem);
    check(info.freemem > 0, "free memory recovers after release");
    exit(0);
  }
  expect("wait returned exhaustion child PID", wait(&status), pid);
  expect("memory exhaustion child exit status", status, 0);
  expect("free pages after exhaustion child exits", freepages(), before);
  passed();
}

static void
test_sysinfo(void)
{
  group("[1.1] sysinfo arguments and initial snapshot");
  expect("sysinfo(NULL)", sysinfo(0), -1);
  expect("sysinfo(invalid address)", sysinfo((struct sysinfo *)~(uint64)0), -1);
  snapshot();
  record("caller PID", getpid());
  record("free physical memory (bytes)", info.freemem);
  record("open file objects", info.nfile);
  record("process count", info.nproc);
  passed();
  testmem();
  testfiles();
  testpipe();
  testlimits();
  testfork();
  testowners();
  testconcurrent();
  testfiletable();
  testproctable();
  testreparent();
}

static volatile char *pages;
static uint mask;

static void
expectmask(char *label, uint actual, uint expected)
{
  row(label);
  hex(actual);
  printf(" │ ");
  hex(expected);
  printf(" │\n");
  check(actual == expected, label);
}

static uint
sample(int n)
{
  mask = ~0U;
  check(pgaccess((void *)pages, n, &mask) == 0, "sample mapped pages");
  return mask;
}

static void
reject(void *start, int n, uint *out, char *message)
{
  sample(32);
  pages[31 * PGSIZE] = 1;
  int ret = pgaccess(start, n, out);
  uint remaining = sample(32);
  if(!table_open){
    printf("  Expected for every row: return=-1; remaining A=0x80000000.\n");
    printf("  │ Case                       │ Start              │ N   │ Output             │ Ret │ Remaining A │\n");
    printf("  ├────────────────────────────┼────────────────────┼─────┼────────────────────┼─────┼─────────────┤\n");
    table_open = 1;
  }
  printf("  │ ");
  cell(message, 26);
  printf(" │ %p │ ", start);
  number(n, 3);
  printf(" │ %p │ ", out);
  number(ret, 3);
  printf(" │ ");
  hex(remaining);
  printf("  │\n");
  check(ret == -1, message);
  check(remaining == (1U << 31), "failed call preserves access bits");
}

static void
test_pgaccess(void)
{
  group("[2.1] pgaccess: read/write detection and reset between samples");
  char *mem = sbrk(34 * PGSIZE);
  check(mem != (char *)-1, "allocate test pages");
  check((uint64)mem % PGSIZE == 0, "test allocation is page aligned");
  pages = mem;
  for(int i = 0; i < 32; i++)
    pages[i * PGSIZE] = i;
  sample(32);
  printf("    region=%p; sample count=32; initial A bits cleared\n", mem);

  expectmask("untouched pages", sample(32), 0);
  check(pages[PGSIZE] == 1, "read test page");
  pages[3 * PGSIZE] = 9;
  expectmask("read page 1 and write page 3", sample(32), (1U << 1) | (1U << 3));
  expectmask("sample clears accessed bits", sample(32), 0);
  check(pages[3 * PGSIZE] == 9, "read page again");
  expectmask("access is detected again after clearing", sample(32), 1U << 3);
  passed();

  group("[2.2] pgaccess: all 32 bits and exact result size");
  for(int i = 0; i < 32; i++)
    pages[i * PGSIZE] = i;
  expectmask("all 32 pages accessed", sample(32), ~0U);
  pages[31 * PGSIZE] = 1;
  struct {
    uint before, result, after;
  } bounded = {0x12345678, 0, 0x87654321};
  check(pgaccess(mem, 32, &bounded.result) == 0, "32-page sample");
  expectmask("highest mask bit", bounded.result, 1U << 31);
  expectmask("result guard: before", bounded.before, 0x12345678);
  expectmask("result guard: after", bounded.after, 0x87654321);
  check(bounded.before == 0x12345678 && bounded.after == 0x87654321,
        "result writes exactly four bytes");

  passed();
  group("[2.3] pgaccess: zero count, unaligned start and read-only input");
  pages[PGSIZE] = 1;
  expectmask("zero pages returns zero mask", sample(0), 0);
  expectmask("zero count preserves accessed bits", sample(32), 1U << 1);
  pages[2 * PGSIZE] = 1;
  int ret = pgaccess(mem + 2 * PGSIZE + 7, 1, &mask);
  record("start offset within page (bytes)", 7);
  expect("unaligned start: syscall return", ret, 0);
  expectmask("unaligned start: mask", mask, 1);
  check(ret == 0 && mask == 1, "unaligned start selects its containing page");
  expect("read-only user text (VA=0)", pgaccess(0, 1, &mask), 0);
  passed();

  group("[2.4] pgaccess: invalid counts, ranges and protected pages");

  reject(mem, -1, &mask, "negative page count");
  reject(mem, 33, &mask, "page count above 32");
  reject((void *)MAXVA, 1, &mask, "address at MAXVA");
  reject((void *)~(uint64)0, 1, &mask, "address overflow");
  reject((void *)(MAXVA - PGSIZE), 2, &mask, "range exceeds MAXVA");
  reject(mem + 34 * PGSIZE, 1, &mask, "unmapped page");
  reject(mem + 31 * PGSIZE, 4, &mask, "partially mapped range");
  uint64 guard = PGROUNDDOWN(r_sp()) - USERSTACK * PGSIZE;
  reject((void *)guard, 1, &mask, "stack guard page");
  reject((void *)TRAPFRAME, 1, &mask, "supervisor trapframe");
  reject((void *)TRAMPOLINE, 1, &mask, "supervisor trampoline");

  passed();
  group("[2.5] pgaccess: invalid result buffers preserve access bits");
  reject(mem, 32, 0, "read-only output buffer");
  reject(mem, 32, (uint *)MAXVA, "output at MAXVA");
  reject(mem, 32, (uint *)~(uint64)0, "invalid output address");
  reject(mem, 32, (uint *)TRAPFRAME, "supervisor output buffer");
  reject(mem, 32, (uint *)(mem + 34 * PGSIZE - 2), "partially writable output");

  passed();
  group("[2.6] pgaccess: result spans two writable pages");
  pages[7 * PGSIZE] = 1;
  char *cross = mem + 33 * PGSIZE - 2;
  printf("    output buffer=%p\n", cross);
  record("output: offset within page", (uint64)cross % PGSIZE);
  record("output: size in bytes", sizeof(mask));
  expect("cross-page pgaccess return", pgaccess(mem, 32, (uint *)cross), 0);
  memmove(&mask, cross, sizeof(mask));
  expectmask("cross-page result", mask, 1U << 7);
  expectmask("cross-page output clears access bits", sample(32), 0);
  check(sbrk(-34 * PGSIZE) != (char *)-1, "release test pages");
  passed();
}

int
main(void)
{
  completed = 0;
  table_open = 0;
  printf("\nProject 2 │ Memory tests │ 17 groups\n");

  printf("\n[1/2] SYSINFO - free physical memory (11 groups)\n");
  note("Expected '-' is context; snapshot bounds are checked separately.");
  note("1.2 checks freemem; 1.3..1.11 check file/process regressions.");
  test_sysinfo();
  printf("\n[2/2] PGACCESS - accessed pages (6 groups)\n");
  note("Bit i represents page i (starting at 0); this implementation scans at most 32 pages.");
  test_pgaccess();

  expect("completed groups", completed, 17);
  printf("\nRESULT: assertions passed in %d/17 groups.\n", completed);
  note("Run ./project2test.py on Linux to include exec and page-table output checks.");
  exit(0);
}
