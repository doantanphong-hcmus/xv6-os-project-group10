#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// Run a command with tracing enabled for selected system calls.
int
main(int argc, char *argv[])
{
  int i, mask = 0;
  char *nargv[MAXARG];

  if(argc < 3 || (argv[1][0] < '0' || argv[1][0] > '9')){
    fprintf(2, "Usage: %s mask command\n", argv[0]);
    exit(1);
  }

  // Reject overflow beyond the signed 32-bit limit (0x7fffffff).
  for(char *s = argv[1]; *s; s++){
    if(*s < '0' || *s > '9' || mask > (0x7fffffff - (*s - '0')) / 10){
      fprintf(2, "%s: invalid mask\n", argv[0]);
      exit(1);
    }
    mask = mask * 10 + (*s - '0');
  }

  // Enable tracing before exec, which preserves the process's mask.
  if (trace(mask) < 0) {
    fprintf(2, "%s: trace failed\n", argv[0]);
    exit(1);
  }
  
  for(i = 2; i < argc && i < MAXARG; i++){
    nargv[i-2] = argv[i];
  }
  nargv[argc-2] = 0;
  exec(nargv[0], nargv);
  fprintf(2, "trace: exec failed\n");
  exit(1);
}
