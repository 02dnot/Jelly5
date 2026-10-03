/*
 * Jelly5 — libc calls the app never needs, defined here so the link does not
 * bind them to libkernel_sys (the system-process kernel library; apps that
 * run on firmware 11.60 import only libkernel). They fail with ENOSYS.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>

struct statfs;

int fstatfs(int fd, struct statfs *buf) { (void)fd; (void)buf; errno = ENOSYS; return -1; }
int link(const char *a, const char *b) { (void)a; (void)b; errno = ENOSYS; return -1; }
int symlink(const char *a, const char *b) { (void)a; (void)b; errno = ENOSYS; return -1; }
ssize_t readlink(const char *p, char *buf, size_t n) { (void)p; (void)buf; (void)n; errno = EINVAL; return -1; }
