//===-- klee-replay.c -----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee-replay.h"

#include "klee/ADT/KTest.h"

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <signal.h>

#ifndef fgetc_unlocked
#define fgetc_unlocked(x) fgetc (x)
#endif

#ifndef fputc_unlocked
#define fputc_unlocked(x,y) fputc (x,y)
#endif

#else
#include <sys/signal.h>
#endif

#include <limits.h>

#ifdef HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif

static void __emit_error(const char *msg);

static KTest* input;
static unsigned obj_index;

static const char *progname = 0;
static unsigned monitored_pid = 0;
static unsigned monitored_timeout;

static char *rootdir = NULL;

typedef struct {
  char *name;
  unsigned char *data;
  unsigned numBytes;
} SymbolOverride;

static SymbolOverride *overrides = NULL;
static char *override_out_path = NULL;
static unsigned num_overrides = 0;

static struct option long_options[] = {
  {"create-files-only", required_argument, 0, 'f'},
  {"chroot-to-dir", required_argument, 0, 'r'},
  {"help", no_argument, 0, 'h'},
  {"keep-replay-dir", no_argument, 0, 'k'},
  {"override-symbol", required_argument, 0, 'o'},
  {"override-out-file", required_argument, 0, 'O'},
  {0, 0, 0, 0},
};

static void add_override(const char *name, unsigned char *data,
                         unsigned numBytes);
static void parse_override_option(const char *arg);
static void apply_overrides(KTest *t);

static void stop_monitored(int process) {
  fputs("KLEE-REPLAY: NOTE: TIMEOUT: ATTEMPTING GDB EXIT\n", stderr);
  int pid = fork();
  if (pid < 0) {
    fputs("KLEE-REPLAY: ERROR: gdb_exit: fork failed\n", stderr);
  } else if (pid == 0) {
    /* Run gdb in a child process. */
    const char *gdbargs[] = {
      "/usr/bin/gdb",
      "--pid", "",
      "-q",
      "--batch",
      "--eval-command=call exit(1)",
      0,
      0
    };
    char pids[64];
    snprintf(pids, sizeof(pids), "%d", process);

    gdbargs[2] = pids;
    /* Make sure gdb doesn't talk to the user */
    close(0);

    fputs("KLEE-REPLAY: NOTE: RUNNING GDB: ", stderr);
    unsigned i;
    for (i = 0; i != 5; ++i)
      fprintf(stderr, "%s ", gdbargs[i]);
    fputc('\n', stderr);

    execvp(gdbargs[0], (char * const *) gdbargs);
    perror("execvp");
    _exit(66);
  } else {
    /* Parent process, wait for gdb to finish. */
    int res, status;
    do {
      res = waitpid(pid, &status, 0);
    } while (res < 0 && errno == EINTR);

    if (res < 0) {
      perror("waitpid");
      _exit(66);
    }
  }
}

static void int_handler(int signal) {
  fprintf(stderr, "KLEE-REPLAY: NOTE: %s: Received signal %d.  Killing monitored process(es)\n",
          progname, signal);
  if (monitored_pid) {
    stop_monitored(monitored_pid);
    /* Kill the process group of monitored_pid.  Since we called
       setpgrp() for pid, this will not kill us, or any of our
       ancestors */
    kill(-monitored_pid, SIGKILL);
  } else {
    _exit(99);
  }
}

static void timeout_handler(int signal) {
  fprintf(stderr, "KLEE-REPLAY: NOTE: EXIT STATUS: TIMED OUT (%d seconds)\n",
          monitored_timeout);
  if (monitored_pid) {
    stop_monitored(monitored_pid);
    /* Kill the process group of monitored_pid.  Since we called
       setpgrp() for pid, this will not kill us, or any of our
       ancestors */
    kill(-monitored_pid, SIGKILL);
  } else {
    _exit(88);
  }
}

