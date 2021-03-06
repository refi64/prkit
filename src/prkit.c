/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "prkit.h"
#include "utils.h"
#undef prkit_pid_actual_start_time

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define PRKIT_MONITOR_SEND_LENGTH \
  NLMSG_LENGTH(sizeof(struct cn_msg) + sizeof(enum proc_cn_mcast_op))
#define PRKIT_MONITOR_RECV_LENGTH \
  NLMSG_LENGTH(sizeof(struct cn_msg) + sizeof(struct proc_event))


/* We use getdents64 instead of opendir/readdir because getdents64 allows us to read multiple
   files at once + getdents64 supports directly reading an fd. */
struct linux_dirent64 {
  uint64_t       d_ino;
  int64_t        d_off;
  unsigned short d_reclen;
  unsigned char  d_type;
  char           d_name[];
};


static int getdents64(int fd, struct linux_dirent64 *dp, int size) {
  return syscall(SYS_getdents64, fd, dp, size);
}


#define startswith(s, t, o) ({ \
    size_t _len = strlen(t); \
    int _res = strncmp((s), (t), _len) == 0; \
    if (_res) { \
      *(o) = (s) + (_len); \
    } \
    _res; \
  })


// Custom getline that won't attempt to realloc *out if it's too small.
static int getline_noalloc(int fd, char **out, size_t *out_len) {
  char buf[1024];

  int requires_alloc = *out == NULL;
  cleanup(freep) char *allocated = NULL;
  char *target = requires_alloc ? malloc(1) : *out;

  size_t bytes_read = 0;
  ssize_t b = -1;

  // Please don't touch this logic. I don't know how it works, but it does. No touch. NO TOUCH.
  while ((b = read(fd, buf, sizeof(buf))) > 0) {
    void *newline = memchr(buf, '\n', b);
    int chunk = newline ? newline - (void *)buf : b;
    if (newline) {
      // Rewind the pointer to right after the newline.
      if (lseek(fd, -(b - chunk) + 1, SEEK_CUR) == -1) {
        return -errno;
      }
    }

    if (requires_alloc) {
      target = realloc(target, bytes_read + chunk + 1);
      if (target == NULL) {
        return -errno;
      }

      allocated = target;
    } else if (bytes_read + chunk + 1 > *out_len) {
      return -ERANGE;
    }

    memcpy(target + bytes_read, buf, chunk);
    bytes_read += chunk;

    if (newline) {
      break;
    }
  }

  if (b == -1) {
    return -errno;
  }

  target[bytes_read] = 0;
  stealp(&allocated);
  *out = target;
  if (out_len != NULL) {
    *out_len = bytes_read;
  }

  return 0;
}


void prkit_free_strv(char **strv) {
  for (char **p = strv; *p != NULL; p++) {
    free(*p);
  }

  free(strv);
}


void prkit_free_strvp(char ***strv) {
  if (*strv) {
    prkit_free_strv(stealp(strv));
  }
}


int prkit_open(int *out_fd) {
  int fd = openat(-1, "/proc", O_DIRECTORY|O_RDONLY);
  if (fd == -1) {
    return -errno;
  }

  *out_fd = fd;
  return 0;
}


int prkit_walk_reset(int procfd) {
  if (lseek(procfd, 0, SEEK_SET) == -1) {
    return -errno;
  }

  return 0;
}


