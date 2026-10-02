/* A program that tries things a sandboxed program must not be able to do.
 *
 * It is linked statically, like generated programs, and run by
 * sandbox_test.cpp under the same policies seqc uses.
 *
 * Exit status: 0 the operation succeeded, 1 it was denied (EACCES or EPERM),
 * 2 it failed for another reason, 3 usage error. With no arguments the probe
 * prints what its process environment looks like.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

static int result_of(int rc) {
  if (rc >= 0) {
    return 0;
  }
  fprintf(stderr, "errno %d (%s)\n", errno, strerror(errno));
  return (errno == EACCES || errno == EPERM) ? 1 : 2;
}

static int inspect(int argc) {
  struct stat info;
  char cwd[1024];
  char byte;
  char** entry;
  int fd;

  printf("argc=%d\n", argc);
  for (entry = environ; *entry != NULL; ++entry) {
    printf("env=%s\n", *entry);
  }
  printf("cwd=%s\n", getcwd(cwd, sizeof(cwd)) != NULL ? cwd : "?");
  printf("stdin=%s\n", read(0, &byte, 1) == 0 ? "eof" : "data");
  printf("fd3=%s\n",
         fstat(3, &info) == 0 && S_ISFIFO(info.st_mode) ? "fifo" : "other");
  printf("fd4=%s\n",
         fstat(4, &info) == 0 && S_ISDIR(info.st_mode) ? "dir" : "other");
  printf("open=");
  for (fd = 0; fd < 256; ++fd) {
    if (fcntl(fd, F_GETFD) >= 0) {
      printf("%d ", fd);
    }
  }
  printf("\n");
  return 0;
}

int main(int argc, char* argv[]) {
  const char* action;
  const char* arg;

  if (argc < 2) {
    return inspect(argc);
  }
  action = argv[1];
  arg = argc > 2 ? argv[2] : "";

  if (strcmp(action, "read") == 0) {
    char buffer[64];
    int fd = open(arg, O_RDONLY);
    if (fd < 0) {
      return result_of(-1);
    }
    return result_of((int)read(fd, buffer, sizeof(buffer)));
  }
  if (strcmp(action, "write") == 0) {
    int fd = open(arg, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
      return result_of(-1);
    }
    return result_of((int)write(fd, "probe\n", 6));
  }
  if (strcmp(action, "truncate") == 0) {
    return result_of(truncate(arg, 0));
  }
  if (strcmp(action, "unlink") == 0) {
    return result_of(unlink(arg));
  }
  if (strcmp(action, "mkdir") == 0) {
    return result_of(mkdir(arg, 0700));
  }
  if (strcmp(action, "readdir") == 0) {
    DIR* dir = opendir(arg);
    if (dir == NULL) {
      return result_of(-1);
    }
    closedir(dir);
    return 0;
  }
  if (strcmp(action, "symlink") == 0 && argc > 3) {
    return result_of(symlink(arg, argv[3]));
  }
  if (strcmp(action, "link") == 0 && argc > 3) {
    return result_of(link(arg, argv[3]));
  }
  if (strcmp(action, "rename") == 0 && argc > 3) {
    return result_of(rename(arg, argv[3]));
  }
  if (strcmp(action, "socket") == 0) {
    return result_of(socket(AF_INET, SOCK_STREAM, 0));
  }
  if (strcmp(action, "socket-udp") == 0) {
    return result_of(socket(AF_INET, SOCK_DGRAM, 0));
  }
  if (strcmp(action, "socket-unix") == 0) {
    return result_of(socket(AF_UNIX, SOCK_STREAM, 0));
  }
  if (strcmp(action, "fork") == 0) {
    pid_t pid = fork();
    if (pid == 0) {
      _exit(0);
    }
    if (pid > 0) {
      waitpid(pid, NULL, 0);
    }
    return result_of(pid > 0 ? 0 : -1);
  }
  if (strcmp(action, "exec") == 0) {
    char* const args[] = {(char*)arg, NULL};
    execv(arg, args);
    return result_of(-1);
  }
  if (strcmp(action, "signal-parent") == 0) {
    /* Signal 0 only asks whether signalling the supervisor is permitted. */
    return result_of(kill(getppid(), 0));
  }
  if (strcmp(action, "signal-self") == 0) {
    return result_of(kill(getpid(), 0));
  }
  if (strcmp(action, "abort") == 0) {
    abort();
  }
  if (strcmp(action, "spin") == 0) {
    volatile unsigned long counter = 0;
    for (;;) {
      ++counter;
    }
  }
  if (strcmp(action, "sleep") == 0) {
    for (;;) {
      pause();
    }
  }
  if (strcmp(action, "flood") == 0) {
    static char block[65536];
    memset(block, 'x', sizeof(block));
    for (;;) {
      if (write(1, block, sizeof(block)) < 0) {
        return 2;
      }
    }
  }
  if (strcmp(action, "bigfile") == 0) {
    static char block[1 << 20];
    int fd = open(arg, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int i;
    if (fd < 0) {
      return result_of(-1);
    }
    for (i = 0; i < 4096; ++i) {
      if (write(fd, block, sizeof(block)) < 0) {
        return result_of(-1);
      }
    }
    return 0;
  }
  if (strcmp(action, "alloc") == 0) {
    /* 8 GiB, touched so it cannot be satisfied lazily. */
    size_t size = (size_t)8 << 30;
    char* memory = (char*)malloc(size);
    if (memory == NULL) {
      return 1;
    }
    memset(memory, 1, size);
    return memory[size - 1] == 1 ? 0 : 2;
  }
  if (strcmp(action, "orphan") == 0) {
    /* Leaves a child behind that keeps stdout open. */
    pid_t pid = fork();
    if (pid == 0) {
      for (;;) {
        pause();
      }
    }
    return result_of(pid > 0 ? 0 : -1);
  }
  if (strcmp(action, "report") == 0) {
    return result_of((int)write(3, "begin 1 probe\nok 1 probe\n", 25));
  }
  fprintf(stderr, "unknown action: %s\n", action);
  return 3;
}