void process_status(int status, time_t elapsed, const char *pfx) {
  if (pfx)
    fprintf(stderr, "KLEE-REPLAY: NOTE: %s: ", pfx);
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "KLEE-REPLAY: NOTE: EXIT STATUS: CRASHED signal %d (%d seconds)\n",
            WTERMSIG(status), (int) elapsed);
    _exit(77);
  } else if (WIFEXITED(status)) {
    int rc = WEXITSTATUS(status);

    char msg[64];
    if (rc == 0) {
      strcpy(msg, "NORMAL");
    } else {
      snprintf(msg, sizeof(msg), "ABNORMAL %d", rc);
    }
    fprintf(stderr, "KLEE-REPLAY: NOTE: EXIT STATUS: %s (%d seconds)\n", msg, (int) elapsed);
    _exit(rc);
  } else {
    fprintf(stderr, "KLEE-REPLAY: NOTE: EXIT STATUS: NONE (%d seconds)\n", (int) elapsed);
    _exit(0);
  }
}

/* This function assumes that executable is a path pointing to some existing
 * binary and rootdir is a path pointing to some directory.
 */
static inline char *strip_root_dir(char *executable, char *rootdir) {
  return executable + strlen(rootdir);
}

static void run_monitored(char *executable, int argc, char **argv) {
  int pid;
  const char *t = getenv("KLEE_REPLAY_TIMEOUT");
  if (!t)
    t = "10000000";
  monitored_timeout = atoi(t);

  if (monitored_timeout==0) {
    fprintf(stderr, "KLEE-REPLAY: ERROR: invalid timeout (%s)\n", t);
    _exit(1);
  }

  /* Kill monitored process(es) on SIGINT and SIGTERM */
  signal(SIGINT, int_handler);
  signal(SIGTERM, int_handler);

  signal(SIGALRM, timeout_handler);
  pid = fork();
  if (pid < 0) {
    perror("fork");
    _exit(66);
  } else if (pid == 0) {
    /* This process actually executes the target program.
     *
     * Create a new process group for pid, and the process tree it may spawn. We
     * do this, because later on we might want to kill pid _and_ all processes
     * spawned by it and its descendants.
     */
#ifndef __FreeBSD__
    setpgrp();
#else
    setpgrp(0, 0);
#endif

    if (!rootdir) {
      if (chdir(replay_dir) != 0) {
        perror("chdir");
        _exit(66);
      }

      execv(executable, argv);
      perror("execv");
      _exit(66);
    }

    fprintf(stderr, "KLEE-REPLAY: NOTE: rootdir: %s\n", rootdir);
    const char *msg;
    if ((msg = "chdir", chdir(rootdir) == 0) &&
      (msg = "chroot", chroot(rootdir) == 0)) {
      msg = "execv";
      executable = strip_root_dir(executable, rootdir);
      argv[0] = strip_root_dir(argv[0], rootdir);
      execv(executable, argv);
    }
    perror(msg);
    _exit(66);
  } else {
    /* Parent process which monitors the child. */
    int res, status;
    time_t start = time(0);
    sigset_t masked;

    sigemptyset(&masked);
    sigaddset(&masked, SIGALRM);

    monitored_pid = pid;
    alarm(monitored_timeout);
    do {
      res = waitpid(pid, &status, 0);
    } while (res < 0 && errno == EINTR);

    if (res < 0) {
      perror("waitpid");
      _exit(66);
    }

    /* Just in case, kill the process group of pid.  Since we called setpgrp()
       for pid, this will not kill us, or any of our ancestors */
    kill(-pid, SIGKILL);
    process_status(status, time(0) - start, 0);
  }
}

#ifdef HAVE_SYS_CAPABILITY_H
/* ensure this process has CAP_SYS_CHROOT capability. */
void ensure_capsyschroot(const char *executable) {
  cap_t caps = cap_get_proc();  // all current capabilities.
  cap_flag_value_t chroot_permitted, chroot_effective;

  if (!caps)
    perror("cap_get_proc");
  /* effective and permitted flags should be set for CAP_SYS_CHROOT. */
  cap_get_flag(caps, CAP_SYS_CHROOT, CAP_PERMITTED, &chroot_permitted);
  cap_get_flag(caps, CAP_SYS_CHROOT, CAP_EFFECTIVE, &chroot_effective);
  if (chroot_permitted != CAP_SET || chroot_effective != CAP_SET) {
    fputs("KLEE-REPLAY: ERROR: chroot: No CAP_SYS_CHROOT capability.\n", stderr);
    exit(1);
  }
  cap_free(caps);
}
#endif

