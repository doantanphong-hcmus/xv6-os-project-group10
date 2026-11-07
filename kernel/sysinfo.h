#ifndef XV6_SYSINFO_H
#define XV6_SYSINFO_H

#include "param.h"

enum filetype { FD_NONE, FD_PIPE, FD_INODE, FD_DEVICE };
enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

struct fileinfo {
  enum filetype type;
  int ref; // Includes temporary kernel references.
  char readable;
  char writable;
};

struct procinfo {
  int pid;
  enum procstate state;
  int ppid;
  uint64 nsched;
  // IDs are ftable slot + 1, valid within this snapshot; zero means closed.
  int file_id[NOFILE];
};

struct sysinfo {
  int nfile;
  int nproc;
  struct fileinfo files[NFILE]; // Indexed by file_id - 1; ref == 0 means unused.
  struct procinfo procs[NPROC]; // The first nproc entries are valid.
};

#endif