int prkit_walk_read(int procfd, int *out_pids, size_t *out_count) {
  char buf[1024];
  size_t read = 0;

  while (read < *out_count) {
    struct linux_dirent64 *dp = (struct linux_dirent64 *)buf;
    int r = getdents64(procfd, dp, sizeof(buf));
    if (r == -1) {
      return -errno;
    } else if (r == 0) {
      break;
    }

    for (; read < *out_count && ((void *)dp - (void *)buf) < r; dp = (void *)dp + dp->d_reclen) {
      int isdir = 0;

      if (dp->d_type == DT_UNKNOWN) {
        struct stat st;
        if (fstatat(procfd, dp->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1) {
          return -errno;
        }

        isdir = S_ISDIR(st.st_mode);
      } else if (dp->d_type == DT_DIR) {
        isdir = 1;
      }

      if (!isdir) {
        continue;
      }

      char *p = NULL;
      int pid = strtol(dp->d_name, &p, 10);
      if (*p) {
        continue;
      }

      *out_pids++ = pid;
      read++;
    }
  }

  if (out_count != NULL) {
    *out_count = read;
  }

  return 0;
}


int prkit_walk_read_all(int procfd, int **out_pidsv, size_t *out_count) {
  #define PIDS 32

  cleanup(freep) int *allocated = NULL;

  size_t read = 0;

  for (;;) {
    allocated = realloc(allocated, (read + PIDS + 1) * sizeof(int));
    if (allocated == NULL) {
      return -errno;
    }

    int *pos = allocated + read;

    size_t count = PIDS;
    int r = prkit_walk_read(procfd, pos, &count);
    if (r < 0) {
      *out_pidsv = NULL;
      return r;
    } else if (count == 0) {
      break;
    }

    read += count;
  }

  allocated[read] = 0;
  *out_pidsv = stealp(&allocated);
  if (out_count != NULL) {
    *out_count = read;
  }
  return 0;
}


int prkit_kernel_cmdline(int procfd, char **out_cmdline, size_t *out_len) {
  cleanup(closep) int fd = openat(procfd, "cmdline", O_RDONLY);
  if (fd == -1) {
    return -errno;
  }

  return getline_noalloc(fd, out_cmdline, out_len);
}


int prkit_kernel_stat(int procfd, struct prkit_kernel_stat *out_kstat) {
  out_kstat->fields = 0;

  cleanup(closep) int fd = openat(procfd, "stat", O_RDONLY);
  if (fd == -1) {
    return -errno;
  }

  char buf[4096];
  char *bufp = buf;
  for (;;) {
    size_t b = sizeof(buf);
    int r = getline_noalloc(fd, &bufp, &b);
    if (r < 0 || b == 0) {
      return b;
    }

    char *p = NULL;
    if (startswith(buf, "ctxt ", &p)) {
      out_kstat->ctxt = strtol(p, &p, 10);
      out_kstat->fields |= PRKIT_KERNEL_STAT_CTXT;
    } else if (startswith(buf, "btime ", &p)) {
      out_kstat->btime = strtol(p, &p, 10);
      out_kstat->fields |= PRKIT_KERNEL_STAT_BTIME;
    } else if (startswith(buf, "processes ", &p)) {
      out_kstat->processes = strtol(p, &p, 10);
      out_kstat->fields |= PRKIT_KERNEL_STAT_PROCESSES;
    } else if (startswith(buf, "procs_running", &p)) {
      out_kstat->procs_running = strtol(p, &p, 10);
      out_kstat->fields |= PRKIT_KERNEL_STAT_PROCS_RUNNING;
    } else if (startswith(buf, "procs_blocked", &p)) {
      out_kstat->procs_blocked = strtol(p, &p, 10);
      out_kstat->fields |= PRKIT_KERNEL_STAT_PROCS_BLOCKED;
    } else {
      continue;
    }

    if (*p) {
      return -EINVAL;
    }
  }

  return 0;
}

int prkit_pid_open(int procfd, int pid, int *out_pidfd) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", pid);

  int fd = openat(procfd, buf, O_DIRECTORY|O_RDONLY);
  if (fd == -1) {
    return -errno;
  }

  *out_pidfd = fd;
  return 0;
}


static int flat_to_strv(char *line, char ***out, size_t len) {
  int items = 0;

  for (char *p = line; (p - line) < len; ) {
    size_t l = strlen(p);
    if (l) {
      items++;
      p += l + 1;
    } else {
      p++;
    }
  }

  cleanup(prkit_free_strvp) char **sv = *out = calloc(items + 1, sizeof(char *));
  if (sv == NULL) {
    return -errno;
  }

  for (char *p = line; (p - line) < len; ) {
    size_t l = strlen(p);
    if (l) {
      *sv = malloc(l + 1);
      if (*sv == NULL) {
        return -errno;
      }

      memcpy(*sv++, p, l + 1);
      p += l + 1;
    } else {
      p++;
    }
  }

  *sv = NULL;

  stealp(&sv);
  return items;
}


static int read_line_flat(int pidfd, const char *path, char **out, size_t *out_len) {
  cleanup(closep) int fd = openat(pidfd, path, O_RDONLY);
  if (fd == -1) {
    return -errno;
  }

  return getline_noalloc(fd, out, out_len);
}