static void add_override(const char *name, unsigned char *data,
                         unsigned numBytes) {
  SymbolOverride *tmp =
      (SymbolOverride *)realloc(overrides,
                                (num_overrides + 1) * sizeof(SymbolOverride));
  if (!tmp) {
    fputs("KLEE-REPLAY: ERROR: override realloc failed\n", stderr);
    exit(1);
  }
  overrides = tmp;

  overrides[num_overrides].name = strdup(name);
  if (!overrides[num_overrides].name) {
    fputs("KLEE-REPLAY: ERROR: override name strdup failed\n", stderr);
    exit(1);
  }
  overrides[num_overrides].data = data;
  overrides[num_overrides].numBytes = numBytes;
  ++num_overrides;
}

static void parse_override_option(const char *arg) {
  const char *eq = strchr(arg, '=');
  if (!eq || eq == arg || !*(eq + 1)) {
    fprintf(stderr,
            "KLEE-REPLAY: ERROR: invalid override '%s' "
            "(expected NAME=b0,b1,...)\n",
            arg);
    exit(1);
  }

  size_t name_len = (size_t)(eq - arg);
  char *name = (char *)malloc(name_len + 1);
  if (!name) {
    fputs("KLEE-REPLAY: ERROR: override name malloc failed\n", stderr);
    exit(1);
  }
  memcpy(name, arg, name_len);
  name[name_len] = '\0';

  const char *p = eq + 1;
  unsigned char *buf = NULL;
  unsigned count = 0;

  while (*p) {
    while (*p == ',' || *p == ' ' || *p == '\t')
      ++p;
    if (!*p)
      break;

    char *endptr = NULL;
    long v = strtol(p, &endptr, 10);
    if (endptr == p || v < 0 || v > 255) {
      fprintf(stderr,
              "KLEE-REPLAY: ERROR: invalid byte in override '%s' "
              "(got '%.*s')\n",
              arg, (int)(endptr - p), p);
      exit(1);
    }

    unsigned char *tmp =
        (unsigned char *)realloc(buf, (count + 1) * sizeof(unsigned char));
    if (!tmp) {
      fputs("KLEE-REPLAY: ERROR: override bytes realloc failed\n", stderr);
      free(buf);
      exit(1);
    }
    buf = tmp;
    buf[count++] = (unsigned char)v;

    p = endptr;
  }

  if (count == 0) {
    fprintf(stderr, "KLEE-REPLAY: ERROR: override '%s' has no bytes\n", arg);
    free(name);
    free(buf);
    exit(1);
  }

  add_override(name, buf, count);
  free(name);
}

static void usage(void) {
  fprintf(stderr,
    "Usage: %s [option]... <executable> <ktest-file>...\n"
    "   or: %s --create-files-only <ktest-file>\n"
    "\n"
    "-r, --chroot-to-dir=DIR  use chroot jail, requires CAP_SYS_CHROOT\n"
    "-k, --keep-replay-dir    do not delete replay directory\n"
    "-o, --override-symbol    override symbolic object bytes: NAME=b0,b1,...\n"
    "-O, --override-out-file=FILE  write overridden ktest to FILE\n"
    "-h, --help               display this help and exit\n"
    "\n"
    "Use KLEE_REPLAY_TIMEOUT environment variable to set a timeout (in seconds).\n",
    progname, progname);
  exit(1);
}

static void apply_overrides(KTest *t) {
  if (!t || num_overrides == 0)
    return;
  unsigned i, j;
  for (i = 0; i < num_overrides; ++i) {
    SymbolOverride *ov = &overrides[i];
    int found = 0;

    for (j = 0; j < t->numObjects; ++j) {
      KTestObject *obj = &t->objects[j];
      if (obj->name && strcmp(obj->name, ov->name) == 0) {
        found = 1;
        if (obj->numBytes != ov->numBytes) {
          fprintf(stderr,
                  "KLEE-REPLAY: ERROR: override for '%s' size mismatch "
                  "(ktest=%d, override=%u)\n",
                  ov->name, obj->numBytes, ov->numBytes);
          exit(1);
        }
        memcpy(obj->bytes, ov->data, ov->numBytes);
      }
    }

    if (!found) {
      fprintf(stderr,
              "KLEE-REPLAY: WARNING: override for '%s' did not match any "
              "object in ktest\n",
              ov->name);
    }
  }
}

int keep_temps = 0;

