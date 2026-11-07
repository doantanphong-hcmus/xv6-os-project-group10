#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/sysinfo.h"
#include "user/user.h"

// The result spans multiple pages; keep it off the one-page user stack.
static struct sysinfo info;

static void
check(int ok, char *message)
{
  if(!ok){
    fprintf(2, "sysinfotest: FAIL: %s\n", message);
    exit(1);
  }
}

static void
expect(char *label, int actual, int expected)
{
  printf("    %s: expected=%d observed=%d [%s]\n",
         label, expected, actual, actual == expected ? "OK" : "FAIL");
  check(actual == expected, label);
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
showfile(int id)
{
  char *types[] = {"none", "pipe", "inode", "device"};
  struct fileinfo *f = &info.files[id - 1];

  printf("    [file #%d: %s %s%s ref=%d, owners=%d]\n", id,
         types[f->type], f->readable ? "r" : "-", f->writable ? "w" : "-",
         f->ref, owners(id));
  printf("        ^\n");
  for(int i = 0; i < info.nproc; i++){
    struct procinfo *p = &info.procs[i];
    if(!holds(p, id))
      continue;
    printf("        +--- pid %d:", p->pid);
    for(int fd = 0; fd < NOFILE; fd++)
      if(p->file_id[fd] == id)
        printf(" fd %d", fd);
    printf("\n");
  }
}

static void
dump(void)
{
  char *states[] = {"unused", "used", "sleeping", "runnable", "running", "zombie"};

  printf("  Snapshot: files=%d processes=%d\n", info.nfile, info.nproc);
  for(int i = 0; i < NFILE; i++)
    if(info.files[i].ref > 0)
      showfile(i + 1);
  for(int i = 0; i < info.nproc; i++){
    struct procinfo *p = &info.procs[i];
    printf("  pid=%d state=%s ppid=%d nsched=%lu fd:file:",
           p->pid, states[p->state], p->ppid, p->nsched);
    for(int fd = 0; fd < NOFILE; fd++)
      if(p->file_id[fd])
        printf(" %d:%d", fd, p->file_id[fd]);
    printf("\n");
  }
}

static void
testfiles(void)
{
  char first[3], second[3];
  printf("\n[2/10] Open, dup, independent open, close and modes\n");
  snapshot();
  int baseline = info.nfile;
  printf("  Baseline: %d open objects\n", baseline);
  printf("  -> open(README, O_RDONLY)\n");
  int fd = open("README", O_RDONLY);
  check(fd >= 0, "open README");
  snapshot();
  int id = fileid(fd);
  expect("open objects", info.nfile, baseline + 1);
  check(info.files[id - 1].type == FD_INODE, "new file type");
  expect("new file references", info.files[id - 1].ref, 1);
  check(info.files[id - 1].readable && !info.files[id - 1].writable, "read-only mode");
  showfile(id);

  printf("  -> dup(fd %d): expect the same object, one extra reference\n", fd);
  int copy = dup(fd);
  check(copy >= 0, "dup README");
  snapshot();
  expect("dup file ID", fileid(copy), id);
  expect("open objects after dup", info.nfile, baseline + 1);
  expect("references", info.files[id - 1].ref, 2);
  expect("unique owner PIDs", owners(id), 1);
  showfile(id);

  printf("  -> open(README) again: expect a different object\n");
  int separate = open("README", O_RDONLY);
  check(separate >= 0, "independent open");
  snapshot();
  printf("    Original file ID=%d, new open's file ID=%d; expected different IDs\n",
         id, fileid(separate));
  check(fileid(separate) != id, "open has a separate object");
  expect("open objects", info.nfile, baseline + 2);
  showfile(fileid(separate));
  printf("  -> read 2 bytes via fd %d, then 1 via dup fd %d\n", fd, copy);
  check(read(fd, first, 2) == 2 && read(copy, first + 2, 1) == 1 &&
        read(separate, second, 3) == 3, "read README");
  check(memcmp(first, second, 3) == 0, "dup shares offset; open has its own offset");
  printf("    Shared offset bytes: %x %x %x\n", (uchar)first[0], (uchar)first[1], (uchar)first[2]);
  printf("    New open's 3 bytes:  %x %x %x [equal]\n", (uchar)second[0], (uchar)second[1], (uchar)second[2]);
  printf("  -> close(fd %d): dup fd %d must remain usable\n", fd, copy);
  close(fd);
  snapshot();
  expect("closed descriptor mapping", process(getpid())->file_id[fd], 0);
  expect("remaining references", info.files[id - 1].ref, 1);
  expect("read through remaining descriptor", read(copy, first, 1), 1);
  showfile(id);
  printf("  -> close the remaining two descriptors\n");
  close(copy);
  close(separate);
  snapshot();
  expect("open objects", info.nfile, baseline);

  printf("  -> inspect access modes (no writes to README)\n");
  int modes[] = {O_WRONLY, O_RDWR};
  for(int i = 0; i < 2; i++){
    fd = open("README", modes[i]);
    check(fd >= 0, "open access mode");
    snapshot();
    struct fileinfo *f = &info.files[fileid(fd) - 1];
    check(f->writable && f->readable == (i == 1), "write-only or read-write mode");
    printf("    open(README, %s) returned fd %d\n", i == 0 ? "O_WRONLY" : "O_RDWR", fd);
    showfile(fileid(fd));
    close(fd);
  }
  fd = open(".", O_RDONLY);
  check(fd >= 0, "open directory");
  snapshot();
  check(info.files[fileid(fd) - 1].type == FD_INODE, "directory file type");
  printf("  -> open directory: expected inode, observed below\n");
  showfile(fileid(fd));
  close(fd);
  fd = open("console", O_RDWR);
  check(fd >= 0, "open console");
  snapshot();
  struct fileinfo *f = &info.files[fileid(fd) - 1];
  check(f->type == FD_DEVICE && f->readable && f->writable, "device type and mode");
  printf("  -> open console: expected device rw, observed below\n");
  showfile(fileid(fd));
  close(fd);
  snapshot();
  expect("open objects after cleanup", info.nfile, baseline);
  printf("sysinfotest: open/dup/close/modes OK\n");
}

static void
testpipe(void)
{
  int p[2];
  char c;
  printf("\n[3/10] Pipe endpoints and cleanup\n");
  snapshot();
  int baseline = info.nfile;
  printf("  -> pipe(): expect two objects, one read end and one write end\n");
  check(pipe(p) == 0, "create pipe");
  snapshot();
  int r = fileid(p[0]), w = fileid(p[1]);
  check(r != w, "pipe has two file objects");
  check(info.files[r - 1].type == FD_PIPE && info.files[w - 1].type == FD_PIPE,
        "pipe type");
  check(info.files[r - 1].readable && !info.files[r - 1].writable &&
        !info.files[w - 1].readable && info.files[w - 1].writable, "pipe modes");
  expect("open objects", info.nfile, baseline + 2);
  showfile(r);
  showfile(w);
  printf("  -> write byte 0x78, then read from the other end\n");
  check(write(p[1], "x", 1) == 1 && read(p[0], &c, 1) == 1 && c == 'x', "pipe I/O");
  printf("    Observed byte: 0x%x [equal]\n", (uchar)c);
  printf("  -> close both ends\n");
  close(p[0]);
  close(p[1]);
  snapshot();
  expect("open objects after cleanup", info.nfile, baseline);
  printf("sysinfotest: pipe OK\n");
}

static void
testlimits(void)
{
  int fds[NOFILE], p[2], n = 0, used = 0;
  printf("\n[4/10] Full descriptor table and failed-allocation cleanup\n");
  snapshot();
  int baseline = info.nfile;
  for(int fd = 0; fd < NOFILE; fd++)
    used += process(getpid())->file_id[fd] != 0;
  printf("  Baseline: %d/%d descriptors used, %d open objects\n", used, NOFILE, baseline);
  printf("  -> open README until open returns -1\n");
  while(n < NOFILE && (fds[n] = open("README", O_RDONLY)) >= 0)
    n++;
  expect("successful opens", n, NOFILE - used);
  check(n > 0, "no free descriptors at test start");
  expect("dup on full table", dup(fds[0]), -1);
  expect("pipe on full table", pipe(p), -1);
  snapshot();
  printf("    Observed fd:file mappings:");
  for(int fd = 0; fd < NOFILE; fd++)
    printf(" %d:%d", fd, process(getpid())->file_id[fd]);
  printf("\n");
  expect("open objects (no leaked allocations)", info.nfile, baseline + n);
  int freed = fds[--n];
  close(freed);
  printf("  -> close fd %d; pipe still needs TWO free slots\n", freed);
  expect("pipe with one free slot", pipe(p), -1);
  snapshot();
  expect("freed descriptor mapping", process(getpid())->file_id[freed], 0);
  expect("open objects after failed pipe", info.nfile, baseline + n);
  printf("  -> close all descriptors opened by this test\n");
  while(n > 0)
    close(fds[--n]);
  snapshot();
  expect("open objects after cleanup", info.nfile, baseline);
  printf("sysinfotest: descriptor limits/rollback OK\n");
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
  int ready[2], go[2], status;
  char c;
  printf("\n[5/10] Fork, scheduling, exit and wait\n");
  snapshot();
  int nproc = info.nproc, nfile = info.nfile, parent = getpid();
  printf("  Baseline: parent pid=%d, processes=%d, open objects=%d\n", parent, nproc, nfile);
  printf("  -> open README, dup it, then fork\n");
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
  printf("  -> wait for child's ready byte; observe it blocked on a pipe\n");
  check(read(ready[0], &c, 1) == 1, "parent receives ready");
  struct procinfo *child = awaitstate(pid, SLEEPING);
  int id = fileid(fd);
  expect("process count", info.nproc, nproc + 1);
  expect("child's parent PID", child->ppid, parent);
  expect("child state (SLEEPING)", child->state, SLEEPING);
  check(child->file_id[fd] == id && child->file_id[copy] == id && fileid(copy) == id,
        "fork inherits descriptors");
  uint64 nsched = child->nsched;
  expect("references (two descriptors per process)", info.files[id - 1].ref, 4);
  expect("unique owner PIDs", owners(id), 2);
  showfile(id);
  printf("  -> send a byte to wake child, then wait for its next ready byte\n");
  check(write(go[1], "g", 1) == 1 && read(ready[0], &c, 1) == 1, "wake child");
  child = awaitstate(pid, SLEEPING);
  printf("    Child pid=%d nsched: before=%lu after=%lu; expected after > before\n",
         pid, nsched, child->nsched);
  check(child->nsched > nsched, "schedule count grows after wakeup");
  printf("  -> release child to exit(23), without calling wait yet\n");
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
  showfile(id);
  printf("  -> wait(&status), then close test descriptors\n");
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
  printf("sysinfotest: fork/parent/state/scheduling/zombie/wait OK\n");
}

static void
testowners(void)
{
  int gate[2], pids[16], status;
  char tokens[16] = {0}, c;
  printf("\n[6/10] One file object shared by 17 processes\n");
  snapshot();
  int nproc = info.nproc, nfile = info.nfile;
  int fd = open("README", O_RDONLY);
  check(fd >= 0 && pipe(gate) == 0, "owner test setup");
  printf("  -> open README ONCE, fork 16 children, keep them alive on a pipe\n");
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
  printf("sysinfotest: shared file %d has %d PIDs and ref=%d\n",
         id, owners(id), info.files[id - 1].ref);
  showfile(id);
  printf("  -> release all children, wait for them, then close README\n");
  check(write(gate[1], tokens, sizeof(tokens)) == sizeof(tokens), "release owners");
  close(gate[1]);
  for(int i = 0; i < 16; i++)
    check(wait(&status) > 0 && status == 0, "wait file owner");
  close(fd);
  snapshot();
  expect("process count after wait", info.nproc, nproc);
  expect("open objects after cleanup", info.nfile, nfile);
  printf("sysinfotest: 17 shared-file owners OK\n");
}

static void
testconcurrent(void)
{
  printf("\n[7/10] Snapshots while another process opens and closes files\n");
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
  printf("  Worker pid=%d: 100 open/dup/pipe/close cycles, one snapshot per cycle\n", pid);
  printf("  Parent pid=%d: 200 snapshots checking counts, IDs, states and references\n", getpid());
  for(int i = 0; i < 200; i++){
    snapshot();
    if((i + 1) % 50 == 0)
      printf("    Checked %d/200 snapshots: observed files=%d processes=%d [OK]\n",
             i + 1, info.nfile, info.nproc);
  }
  expect("wait returned worker PID", wait(&status), pid);
  expect("worker exit status", status, 0);
  snapshot();
  expect("process count after wait", info.nproc, nproc);
  expect("open objects after cleanup", info.nfile, nfile);
  printf("sysinfotest: concurrent snapshots OK\n");
}

static void
testfiletable(void)
{
  int ready[2], gate[2], pids[NPROC], counts[NPROC];
  int workers = 0, opened = 0, status;
  char tokens[NPROC] = {0}, c;

  printf("\n[8/10] Fill the GLOBAL file table, not just one descriptor table\n");
  snapshot();
  int nfile = info.nfile, nproc = info.nproc;
  check(pipe(ready) == 0 && pipe(gate) == 0, "file table test pipes");
  snapshot();
  int baseline = info.nfile;
  printf("  Baseline: %d objects, including the test pipes; NFILE=%d\n", baseline, NFILE);
  printf("  -> fork workers that each open README until open fails\n");
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
    printf("    Worker pid=%d opened %d objects; observed global count=%d/%d\n",
           pid, counts[workers - 1], info.nfile, NFILE);
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
  printf("  -> parent still has descriptor space; global exhaustion must reject open/pipe\n");
  int used = 0;
  for(int fd = 0; fd < NOFILE; fd++)
    used += process(getpid())->file_id[fd] != 0;
  printf("    Parent descriptors: %d/%d used\n", used, NOFILE);
  check(used < NOFILE, "parent needs a free descriptor for global limit test");
  expect("open with full global table", open("README", O_RDONLY), -1);
  int extra[2];
  expect("pipe with full global table", pipe(extra), -1);
  printf("  -> dup an existing pipe descriptor: no new file object is needed\n");
  int id = fileid(ready[0]), refs = info.files[id - 1].ref;
  int copy = dup(ready[0]);
  check(copy >= 0, "dup must work with full global file table");
  snapshot();
  expect("dup file ID", fileid(copy), id);
  expect("pipe references after dup", info.files[id - 1].ref, refs + 1);
  expect("global objects after failed allocations and dup", info.nfile, NFILE);
  close(copy);
  printf("  -> release %d workers and wait for all their files to close\n", workers);
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
  printf("sysinfotest: full global file table OK\n");
}

static void
testproctable(void)
{
  int gate[2], pids[NPROC], n = 0, status;
  char tokens[NPROC] = {0}, c;

  printf("\n[9/10] Fill the process table and reuse a released slot\n");
  snapshot();
  int nproc = info.nproc, nfile = info.nfile;
  check(pipe(gate) == 0, "process table gate");
  printf("  Baseline: %d processes; NPROC=%d\n", nproc, NPROC);
  printf("  -> fork until failure; children stay alive waiting on a pipe\n");
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
  printf("    Observed children:");
  for(int i = 0; i < n; i++){
    struct procinfo *p = process(pids[i]);
    check(p->ppid == getpid() && p->file_id[gate[0]] == id, "full table child details");
    if(i % 8 == 0)
      printf("\n      ");
    printf(" %d", p->pid);
  }
  printf("\n  -> release ONE child, then wait to free its process slot\n");
  check(write(gate[1], "g", 1) == 1, "release one process slot");
  int reaped = wait(&status);
  check(reaped > 0 && status == 0, "wait one process table child");
  snapshot();
  expect("processes after one wait", info.nproc, NPROC - 1);
  check(findproc(reaped) == 0, "reaped PID absent from full table");
  printf("  -> fork again: the released capacity must be usable\n");
  int replacement = fork();
  check(replacement >= 0, "fork after freeing one process slot");
  if(replacement == 0){
    close(gate[1]);
    check(read(gate[0], &c, 1) == 1, "release replacement child");
    exit(0);
  }
  snapshot();
  printf("    Reaped PID=%d; replacement PID=%d\n", reaped, replacement);
  expect("occupied process slots after replacement", info.nproc, NPROC);
  expect("replacement's parent PID", process(replacement)->ppid, getpid());
  check(replacement != reaped, "replacement has a new PID");
  printf("  -> release and wait for the remaining %d children\n", n);
  check(write(gate[1], tokens, n) == n, "release remaining process table children");
  close(gate[1]);
  for(int i = 0; i < n; i++)
    check(wait(&status) > 0 && status == 0, "wait process table child");
  close(gate[0]);
  snapshot();
  expect("process count after cleanup", info.nproc, nproc);
  expect("open objects after cleanup", info.nfile, nfile);
  printf("sysinfotest: full process table and slot reuse OK\n");
}

static void
testreparent(void)
{
  int report[2], leave[2], hold[2], orphan, status;
  char c;

  printf("\n[10/10] Parent exits before child: adoption by init\n");
  snapshot();
  int nproc = info.nproc, nfile = info.nfile;
  check(pipe(report) == 0 && pipe(leave) == 0 && pipe(hold) == 0, "reparent test pipes");
  printf("  -> create a child which creates its own child\n");
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
  printf("    pid %d\n      +--- pid %d\n             +--- pid %d (ppid=%d)\n",
         getpid(), parent, orphan, child->ppid);
  printf("  -> let the intermediate parent exit, then reap it with wait\n");
  check(write(leave[1], "g", 1) == 1, "let orphan's parent exit");
  close(leave[1]);
  expect("wait returned parent PID", wait(&status), parent);
  expect("parent exit status", status, 0);
  child = awaitstate(orphan, SLEEPING);
  expect("orphan's new parent PID", child->ppid, 1);
  expect("process count after parent's wait", info.nproc, nproc + 1);
  check(findproc(parent) == 0, "old parent has been reaped");
  printf("    init pid %d\n      +--- pid %d (ppid=%d)\n", process(1)->pid, orphan, child->ppid);
  printf("  -> fork a new child after reaping the old parent\n");
  int replacement = fork();
  check(replacement >= 0, "fork after orphan's parent was reaped");
  if(replacement == 0)
    exit(0);
  snapshot();
  printf("    Old parent PID=%d; new child PID=%d; orphan PID=%d\n", parent, replacement, orphan);
  expect("orphan's parent stays init", process(orphan)->ppid, 1);
  expect("new child's parent PID", process(replacement)->ppid, getpid());
  expect("wait returned new child PID", wait(&status), replacement);
  expect("new child exit status", status, 0);
  printf("  -> release orphan; it verifies its own parent PID before exiting\n");
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
  printf("sysinfotest: reparent and old-parent cleanup OK\n");
}

int
main(void)
{
  printf("+--------------------------------------------------+\n");
  printf("| sysinfotest: File and Process Management          |\n");
  printf("+--------------------------------------------------+\n");
  printf("Each step shows an action and its observed result.\n");
  printf("File IDs and diagrams come from the current snapshot.\n");
  printf("\n[1/10] Arguments and initial system snapshot\n");
  expect("sysinfo(NULL)", sysinfo(0), -1);
  expect("sysinfo(invalid address)", sysinfo((struct sysinfo *)~(uint64)0), -1);
  snapshot();
  dump();
  printf("sysinfotest: arguments/snapshot fields OK\n");
  testfiles();
  testpipe();
  testlimits();
  testfork();
  testowners();
  testconcurrent();
  testfiletable();
  testproctable();
  testreparent();
  printf("\n+--------------------------------------------------+\n");
  printf("sysinfotest: ALL TESTS PASSED (10/10 groups)\n");
  printf("+--------------------------------------------------+\n");
  exit(0);
}
