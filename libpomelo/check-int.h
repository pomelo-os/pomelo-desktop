/* SPDX-License-Identifier: GPL-3.0-or-later */
/* check-int.h — 健康檢查各類之間共用的內部函式（不裝進 /usr/include） */
#ifndef POMELO_CHECK_INT_H
#define POMELO_CHECK_INT_H
#include "pomelo-check.h"

#pragma GCC visibility push(hidden)

PmCheck *chk_add(PmReport *r, const char *id, int level, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void chk_detail(PmCheck *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void chk_advice(PmCheck *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void chk_fix(PmCheck *c, const char *fix, const char *arg, const char *label);
const char *chk_path(const char *path);                  /* 加上 POMELO_CHECK_ROOT（測試用） */
int chk_read(const char *path, char *out, size_t n);      /* 讀整個小檔案，去掉結尾換行；1＝成功 */
long chk_read_long(const char *path, long def);
char *chk_run(const char *cmd);                           /* pm_run：LC_ALL=C.UTF-8、丟掉 stderr；呼叫者 free */
long chk_now_ms(void);
char *chk_line(char *l);
int chk_split_colon(char *line, char **f, int max);

void chk_network(PmReport *r, int flags);
void chk_security(PmReport *r, int flags);
void chk_updates(PmReport *r, int flags);
void chk_stability(PmReport *r, int flags);
void chk_services(PmReport *r, int flags);
void chk_performance(PmReport *r, int flags);
void chk_hardware(PmReport *r, int flags);
void chk_battery(PmReport *r, int flags);
int chk_fix_argv(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);
int chk_net_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);
int chk_sec_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);
int chk_upd_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);
int chk_sys_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);
int chk_svc_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);
/* 共用：kernel 訊息（本次開機）、對網路／本機在聽的 TCP 埠 */
char *chk_kernel_log(int days);
int chk_listening(int port);
int chk_perf_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);
int chk_hw_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);
int chk_bat_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);

#pragma GCC visibility pop
#endif