int main(int argc, char** argv) {
  int prg_argc;
  char ** prg_argv;

  progname = argv[0];

  if (argc < 3)
    usage();

  int c, opt_index;
  while ((c = getopt_long(argc, argv, "f:r:ko:O:", long_options, &opt_index)) != -1) {
    switch (c) {
    case 'f': {
      /* Special case hack for only creating files and not actually executing
       * the program. */
      if (argc != 3)
        usage();

      char *input_fname = optarg;

      input = kTest_fromFile(input_fname);
      if (!input) {
        fprintf(stderr, "KLEE-REPLAY: ERROR: input file %s not valid.\n", input_fname);
        exit(1);
      }

      apply_overrides(input);
      if (num_overrides > 0) {
        const char *out_path = override_out_path;
        char default_name[PATH_MAX];

        if (!out_path) {
          snprintf(default_name, sizeof(default_name), "%s.overridden.ktest", input_fname);
          out_path = default_name;
        }

        if (kTest_toFile(input, out_path) != 0) {
          fprintf(stderr, "KLEE-REPLAY: ERROR: failed to write overridden test '%s': %s (errno=%d)\n", out_path, strerror(errno), errno);
          exit(1);
        }
        fprintf(stderr, "KLEE-REPLAY: NOTE: wrote overridden test '%s'\n", out_path);
      }

      prg_argc = input->numArgs;
      prg_argv = input->args;
      free(prg_argv[0]);
      prg_argv[0] = strdup(argv[1]);
      klee_init_env(&prg_argc, &prg_argv);

      replay_create_files(&__exe_fs);
      kTest_free(input);
      return 0;
    }

    case 'r':
      rootdir = optarg;
      break;

    case 'k':
      keep_temps = 1;
      break;

    case 'o':
      /* override-symbol: NAME=b0,b1,... */
      parse_override_option(optarg);
      break;

    case 'O':
      override_out_path = optarg;
      break;

    case 'h':
      usage();
      break;

    default:
      usage();
    }
  }

  // Executable needs to be converted to an absolute path, as klee-replay calls
  // chdir just before executing it
  char executable[PATH_MAX];
  if (!realpath(argv[optind], executable)) {
    snprintf(executable, PATH_MAX, "KLEE-REPLAY: ERROR: executable %s:",
             argv[optind]);
    perror(executable);
    exit(1);
  }
  /* Normal execution path ... */

  /* make sure this process has the CAP_SYS_CHROOT capability, if possible. */
#ifdef HAVE_SYS_CAPABILITY_H
  if (rootdir)
    ensure_capsyschroot(progname);
#endif

  /* rootdir should be a prefix of executable's path. */
  if (rootdir && strstr(executable, rootdir) != executable) {
    fputs("KLEE-REPLAY: ERROR: chroot: root dir should be a parent dir of executable.\n", stderr);
    exit(1);
  }

  if (override_out_path && (argc - (optind + 1) > 1)) {
    fputs("KLEE-REPLAY: ERROR: --override-out-file can only be used with a single ktest input.\n", stderr);
    exit(1);
  }

  int idx = 0;
  for (idx = optind + 1; idx != argc; ++idx) {
    char* input_fname = argv[idx];
    unsigned i;

    input = kTest_fromFile(input_fname);
    if (!input) {
      fprintf(stderr, "KLEE-REPLAY: ERROR: input file %s not valid.\n",
              input_fname);
      exit(1);
    }

    apply_overrides(input);
    if (num_overrides > 0) {
      const char *out_path = override_out_path;
      char default_name[PATH_MAX];

      if (!out_path) {
        snprintf(default_name, sizeof(default_name),
                 "%s.overridden.ktest", input_fname);
        out_path = default_name;
      }

      FILE *dbg = fopen(out_path, "wb");
      if (!dbg) {
        fprintf(stderr,
                "KLEE-REPLAY: DEBUG: fopen('%s','wb') failed: %s (errno=%d)\n",
                out_path, strerror(errno), errno);
        exit(1);
      }
      fclose(dbg);

      if (!kTest_toFile(input, out_path)) {
        fprintf(stderr,
                "KLEE-REPLAY: ERROR: kTest_toFile failed for '%s'\n",
                out_path);
        exit(1);
      }
      fprintf(stderr,
              "KLEE-REPLAY: NOTE: wrote overridden test '%s'\n",
              out_path);
    }

    obj_index = 0;
    prg_argc = input->numArgs;
    prg_argv = input->args;
    free(prg_argv[0]);
    prg_argv[0] = strdup(argv[optind]);

    klee_init_env(&prg_argc, &prg_argv);

    if (idx > 2)
      fputc('\n', stderr);
    fprintf(stderr, "KLEE-REPLAY: NOTE: Test file: %s\n"
                    "KLEE-REPLAY: NOTE: Arguments: ", input_fname);
    for (i=0; i != (unsigned) prg_argc; ++i) {
      char *s = prg_argv[i];
      if (s[0]=='A' && s[1] && !s[2]) s[1] = '\0';
      fprintf(stderr, "\"%s\" ", prg_argv[i]);
    }
    fputc('\n', stderr);

    /* Create the input files, pipes, etc. */
    replay_create_files(&__exe_fs);

    /* Run the test case machinery in a subprocess, eventually this parent
       process should be a script or something which shells out to the actual
       execution tool. */

    int pid = fork();
    if (pid < 0) {
      perror("fork");
      _exit(66);
    } else if (pid == 0) {
      /* Run the executable */
      run_monitored(executable, prg_argc, prg_argv);
      _exit(0);
    } else {
      /* Wait for the executable to finish. */
      int res, status;

      do {
        res = waitpid(pid, &status, 0);
      } while (res < 0 && errno == EINTR);

      // Delete all files in the replay directory
      replay_delete_files();

      if (res < 0) {
        perror("waitpid");
        _exit(66);
      }

      free(prg_argv);
      kTest_free(input);
    }
  }

  return 0;
}


