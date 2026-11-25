/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-keyring — D-Bus 服務（org.freedesktop.secrets）與主程式之間的介面 */
#ifndef SERVICE_H
#define SERVICE_H
#include <dbus/dbus.h>
#include "store.h"

/* login_pw：登入時 PAM 交來的密碼（可為 NULL）；服務會接管並在用完後清除 */
void service_init(DBusConnection *c, KStore *s, char *login_pw);
/* 解鎖視窗的結果：pw 為 NULL 表示使用者取消 */
void service_prompt_done(unsigned id, const char *pw);
int service_is_locked(void);
void service_lock(void);

/* 由主程式提供：顯示解鎖（create = 0）或建立鑰匙圈（create = 1）的視窗
 * 回傳 0 成功、-1 沒有畫面可顯示、-2 已經有一個視窗在顯示（服務會排隊） */
int ui_prompt_open(unsigned id, const char *app, int create);
void ui_prompt_error(unsigned id, const char *msg);   /* 密碼錯：視窗留著，顯示訊息 */
void ui_prompt_close(unsigned id);

#endif