static int read_line_strv(int pidfd, const char *path, char ***out) {
  cleanup(freep) char *flat = NULL;
  size_t len;
  ssize_t r = read_line_flat(pidfd, path, &flat, &len);
  if (r < 0) {
    return r;
  }

  return flat_to_strv(flat, out, len);
}


int prkit_pid_cmdline(int pidfd, char **out_cmdline, size_t *out_len) {
  return read_line_flat(pidfd, "cmdline", out_cmdline, out_len);
}


int prkit_pid_cmdline_strv(int pidfd, char ***out_cmdline_strv) {
  return read_line_strv(pidfd, "cmdline", out_cmdline_strv);
}


int prkit_pid_environ_flat(int pidfd, char **out_environ, size_t *out_len) {
  return read_line_flat(pidfd, "environ", out_environ, out_len);
}


int prkit_pid_environ_strv(int pidfd, char ***out_environ_strv) {
  return read_line_strv(pidfd, "environ", out_environ_strv);
}


static int resolve(int pidfd, const char *path, char **out, size_t *out_len) {
  cleanup(freep) char *allocated = NULL;
  char *target = *out;
  size_t len = 0;
  if (target == NULL) {
    len = PATH_MAX + 1;
    target = allocated = malloc(len);
    if (allocated == NULL) {
      return -errno;
    }
  } else {
    len = *out_len;
  }

  ssize_t r = readlinkat(pidfd, path, target, len - 1);
  if (r == -1) {
    return -errno;
  }

  target[r] = 0;
  if (allocated != NULL) {
    *out = stealp(&allocated);
    if (out_len != NULL) {
      *out_len = r;
    }
  }

  return r;
}


int prkit_pid_resolve_cwd(int pidfd, char **out_cwd, size_t *out_len) {
  return resolve(pidfd, "cwd", out_cwd, out_len);
}


int prkit_pid_resolve_exe(int pidfd, char **out_exe, size_t *out_len) {
  return resolve(pidfd, "exe", out_exe, out_len);
}


static char *skipws(char *p) {
  while (*p && isspace(*p)) p++;
  return p;
}


int prkit_pid_stat_using_buf(int pidfd, struct prkit_pid_stat *out_pstat, char **out_buf,
                             size_t *out_len) {
  cleanup(closep) int fd = openat(pidfd, "stat", O_RDONLY);

  ssize_t r = getline_noalloc(fd, out_buf, out_len);
  if (r < 0) {
    return r;
  }

  char *p = *out_buf;
  out_pstat->pid = strtol(p, &p, 10);
  p = skipws(p);

  if (*p != '(') {
    return -EINVAL;
  }

  char *comm = ++p;
  while (*p != ')') p++;
  int comm_len = p - comm;
  if (comm_len > 15) comm_len = 15;

  strncpy(out_pstat->comm, comm, comm_len);
  out_pstat->comm[comm_len] = 0;
  p++;
  p = skipws(p);

  out_pstat->state = *p++;
  p = skipws(p);

  out_pstat->ppid = strtol(p, &p, 10);
  out_pstat->pgrp = strtol(p, &p, 10);
  out_pstat->session = strtol(p, &p, 10);
  out_pstat->tty_nr = strtol(p, &p, 10);
  out_pstat->tpgid = strtol(p, &p, 10);
  out_pstat->flags = strtoul(p, &p, 10);
  out_pstat->minflt = strtoul(p, &p, 10);
  out_pstat->cminflt = strtoul(p, &p, 10);
  out_pstat->majflt = strtoul(p, &p, 10);
  out_pstat->cmajflt = strtoul(p, &p, 10);
  out_pstat->utime = strtoul(p, &p, 10);
  out_pstat->stime = strtoul(p, &p, 10);
  out_pstat->cutime = strtol(p, &p, 10);
  out_pstat->cstime = strtol(p, &p, 10);
  out_pstat->priority = strtol(p, &p, 10);
  out_pstat->nice = strtol(p, &p, 10);
  out_pstat->num_threads = strtol(p, &p, 10);
  out_pstat->itrealvalue = strtol(p, &p, 10);
  out_pstat->starttime = strtoull(p, &p, 10);
  out_pstat->vsize = strtoul(p, &p, 10);
  out_pstat->rss = strtol(p, &p, 10);
  out_pstat->rsslim = strtoul(p, &p, 10);
  out_pstat->pt_startcode = strtoul(p, &p, 10);
  out_pstat->pt_endcode = strtoul(p, &p, 10);
  out_pstat->pt_startstack = strtoul(p, &p, 10);
  out_pstat->pt_kstkesp = strtoul(p, &p, 10);
  out_pstat->pt_kstkeip = strtoul(p, &p, 10);
  out_pstat->obsolete[0] = strtoul(p, &p, 10);
  out_pstat->obsolete[1] = strtoul(p, &p, 10);
  out_pstat->obsolete[2] = strtoul(p, &p, 10);
  out_pstat->obsolete[3] = strtoul(p, &p, 10);
  out_pstat->pt_wchan = strtoul(p, &p, 10);
  out_pstat->nswap = strtoul(p, &p, 10);
  out_pstat->cnswap = strtoul(p, &p, 10);
  out_pstat->exit_signal = strtol(p, &p, 10);
  out_pstat->processor = strtol(p, &p, 10);
  out_pstat->rt_priority = strtoul(p, &p, 10);
  out_pstat->policy = strtoul(p, &p, 10);
  out_pstat->delayacct_blkio_ticks = strtoull(p, &p, 10);
  out_pstat->guest_time = strtoul(p, &p, 10);
  out_pstat->pt_start_data = strtoul(p, &p, 10);
  out_pstat->pt_end_data = strtoul(p, &p, 10);
  out_pstat->pt_start_brk = strtoul(p, &p, 10);
  out_pstat->pt_arg_start = strtoul(p, &p, 10);
  out_pstat->pt_arg_end = strtoul(p, &p, 10);
  out_pstat->pt_env_start = strtoul(p, &p, 10);
  out_pstat->pt_exit_code = strtoul(p, &p, 10);

  return 0;
}