/* KLEE functions */

int __fputc_unlocked(int c, FILE *f) {
  return fputc_unlocked(c, f);
}

int __fgetc_unlocked(FILE *f) {
  return fgetc_unlocked(f);
}

int klee_get_errno() {
  return errno;
}

void klee_warning(char *name) {
  fprintf(stderr, "KLEE-REPLAY: klee_warning: %s\n", name);
}

void klee_warning_once(char *name) {
  fprintf(stderr, "KLEE-REPLAY: klee_warning_once: %s\n", name);
}

unsigned klee_assume(uintptr_t x) {
  if (!x) {
    fputs("KLEE-REPLAY: klee_assume(0)!\n", stderr);
  }
  return 0;
}

unsigned klee_is_symbolic(uintptr_t x) {
  return 0;
}

void klee_prefer_cex(void *buffer, uintptr_t condition) {
  ;
}

void klee_posix_prefer_cex(void *buffer, uintptr_t condition) {
  ;
}

void klee_make_symbolic(void *addr, size_t nbytes, const char *name) {
  if (obj_index >= input->numObjects) {
      __emit_error("ran out of appropriate inputs");
  } else {
    KTestObject *boo = &input->objects[obj_index];
    if (boo->numBytes != nbytes) {
      fprintf(stderr, "KLEE-REPLAY: ERROR: make_symbolic mismatch, different sizes: "
              "%d in input file, %lu in code\n", boo->numBytes, (unsigned long)nbytes);
      exit(1);
    } else {
      memcpy(addr, boo->bytes, nbytes);
      obj_index++;
    }
  }
}

/* Redefined here so that we can check the value read. */
int klee_range(int start, int end, const char* name) {
  int r;

  if (start >= end) {
    fputs("KLEE-REPLAY: ERROR: klee_range: invalid range\n", stderr);
    exit(1);
  }

  if (start+1 == end)
    return start;
  else {
    klee_make_symbolic(&r, sizeof r, name);

    if (r < start || r >= end) {
      fprintf(stderr, "KLEE-REPLAY: ERROR: klee_range(%d, %d, %s) returned invalid result: %d\n",
        start, end, name, r);
      exit(1);
    }

    return r;
  }
}

void klee_report_error(const char *file, int line,
                       const char *message, const char *suffix) {
  __emit_error(message);
}

void klee_mark_global(void *object) {
  ;
}

/*** HELPER FUNCTIONS ***/

static void __emit_error(const char *msg) {
  fprintf(stderr, "KLEE-REPLAY: ERROR: %s\n", msg);
  exit(1);
}
