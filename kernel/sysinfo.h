// kernel/sysinfo.h
// "Hợp đồng" giữa kernel và user-space cho syscall sysinfo.
// Dùng kích thước cố định để copyout an toàn, không cần cấp phát động.

#define SYSINFO_MAX_PIDS   16   // số pid tối đa mỗi file entry có thể liệt kê
#define SYSINFO_MAX_FDS    16   // số fd tối đa mỗi proc entry có thể liệt kê

// ------------------------------------------------------------------
// Thông tin một open-file trong ftable
// ------------------------------------------------------------------
struct sysinfo_file {
  int    valid;                      // 1 nếu entry này hợp lệ
  int    ftype;                      // 0=NONE, 1=PIPE, 2=INODE, 3=DEVICE
  int    readable;
  int    writable;
  int    ref;                        // reference count
  int    npid;                       // số pid trong mảng pids[]
  int    pids[SYSINFO_MAX_PIDS];     // pid các process đang giữ file này
};

// ------------------------------------------------------------------
// Thông tin một process
// ------------------------------------------------------------------
struct sysinfo_proc {
  int    valid;                      // 1 nếu entry này hợp lệ
  int    pid;
  int    ppid;                       // pid cha (0 nếu không có)
  int    state;                      // enum procstate: 0=UNUSED..5=ZOMBIE
  int    sched_count;                // số lần được scheduler chọn chạy
  int    nfd;                        // số fd đang mở
  int    fds[SYSINFO_MAX_FDS];       // chỉ số fd đang mở
};

// ------------------------------------------------------------------
// Struct chính truyền vào syscall sysinfo()
// Kernel fill các mảng theo chỉ số tương ứng với ftable/proc table.
// ------------------------------------------------------------------
struct sysinfo {
  int    nfile;                      // số file đang mở trong hệ thống
  int    nproc;                      // số process không UNUSED
  struct sysinfo_file files[100];    // NFILE = 100
  struct sysinfo_proc procs[64];     // NPROC = 64
};

