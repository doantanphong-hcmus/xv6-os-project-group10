#define SYSINFO_MAX_PIDS   16
#define SYSINFO_MAX_FDS    16

struct sysinfo_file {
  int    valid;
  int    ftype;                      // 0=NONE, 1=PIPE, 2=INODE, 3=DEVICE
  int    readable;
  int    writable;
  int    ref;
  int    npid;
  int    pids[SYSINFO_MAX_PIDS];
};

struct sysinfo_proc {
  int    valid;
  int    pid;
  int    ppid;
  int    state;
  int    sched_count;
  int    nfd;
  int    fds[SYSINFO_MAX_FDS];
};

struct sysinfo {
  uint64 freemem;   // amount of free memory (bytes)
  uint64 nproc;     // number of process
  int    nfile;     // number of opening files
  struct sysinfo_file files[100];    // NFILE = 100
  struct sysinfo_proc procs[64];     // NPROC = 64
};