int prkit_pid_stat(int pidfd, struct prkit_pid_stat *out_pstat) {
  cleanup(freep) char *allocated = NULL;
  return prkit_pid_stat_using_buf(pidfd, out_pstat, &allocated, NULL);
}


prkit_ulong prkit_pid_actual_start_time(const struct prkit_kernel_stat *kstat,
                                        const struct prkit_pid_stat *pstat) {
  return PRKIT_PID_ACTUAL_START_TIME(kstat, pstat);
}


int prkit_monitor_open(int *out_fd) {
  pid_t pid = getpid();

  cleanup(closep) int nlfd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_CONNECTOR);
  if (nlfd == -1) {
    return -errno;
  }

  struct sockaddr_nl nl_addr = {0};
  nl_addr.nl_family = AF_NETLINK;
  nl_addr.nl_groups = CN_IDX_PROC;
  nl_addr.nl_pid = pid;

  if (bind(nlfd, (struct sockaddr *)&nl_addr, sizeof(nl_addr)) == -1) {
    return -errno;
  }

  char buf[NLMSG_SPACE(PRKIT_MONITOR_SEND_LENGTH)] = {0};
  struct nlmsghdr *hdr = (struct nlmsghdr *)buf;
  hdr->nlmsg_len = PRKIT_MONITOR_SEND_LENGTH;
  hdr->nlmsg_type = NLMSG_DONE;
  hdr->nlmsg_pid = pid;

  struct cn_msg *msg = (struct cn_msg *)NLMSG_DATA(hdr);
  msg->id.idx = CN_IDX_PROC;
  msg->id.val = CN_VAL_PROC;

  enum proc_cn_mcast_op *op = (enum proc_cn_mcast_op *)msg->data;
  *op = PROC_CN_MCAST_LISTEN;
  msg->len = sizeof(enum proc_cn_mcast_op);

  if (send(nlfd, hdr, hdr->nlmsg_len, 0) == -1) {
    return -errno;
  }

  *out_fd = steali(&nlfd);
  return 0;
}


int prkit_monitor_read_event(int nlfd, struct proc_event *out_event) {
  char buf[NLMSG_SPACE(PRKIT_MONITOR_RECV_LENGTH)];
  struct nlmsghdr *hdr = (struct nlmsghdr *)buf;

  if (recv(nlfd, buf, NLMSG_SPACE(PRKIT_MONITOR_RECV_LENGTH), 0) == -1) {
    return -errno;
  }

  if (hdr->nlmsg_type == NLMSG_DONE) {
    memcpy(out_event, (struct proc_event *)((struct cn_msg *)NLMSG_DATA(hdr))->data,
           sizeof(*out_event));
  }

  return 0;
}
