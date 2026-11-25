/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pam_pomelo_keyring — 登入時把密碼交給 Pomelo 鑰匙圈，讓它自動解鎖
 *
 * auth 階段：記下使用者輸入的密碼（放在 PAM 的資料區，pam_end 時清掉）
 * session 階段：以使用者的身分啟動 `pomelo-keyring --stash`，把密碼從標準輸入交給它暫存；
 *               之後 pomelo-session 啟動鑰匙圈服務時再取回。
 * 這個模組一律是 optional：不管成功失敗都不影響登入。
 */
#define _GNU_SOURCE
#define PAM_SM_AUTH
#define PAM_SM_SESSION
#define PAM_SM_PASSWORD
#include <security/pam_modules.h>
#include <security/pam_ext.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>
#include <fcntl.h>

#define DATA "pomelo_keyring_password"
#define BIN "/usr/bin/pomelo-keyring"

static void cleanup(pam_handle_t *h, void *data, int err)
{
	(void)h; (void)err;
	if (data) { explicit_bzero(data, strlen(data)); free(data); }
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *h, int flags, int argc, const char **argv)
{
	(void)flags; (void)argc; (void)argv;
	const char *tok = NULL;
	if (pam_get_item(h, PAM_AUTHTOK, (const void **)&tok) != PAM_SUCCESS || !tok || !*tok) return PAM_IGNORE;
	char *copy = strdup(tok);
	if (!copy) return PAM_IGNORE;
	pam_set_data(h, DATA, copy, cleanup);
	return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *h, int flags, int argc, const char **argv)
{
	(void)h; (void)flags; (void)argc; (void)argv;
	return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t *h, int flags, int argc, const char **argv)
{
	(void)flags; (void)argc; (void)argv;
	const char *pw = NULL, *user = NULL;
	if (pam_get_data(h, DATA, (const void **)&pw) != PAM_SUCCESS || !pw || !*pw) return PAM_SUCCESS;
	if (pam_get_user(h, &user, NULL) != PAM_SUCCESS || !user) return PAM_SUCCESS;
	if (access(BIN, X_OK)) return PAM_SUCCESS;
	struct passwd *pwd = getpwnam(user);
	if (!pwd) return PAM_SUCCESS;
	/* pam_systemd 會把 XDG_RUNTIME_DIR 放進 PAM 的環境 */
	const char *rt = pam_getenv(h, "XDG_RUNTIME_DIR");
	char rtbuf[128] = "";
	if (!rt || !*rt) {
		snprintf(rtbuf, sizeof rtbuf, "/run/user/%u", (unsigned)pwd->pw_uid);
		struct stat st;
		if (!stat(rtbuf, &st) && S_ISDIR(st.st_mode)) rt = rtbuf; else rt = NULL;
	}
	int fds[2];
	if (pipe(fds)) return PAM_SUCCESS;
	pid_t pid = fork();
	if (pid < 0) { close(fds[0]); close(fds[1]); return PAM_SUCCESS; }
	if (pid == 0) {
		close(fds[1]);
		dup2(fds[0], 0);
		close(fds[0]);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
		/* 換成使用者的身分（順序：群組 → 使用者） */
		if (initgroups(pwd->pw_name, pwd->pw_gid) || setgid(pwd->pw_gid) || setuid(pwd->pw_uid)) _exit(1);
		if (chdir(pwd->pw_dir)) { /* 家目錄不在也沒關係 */ }
		clearenv();
		setenv("HOME", pwd->pw_dir, 1);
		setenv("USER", pwd->pw_name, 1);
		setenv("PATH", "/usr/bin:/bin", 1);
		if (rt) setenv("XDG_RUNTIME_DIR", rt, 1);
		execl(BIN, "pomelo-keyring", "--stash", (char *)NULL);
		_exit(127);
	}
	close(fds[0]);
	size_t n = strlen(pw), off = 0;
	while (off < n) {
		ssize_t w = write(fds[1], pw + off, n - off);
		if (w <= 0) break;
		off += (size_t)w;
	}
	close(fds[1]);
	int st;
	waitpid(pid, &st, 0);   /* --stash 收完密碼就會把自己放到背景，這裡不會等太久 */
	if (!WIFEXITED(st) || WEXITSTATUS(st)) pam_syslog(h, LOG_WARNING, "pomelo-keyring --stash failed");
	return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t *h, int flags, int argc, const char **argv)
{
	(void)h; (void)flags; (void)argc; (void)argv;
	return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_chauthtok(pam_handle_t *h, int flags, int argc, const char **argv)
{
	(void)h; (void)flags; (void)argc; (void)argv;
	return PAM_IGNORE;
}
