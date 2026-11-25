/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ac-int.h — 核心內部共用（不對外） */
#ifndef AC_INT_H
#define AC_INT_H
#define _GNU_SOURCE
#include "ac.h"
#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define AC_SETERR(buf, n, ...) do { if ((buf) && (n)) snprintf((buf), (n), __VA_ARGS__); } while (0)

int ac_run(char *const argv[], const char *stdin_data, size_t stdin_len, const char *cwd,
           int (*cb)(void *, const char *, size_t), void *cbu, char **out);
char *ac_pw_lines(const char *pw, size_t *len);
void ac_pw_free(char *s, size_t len);
AcEntry *ac_list_add(AcList *l);
const char *ac_gpg(void);

/* ac-read.c */
int ac_la_probe_read(const char *what);   /* 這個 libarchive 認不認得：1 = 有 */
int ac_check_dest_safe_tree(const char *root, int *removed);

#endif
