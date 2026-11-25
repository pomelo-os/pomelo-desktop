/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-vm — Pomelo 虛擬機器
 *
 * 用 QEMU／KVM 在這台電腦裡執行別的作業系統：建立、設定、開關機、暫停、換光碟、快照、儲存狀態。
 * 不用 libvirt：每台虛擬機器是一個資料夾（見 vm.c），QEMU 在背景執行，管理視窗關掉也不影響。
 * 畫面視窗是自己寫的 VNC 用戶端（見 console.c）。
 *
 * 用法：pomelo-vm                       虛擬機器清單
 *       pomelo-vm --show 名稱            打開某台的設定頁
 *       pomelo-vm --start 名稱           開機並顯示畫面
 *       pomelo-vm --console 名稱         顯示畫面（已經在執行的）
 *       pomelo-vm --stop 名稱            請虛擬機器關機（像按電源鍵）
 *       pomelo-vm --kill 名稱            強制關機（像拔掉電源）
 *       pomelo-vm --list                 列出虛擬機器與狀態
 */
#define _GNU_SOURCE
#include <X11/XKBlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <pomelo.h>
#include <pomelo-widgets.h>
#include <pomelo-wl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <sys/utsname.h>
#include <pulse/simple.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>

static cairo_surface_t *icon(int size);

#include "json.c"
#include "vm.c"
#include "console.c"

/* ------------------------------------------------------------------ */
/* 圖示                                                                 */
/* ------------------------------------------------------------------ */

/* 一個方塊（虛擬機器）放在螢幕裡 */
static void draw_cube(cairo_t *cr, double cx, double cy, double s)
{
	double h = s * 0.5;
	cairo_move_to(cr, cx, cy - s); cairo_line_to(cr, cx + s, cy - h); cairo_line_to(cr, cx, cy); cairo_line_to(cr, cx - s, cy - h); cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.62, 0.92, 1.0);
	cairo_fill(cr);
	cairo_move_to(cr, cx - s, cy - h); cairo_line_to(cr, cx, cy); cairo_line_to(cr, cx, cy + s); cairo_line_to(cr, cx - s, cy + h); cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.25, 0.70, 0.98);
	cairo_fill(cr);
	cairo_move_to(cr, cx + s, cy - h); cairo_line_to(cr, cx, cy); cairo_line_to(cr, cx, cy + s); cairo_line_to(cr, cx + s, cy + h); cairo_close_path(cr);
	cairo_set_source_rgb(cr, 0.12, 0.50, 0.90);
	cairo_fill(cr);
}

static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, 128);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.42, 0.36, 0.98);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.26, 0.18, 0.78);
	pm_rrect(cr, 8, 8, 112, 112, 28, 28);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	/* 螢幕 */
	pm_rrect(cr, 20, 26, 88, 60, 8, 8);
	cairo_set_source_rgb(cr, 0.97, 0.97, 1.0);
	cairo_fill(cr);
	pm_rrect(cr, 26, 32, 76, 48, 3, 3);
	cairo_set_source_rgb(cr, 0.09, 0.10, 0.20);
	cairo_fill(cr);
	draw_cube(cr, 64, 58, 15);
	/* 腳架 */
	cairo_set_source_rgb(cr, 0.97, 0.97, 1.0);
	cairo_rectangle(cr, 58, 86, 12, 9);
	cairo_fill(cr);
	pm_rrect(cr, 42, 94, 44, 7, 3.5, 3.5);
	cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

/* 清單裡每台虛擬機器的小圖示：作業系統不同顏色 */
static cairo_surface_t *vm_icon(unsigned top, unsigned bottom)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 56, 56);
	cairo_t *cr = cairo_create(s);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 0, 0, 56);
	cairo_pattern_add_color_stop_rgb(g, 0, (top >> 16) / 255.0, ((top >> 8) & 255) / 255.0, (top & 255) / 255.0);
	cairo_pattern_add_color_stop_rgb(g, 1, (bottom >> 16) / 255.0, ((bottom >> 8) & 255) / 255.0, (bottom & 255) / 255.0);
	pm_rrect(cr, 0, 0, 56, 56, 12, 12);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	pm_rrect(cr, 9, 12, 38, 26, 3, 3);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_fill(cr);
	pm_rrect(cr, 12, 15, 32, 20, 1.5, 1.5);
	cairo_set_source_rgb(cr, 0.09, 0.10, 0.20);
	cairo_fill(cr);
	draw_cube(cr, 28, 26, 6.5);
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_rectangle(cr, 25, 38, 6, 4);
	cairo_fill(cr);
	pm_rrect(cr, 18, 42, 20, 3, 1.5, 1.5);
	cairo_fill(cr);
	cairo_destroy(cr);
	return s;
}

static const char *os_icon(int os) { return os == OS_LINUX ? "@vm-linux" : os == OS_WINDOWS ? "@vm-windows" : "@vm-other"; }

/* ------------------------------------------------------------------ */
/* 背景工作：開機、快照、加大硬碟…（做完由 tick 收結果，介面不會卡住）       */
/* ------------------------------------------------------------------ */

enum { JOB_START, JOB_SNAP_TAKE, JOB_SNAP_RESTORE, JOB_SNAP_DELETE, JOB_RESIZE, JOB_FW };
typedef struct {
	int fd, kind;
	char id[32], tag[64], name[128];
	char out[4096];
	size_t n;
	int ram;
	int open_console;
} Job;

static Job jobs[16];
static int njobs;
static char status[600];          /* 最近一次的訊息（錯誤或完成） */
static char status_id[32];        /* 訊息是哪一台的（空 = 清單頁） */
static int status_err;

static void say(const Vm *v, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(status, sizeof status, fmt, ap);
	va_end(ap);
	snprintf(status_id, sizeof status_id, "%s", v ? v->id : "");
	status_err = err;
}

static Job *job_for(const Vm *v)
{
	for (int i = 0; i < njobs; i++) if (!strcmp(jobs[i].id, v->id)) return &jobs[i];
	return NULL;
}

static void spawn_self(const char *args)
{
	static char self[PATH_MAX];
	if (!self[0]) {
		ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
		if (n > 0) self[n] = 0;
	}
	char cmd[PATH_MAX + 200];
	if (self[0] && access(self, X_OK) == 0) snprintf(cmd, sizeof cmd, "exec '%s' %s", self, args);
	else snprintf(cmd, sizeof cmd, "exec pomelo-vm %s", args);
	pm_spawn(cmd);
}

static void open_console(const Vm *v)
{
	char a[64];
	snprintf(a, sizeof a, "--console %s", v->id);
	spawn_self(a);
}

static void start_vm(Vm *v, const char *loadvm)
{
	if (job_for(v) || njobs >= (int)LEN(jobs)) return;
	char err[600];
	int fd = vm_start(v, loadvm ? loadvm : v->resume[0] ? v->resume : NULL, err, sizeof err);
	if (fd < 0) { say(v, 1, "%s", err); return; }
	Job *j = &jobs[njobs++];
	memset(j, 0, sizeof *j);
	j->fd = fd;
	j->kind = JOB_START;
	j->open_console = 1;
	snprintf(j->id, sizeof j->id, "%s", v->id);
	snprintf(j->tag, sizeof j->tag, "%s", loadvm ? loadvm : v->resume);
	v->state = ST_STARTING;
	status[0] = 0;
}

/* 在子行程做 fn，做完寫一行「OK」或錯誤訊息 */
static void run_job(Vm *v, int kind, const char *tag, const char *name, int ram, int (*fn)(Vm *, const char *, char *, size_t))
{
	if (job_for(v) || njobs >= (int)LEN(jobs)) return;
	int p[2];
	if (pipe(p)) return;
	pid_t pid = fork();
	if (pid == 0) {
		close(p[0]);
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		char err[1024] = "";
		int rc = fn(v, tag, err, sizeof err);
		char line[1100];
		int l = snprintf(line, sizeof line, "%s\n", rc ? (err[0] ? err : "error") : "OK");
		if (write(p[1], line, (size_t)l) < 0) _exit(1);
		_exit(0);
	}
	close(p[1]);
	if (pid < 0) { close(p[0]); return; }
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	Job *j = &jobs[njobs++];
	memset(j, 0, sizeof *j);
	j->fd = p[0];
	j->kind = kind;
	j->ram = ram;
	snprintf(j->id, sizeof j->id, "%s", v->id);
	snprintf(j->tag, sizeof j->tag, "%s", tag ? tag : "");
	snprintf(j->name, sizeof j->name, "%s", name ? name : "");
	status[0] = 0;
}

static int fn_snap_take(Vm *v, const char *tag, char *err, size_t en)
{
	char cmd[96];
	if (!vm_pid(v)) return img_snapshot(v, "-c", tag, err, en);
	snprintf(cmd, sizeof cmd, "savevm %s", tag);
	return hmp_ok(v, cmd, 600000, err, en);
}

static int fn_snap_restore(Vm *v, const char *tag, char *err, size_t en)
{
	char cmd[96];
	if (!vm_pid(v)) return img_snapshot(v, "-a", tag, err, en);
	snprintf(cmd, sizeof cmd, "loadvm %s", tag);
	int rc = hmp_ok(v, cmd, 600000, err, en);
	qmp_simple(v, "cont");
	return rc;
}

static int fn_snap_delete(Vm *v, const char *tag, char *err, size_t en)
{
	char cmd[96];
	if (!vm_pid(v)) return img_snapshot(v, "-d", tag, err, en);
	snprintf(cmd, sizeof cmd, "delvm %s", tag);
	return hmp_ok(v, cmd, 600000, err, en);
}

static int fn_resize(Vm *v, const char *add, char *err, size_t en)
{
	char disk[PATH_MAX], out[1024];
	vm_disk(v, disk, sizeof disk);
	char *argv[] = { "qemu-img", "resize", disk, (char *)add, NULL };
	if (!run_argv(argv, out, sizeof out)) return 0;
	last_line(out);
	snprintf(err, en, "%s", out);
	return -1;
}

/* 換了安全開機：重做 UEFI 變數檔（開機選項會回到預設） */
static int fn_fw(Vm *v, const char *tag, char *err, size_t en) { return vm_prepare_fw(v, 1, err, en); }

static void job_done(Job *j)
{
	Vm *v = vm_find(j->id);
	char *nl = strchr(j->out, '\n');
	if (j->kind != JOB_START && nl) *nl = 0;
	int ok = j->kind == JOB_START ? 0 : !strcmp(j->out, "OK");
	if (!v) return;
	switch (j->kind) {
	case JOB_START:
		v->state = ST_OFF;
		if (vm_pid(v)) {
			ok = 1;
			if (v->resume[0] && !strcmp(j->tag, v->resume)) {   /* 從儲存的狀態繼續了：那份狀態用掉了 */
				v->resume[0] = 0;
				vm_save(v);
				run_job(v, JOB_SNAP_DELETE, RESUME_TAG, NULL, 0, fn_snap_delete);
			}
			if (j->open_console) open_console(v);
		} else {
			explain_qemu_error(j->out, sizeof j->out);
			say(v, 1, "%s：%s", _("無法開機"), j->out[0] ? j->out : _("QEMU 沒有說原因"));
		}
		vm_refresh(v);
		break;
	case JOB_SNAP_TAKE:
		if (ok) {
			Snap s[MAX_SNAPS];
			int n = snaps_load(v, s, MAX_SNAPS);
			if (n >= MAX_SNAPS) n = MAX_SNAPS - 1;
			memset(&s[n], 0, sizeof s[n]);
			snprintf(s[n].tag, sizeof s[n].tag, "%s", j->tag);
			snprintf(s[n].name, sizeof s[n].name, "%s", j->name);
			s[n].t = (long long)time(NULL);
			s[n].ram = j->ram;
			snaps_save(v, s, n + 1);
			say(v, 0, _("拍好了：%s"), j->name);
		} else say(v, 1, "%s：%s", _("無法拍快照"), j->out);
		break;
	case JOB_SNAP_RESTORE:
		if (ok) say(v, 0, _("已經回到「%s」"), j->name);
		else say(v, 1, "%s：%s", _("無法回到快照"), j->out);
		break;
	case JOB_SNAP_DELETE:
		if (ok || strstr(j->out, "not found") || strstr(j->out, "does not exist")) {
			Snap s[MAX_SNAPS];
			int n = snaps_load(v, s, MAX_SNAPS), k = 0;
			for (int i = 0; i < n; i++) if (strcmp(s[i].tag, j->tag)) s[k++] = s[i];
			if (k != n) snaps_save(v, s, k);
		} else say(v, 1, "%s：%s", _("無法刪除快照"), j->out);
		break;
	case JOB_RESIZE:
		if (ok) say(v, 0, "%s", _("硬碟加大了。開機後還要在虛擬機器裡把分割區延伸，才用得到多出來的空間。"));
		else say(v, 1, "%s：%s", _("無法加大硬碟"), j->out);
		break;
	case JOB_FW:
		if (ok) vm_save(v);
		else say(v, 1, "%s", j->out);
		break;
	}
}

static int poll_jobs(void)
{
	int changed = 0;
	for (int i = 0; i < njobs; i++) {
		Job *j = &jobs[i];
		for (;;) {
			char buf[1024];
			ssize_t r = read(j->fd, buf, sizeof buf);
			if (r > 0) {
				size_t k = MIN((size_t)r, sizeof j->out - 1 - j->n);
				memcpy(j->out + j->n, buf, k);
				j->n += k;
				j->out[j->n] = 0;
				continue;
			}
			if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
			/* EOF：做完了 */
			close(j->fd);
			Job done = *j;
			memmove(j, j + 1, (size_t)(njobs - i - 1) * sizeof *j);
			njobs--;
			i--;
			job_done(&done);
			changed = 1;
			break;
		}
	}
	return changed;
}

/* ------------------------------------------------------------------ */
/* 安裝 QEMU、KVM 權限（開終端機執行，看得到進度與密碼提示）                  */
/* ------------------------------------------------------------------ */

/* 譯文要放進 shell 的單引號裡：撇號（法文、義大利文常見）換成全形的 ’ */
static const char *sh_safe(const char *s)
{
	static char buf[2][600];
	static int k;
	char *o = buf[k ^= 1];
	size_t n = 0;
	for (; *s && n + 4 < sizeof buf[0]; s++) {
		if (*s == '\'') { memcpy(o + n, "’", 3); n += 3; }
		else o[n++] = *s;
	}
	o[n] = 0;
	return o;
}

static void run_in_terminal(const char *title, const char *cmd)
{
	char full[1600];
	snprintf(full, sizeof full, "%s; echo; echo '%s'; read x", cmd, sh_safe(_("完成，按 Enter 關閉這個視窗")));
	if (fork() == 0) {
		if (fork() == 0) {
			setsid();
			if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
			execlp("pomelo-terminal", "pomelo-terminal", "-t", title, "-e", "sh", "-c", full, (char *)NULL);
			execlp("x-terminal-emulator", "x-terminal-emulator", "-e", "sh", "-c", full, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
}

/* 只裝執行虛擬機器需要的部分：Ubuntu 的 qemu-system-x86 建議套件會帶進 GTK 介面（qemu-system-gui）等，不裝 */
static const char *qemu_packages(void)
{
	if (pm_pkg_kind() == PM_PKG_DNF) {
		if (pm_os_like("rhel") || pm_os_like("centos"))
			return "qemu-kvm-core qemu-img edk2-ovmf qemu-kvm-device-display-virtio-vga qemu-kvm-audio-pa qemu-kvm-device-usb-host";
		return "qemu-system-x86-core qemu-img edk2-ovmf qemu-device-display-virtio-vga qemu-audio-pa qemu-device-usb-host";
	}
	return "qemu-system-x86 qemu-utils ovmf";
}

/* ARM64 模擬器與它的 UEFI 韌體；Rocky／RHEL 沒有 */
static const char *qemu_arm_packages(void)
{
	if (pm_pkg_kind() == PM_PKG_DNF) return pm_os_like("rhel") || pm_os_like("centos") ? NULL : "qemu-system-aarch64-core edk2-aarch64";
	return "qemu-system-arm qemu-efi-aarch64";
}

static void install_pkgs(const char *pkgs)
{
	char cmd[600];
	int dnf = pm_pkg_kind() == PM_PKG_DNF;
	snprintf(cmd, sizeof cmd, "sudo %s install -y %s %s", dnf ? "dnf" : "apt-get",
	         dnf ? "--setopt=install_weak_deps=False" : "--no-install-recommends", pkgs);
	run_in_terminal(_("安裝"), cmd);
}

/* ------------------------------------------------------------------ */
/* 選項                                                                 */
/* ------------------------------------------------------------------ */

static const int mem_mb[] = { 512, 1024, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536 };
static const int disk_gb[] = { 16, 32, 64, 128, 256, 512, 1024 };

static long host_mb(void) { return sysconf(_SC_PHYS_PAGES) / 1024 * sysconf(_SC_PAGESIZE) / 1024; }

static void fmt_mb(char *out, size_t n, int mb)
{
	if (mb < 1024) snprintf(out, n, "%d MB", mb);
	else if (mb % 1024) snprintf(out, n, "%.1f GB", mb / 1024.0);
	else snprintf(out, n, "%d GB", mb / 1024);
}

/* 記憶體：最多到這台電腦的記憶體減 1 GB */
static int ui_mem(PmUI *u, int *mem)
{
	static char labels[LEN(mem_mb) + 1][24];
	const char *opts[LEN(mem_mb) + 1];
	int vals[LEN(mem_mb) + 1], n = 0, cur = -1;
	long max = MAX(1024, host_mb() - 1024);
	for (size_t i = 0; i < LEN(mem_mb); i++) {
		if (mem_mb[i] > max && mem_mb[i] != *mem) continue;
		if (cur < 0 && mem_mb[i] > *mem) { vals[n] = *mem; cur = n++; }   /* 設定檔裡的特殊大小 */
		if (mem_mb[i] == *mem) cur = n;
		vals[n++] = mem_mb[i];
	}
	if (cur < 0) { vals[n] = *mem; cur = n++; }
	for (int i = 0; i < n; i++) { fmt_mb(labels[i], sizeof labels[i], vals[i]); opts[i] = labels[i]; }
	int v = cur;
	if (!pm_ui_choice(u, _("記憶體"), opts, n, &v)) return 0;
	*mem = vals[v];
	return 1;
}

static int ui_cpus(PmUI *u, int *cpus)
{
	static char labels[64][8];
	const char *opts[64];
	int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN), n = MIN(MAX(ncpu, *cpus), 64);
	for (int i = 0; i < n; i++) { snprintf(labels[i], sizeof labels[i], "%d", i + 1); opts[i] = labels[i]; }
	int v = CLAMP(*cpus, 1, n) - 1;
	if (!pm_ui_choice(u, _("處理器核心"), opts, n, &v)) return 0;
	*cpus = v + 1;
	return 1;
}

static int ui_os(PmUI *u, int *os)
{
	const char *opts[] = { "Linux", "Windows", _("其他") };
	return pm_ui_choice(u, _("作業系統"), opts, 3, os);
}

static void human_bytes(char *out, size_t n, unsigned long long b)
{
	if (b >= (1ULL << 30)) snprintf(out, n, "%.1f GB", b / 1073741824.0);
	else if (b >= (10ULL << 20)) snprintf(out, n, "%.0f MB", b / 1048576.0);
	else snprintf(out, n, "%.1f MB", b / 1048576.0);
}

static unsigned long long free_space(void)
{
	char root[PATH_MAX];
	data_root(root, sizeof root);
	mkdirs(root, 0700);
	struct statvfs st;
	if (statvfs(root, &st)) return 0;
	return (unsigned long long)st.f_bavail * st.f_frsize;
}

/* 從光碟映像的檔名猜作業系統 */
static int guess_os(const char *iso)
{
	char b[256];
	snprintf(b, sizeof b, "%s", base_name(iso));
	for (char *p = b; *p; p++) *p = (char)tolower((unsigned char)*p);
	if (strstr(b, "win")) return OS_WINDOWS;
	static const char *lx[] = { "ubuntu", "debian", "fedora", "mint", "linux", "centos", "rocky", "alma", "arch", "manjaro",
	                            "opensuse", "suse", "kali", "pop", "elementary", "zorin", "pomelo", "rhel", "kubuntu", "xubuntu",
	                            "lubuntu", "mx-", "endeavour", "gentoo", "nixos", "void", "alpine", "deepin", "slackware" };
	for (size_t i = 0; i < LEN(lx); i++) if (strstr(b, lx[i])) return OS_LINUX;
	return -1;
}

static int guess_arch(const char *iso)
{
	char b[256];
	snprintf(b, sizeof b, "%s", base_name(iso));
	for (char *p = b; *p; p++) *p = (char)tolower((unsigned char)*p);
	if (strstr(b, "aarch64") || strstr(b, "arm64")) return ARCH_ARM;
	if (strstr(b, "amd64") || strstr(b, "x86_64") || strstr(b, "x64")) return ARCH_X86;
	return -1;
}

/* 光碟映像的檔名 → 預設的虛擬機器名稱：拿掉副檔名、常見的架構與版本字尾 */
static void name_from_iso(char *out, size_t n, const char *iso)
{
	snprintf(out, n, "%s", base_name(iso));
	char *dot = strrchr(out, '.');
	if (dot && dot != out) *dot = 0;
	static const char *tails[] = { "-amd64", "_amd64", "-x86_64", ".x86_64", "_x64", "-x64", "-arm64", "_arm64", "-aarch64", ".aarch64",
	                               "-dvd", "-live", "-desktop", "-server", "-netinst" };
	for (int again = 1; again;) {
		again = 0;
		size_t l = strlen(out);
		for (size_t i = 0; i < LEN(tails); i++) {
			size_t tl = strlen(tails[i]);
			if (l > tl + 2 && !strcasecmp(out + l - tl, tails[i])) { out[l - tl] = 0; again = 1; break; }
		}
	}
}

static int pick_file(const char *title, int iso, const char *cur, char *out, size_t n)
{
	static const PmFileType iso_types[] = { { N_("光碟映像（.iso）"), "iso;img" } };
	static const PmFileType disk_types[] = { { N_("虛擬硬碟（.qcow2、.img、.raw、.vmdk、.vdi、.vhdx）"), "qcow2;img;raw;vmdk;vdi;vhdx" } };
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", cur ? cur : "");
	char *sl = strrchr(dir, '/');
	if (sl) *sl = 0; else dir[0] = 0;
	if (!dir[0]) {
		const char *home = getenv("HOME");
		snprintf(dir, sizeof dir, "%s/%s", home ? home : "/", "Downloads");
		if (access(dir, R_OK)) snprintf(dir, sizeof dir, "%s", home ? home : "/");
	}
	int type = 0;
	return pm_file_dialog(title, "pomelo-vm", icon, 0, dir, NULL, iso ? iso_types : disk_types, 1, &type, out, n);
}

/* ------------------------------------------------------------------ */
/* 使用量：CPU、記憶體（看 QEMU 行程）、磁碟（QMP query-blockstats）、網路     */
/* ------------------------------------------------------------------ */
/* 網路：橋接時看 tap 介面的計數；NAT 時 QEMU 用自己的 socket 幫虛擬機器連出去，
 * 加總那些 TCP socket 收送的位元組（sock_diag）。UDP（DNS、QUIC）算不到，所以 NAT 的數字是概略的。 */

#define HIST 60
typedef struct { unsigned long ino; unsigned long long in, out; } SockCnt;
typedef struct {
	char id[32];
	pid_t pid;
	long last_ms;
	unsigned long long ticks, rd, wr, rx, tx;
	double cpu[HIST], rdr[HIST], wrr[HIST], rxr[HIST], txr[HIST];
	long long rss;
	int n;
	char tap[32];
	SockCnt *socks;
	int nsocks;
} Stats;

static Stats stats[MAX_VMS];
static int nstats;

static Stats *stats_for(const char *id)
{
	for (int i = 0; i < nstats; i++) if (!strcmp(stats[i].id, id)) return &stats[i];
	if (nstats >= MAX_VMS) return NULL;
	Stats *st = &stats[nstats++];
	memset(st, 0, sizeof *st);
	snprintf(st->id, sizeof st->id, "%s", id);
	return st;
}

static void push(double *h, double v)
{
	memmove(h, h + 1, (HIST - 1) * sizeof *h);
	h[HIST - 1] = v;
}

static unsigned long long proc_ticks(pid_t pid)
{
	char p[64], buf[1024];
	snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
	FILE *f = fopen(p, "r");
	if (!f) return 0;
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	char *e = strrchr(buf, ')');
	unsigned long long ut = 0, st = 0;
	/* ) 後面：狀態、ppid…，utime、stime 是第 14、15 欄 */
	if (e) sscanf(e + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st);
	return ut + st;
}

static long long proc_rss(pid_t pid)
{
	char p[64], line[256];
	snprintf(p, sizeof p, "/proc/%d/status", (int)pid);
	FILE *f = fopen(p, "r");
	long long kb = 0;
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, "VmRSS:", 6)) { kb = atoll(line + 6); break; }
	if (f) fclose(f);
	return kb * 1024;
}

static int cmp_ul(const void *a, const void *b)
{
	unsigned long x = *(const unsigned long *)a, y = *(const unsigned long *)b;
	return x < y ? -1 : x > y;
}

/* QEMU 開著的 socket（inode，排序好）；tap：橋接時順便找出 tap 介面的名稱 */
static int proc_sockets(pid_t pid, unsigned long *out, int max, char *tap, size_t tapn)
{
	char dir[64];
	snprintf(dir, sizeof dir, "/proc/%d/fd", (int)pid);
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;
	while (d && (e = readdir(d))) {
		char p[128], l[64];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		ssize_t k = readlink(p, l, sizeof l - 1);
		if (k <= 0) continue;
		l[k] = 0;
		unsigned long ino;
		if (sscanf(l, "socket:[%lu]", &ino) == 1 && n < max) out[n++] = ino;
		else if (tap && !tap[0] && !strcmp(l, "/dev/net/tun")) {
			char fi[128], line[128];
			snprintf(fi, sizeof fi, "/proc/%d/fdinfo/%s", (int)pid, e->d_name);
			FILE *f = fopen(fi, "r");
			while (f && fgets(line, sizeof line, f))
				if (!strncmp(line, "iff:", 4)) { sscanf(line + 4, "%31s", tap); break; }
			if (f) fclose(f);
			(void)tapn;
		}
	}
	if (d) closedir(d);
	qsort(out, (size_t)n, sizeof *out, cmp_ul);
	return n;
}

/* 列出系統所有 TCP socket，挑出 inode 在清單裡的，回傳個數（ino、累計收、送） */
static int tcp_counts(const unsigned long *inos, int nino, SockCnt *out, int max)
{
	int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
	if (fd < 0) return 0;
	int n = 0;
	static const int fams[] = { AF_INET, AF_INET6 };
	for (size_t fi = 0; fi < LEN(fams); fi++) {
		struct { struct nlmsghdr nlh; struct inet_diag_req_v2 r; } req;
		memset(&req, 0, sizeof req);
		req.nlh.nlmsg_len = sizeof req;
		req.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
		req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
		req.r.sdiag_family = (unsigned char)fams[fi];
		req.r.sdiag_protocol = IPPROTO_TCP;
		req.r.idiag_ext = 1 << (INET_DIAG_INFO - 1);
		req.r.idiag_states = ~0U;
		if (send(fd, &req, sizeof req, 0) < 0) break;
		static char buf[32768];
		for (int done = 0; !done;) {
			struct pollfd pf = { fd, POLLIN, 0 };
			if (poll(&pf, 1, 500) <= 0) break;
			ssize_t len = recv(fd, buf, sizeof buf, 0);
			if (len <= 0) break;
			for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (size_t)len); h = NLMSG_NEXT(h, len)) {
				if (h->nlmsg_type == NLMSG_DONE || h->nlmsg_type == NLMSG_ERROR) { done = 1; break; }
				struct inet_diag_msg *m = NLMSG_DATA(h);
				unsigned long ino = m->idiag_inode;
				if (!bsearch(&ino, inos, (size_t)nino, sizeof *inos, cmp_ul) || n >= max) continue;
				int alen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof *m));
				for (struct rtattr *a = (struct rtattr *)(m + 1); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
					if (a->rta_type != INET_DIAG_INFO) continue;
					struct tcp_info *ti = RTA_DATA(a);
					if (RTA_PAYLOAD(a) < (int)(offsetof(struct tcp_info, tcpi_bytes_received) + sizeof ti->tcpi_bytes_received)) break;
					out[n++] = (SockCnt){ ino, ti->tcpi_bytes_received, ti->tcpi_bytes_acked };
				}
			}
		}
	}
	close(fd);
	return n;
}

static unsigned long long sys_counter(const char *ifname, const char *what)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "/sys/class/net/%s/statistics/%s", ifname, what);
	FILE *f = fopen(p, "r");
	unsigned long long v = 0;
	if (f) { if (fscanf(f, "%llu", &v) != 1) v = 0; fclose(f); }
	return v;
}

/* 網路的累計收、送（虛擬機器的角度：rx = 下載） */
static void net_counts(Stats *st, const Vm *v, unsigned long long *rx, unsigned long long *tx)
{
	unsigned long inos[512];
	int nino = proc_sockets(st->pid, inos, LEN(inos), v->net == NET_BRIDGE ? st->tap : NULL, sizeof st->tap);
	if (v->net == NET_BRIDGE) {
		*rx = st->tap[0] ? sys_counter(st->tap, "tx_bytes") : 0;   /* tap 送出去的 = 虛擬機器收到的 */
		*tx = st->tap[0] ? sys_counter(st->tap, "rx_bytes") : 0;
		return;
	}
	*rx = st->rx; *tx = st->tx;
	if (v->net != NET_NAT) return;
	SockCnt cur[512];
	int n = tcp_counts(inos, nino, cur, LEN(cur));
	for (int i = 0; i < n; i++) {
		unsigned long long pin = 0, pout = 0;
		for (int k = 0; k < st->nsocks; k++)
			if (st->socks[k].ino == cur[i].ino) { pin = st->socks[k].in; pout = st->socks[k].out; break; }
		*rx += cur[i].in >= pin ? cur[i].in - pin : cur[i].in;   /* 新的連線：整個都算在這一秒 */
		*tx += cur[i].out >= pout ? cur[i].out - pout : cur[i].out;
	}
	free(st->socks);
	st->socks = n ? malloc((size_t)n * sizeof *cur) : NULL;
	if (st->socks) memcpy(st->socks, cur, (size_t)n * sizeof *cur);
	st->nsocks = st->socks ? n : 0;
}

static void stats_sample(Vm *v)
{
	Stats *st = stats_for(v->id);
	if (!st) return;
	pid_t pid = v->state == ST_RUNNING || v->state == ST_PAUSED ? vm_pid(v) : 0;
	if (!pid) { free(st->socks); memset(st, 0, sizeof *st); snprintf(st->id, sizeof st->id, "%s", v->id); return; }
	if (pid != st->pid) {   /* 重新開機了：從頭算 */
		free(st->socks);
		memset(st, 0, sizeof *st);
		snprintf(st->id, sizeof st->id, "%s", v->id);
		st->pid = pid;
	}
	long now = pm_now_ms();
	unsigned long long ticks = proc_ticks(pid), rd = 0, wr = 0, rx = 0, tx = 0;
	char *r = qmp_call(v, "{\"execute\":\"query-blockstats\"}", 300);
	Js *j = js_parse(r);
	for (Js *b = js_get(j, "return") ? js_get(j, "return")->kids : NULL; b; b = b->next) {
		Js *s = js_get(b, "stats");
		rd += (unsigned long long)js_num(js_get(s, "rd_bytes"), 0);
		wr += (unsigned long long)js_num(js_get(s, "wr_bytes"), 0);
	}
	int have_blk = j != NULL;
	js_free(j);
	free(r);
	if (!have_blk) { rd = st->rd; wr = st->wr; }   /* 忙著（拍快照）：這一秒當作沒有 */
	net_counts(st, v, &rx, &tx);
	st->rss = proc_rss(pid);
	if (st->last_ms) {
		double dt = (now - st->last_ms) / 1000.0;
		if (dt < 0.2) return;
		static long hz;
		if (!hz) hz = sysconf(_SC_CLK_TCK);
		push(st->cpu, CLAMP((ticks - st->ticks) / (double)hz / dt * 100.0 / MAX(1, v->cpus), 0, 100));
		push(st->rdr, rd >= st->rd ? (rd - st->rd) / dt : 0);
		push(st->wrr, wr >= st->wr ? (wr - st->wr) / dt : 0);
		push(st->rxr, rx >= st->rx ? (rx - st->rx) / dt : 0);
		push(st->txr, tx >= st->tx ? (tx - st->tx) / dt : 0);
		if (st->n < HIST) st->n++;
	}
	st->last_ms = now;
	st->ticks = ticks; st->rd = rd; st->wr = wr; st->rx = rx; st->tx = tx;
}

static double peak(const double *h, const double *h2)
{
	double m = 0;
	for (int i = 0; i < HIST; i++) m = MAX(m, h[i] + (h2 ? h2[i] : 0));
	return m;
}

static void fmt_rate(char *out, size_t n, double bps)
{
	if (bps >= 1e6) snprintf(out, n, "%.1f MB/s", bps / 1e6);
	else if (bps >= 1e3) snprintf(out, n, "%.0f KB/s", bps / 1e3);
	else snprintf(out, n, "%.0f B/s", bps);
}

/* ------------------------------------------------------------------ */
/* 介面                                                                 */
/* ------------------------------------------------------------------ */

enum { PG_LIST, PG_NEW, PG_VM };
static int page;
static char cur_id[32];           /* PG_VM 顯示哪一台 */
static Vm saved;                  /* PG_VM 上次存檔的設定（有改就存） */
static int confirm_delete, confirm_kill, confirm_discard, snap_sel = -1, resize_choice;
static char snap_name[128];
static ImgInfo disk_ii;
static int disk_ii_ok;
static cairo_surface_t *preview;
static char preview_id[32];
static time_t preview_mtime;
static long last_reload;
static long list_click_ms;        /* 在清單點了一下（換到設定頁）的時間：緊接著的第二下當作在清單上點兩下 */

/* 新增虛擬機器 */
static Vm nv;
static int nv_disk = 1, nv_existing;
static char nv_autoname[128];

static void preview_load(const Vm *v)
{
	char path[PATH_MAX];
	vm_file(v, "screen.png", path, sizeof path);
	struct stat st;
	time_t mt = stat(path, &st) ? 0 : st.st_mtime;
	if (preview && !strcmp(preview_id, v->id) && mt == preview_mtime) return;
	if (preview) cairo_surface_destroy(preview);
	preview = mt ? pm_image_load(path) : NULL;
	snprintf(preview_id, sizeof preview_id, "%s", v->id);
	preview_mtime = mt;
}

static PmUI *ui;   /* 換頁時要立刻重畫（不然要等下一個事件才看得到新的一頁） */

static void go(int pg)
{
	page = pg;
	if (ui) ui->again = 1;
}

static void open_vm(Vm *v)
{
	go(PG_VM);
	snprintf(cur_id, sizeof cur_id, "%s", v->id);
	saved = *v;
	confirm_delete = confirm_kill = confirm_discard = 0;
	snap_sel = -1;
	resize_choice = 0;
	char disk[PATH_MAX];
	vm_disk(v, disk, sizeof disk);
	disk_ii_ok = !img_info(disk, &disk_ii);
	if (strcmp(status_id, v->id)) status[0] = 0;
}

static void start_new(void)
{
	memset(&nv, 0, sizeof nv);
	vm_defaults(&nv, OS_LINUX);
	nv_disk = 1;   /* 32 GB */
	nv_existing = 0;
	nv_autoname[0] = 0;
	status[0] = 0;
	go(PG_NEW);
}

static void status_note(PmUI *u, const Vm *v)
{
	if (!status[0] || strcmp(status_id, v ? v->id : "")) return;
	char t[640];
	snprintf(t, sizeof t, "%s%s", status_err ? "⚠ " : "", status);
	pm_ui_note(u, t);
}

/* 開機前要確認的環境（QEMU、KVM），回傳 1 = 可以開機 */
static int env_notes(PmUI *u)
{
	if (!qemu_bin()) {
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, _("要先安裝 QEMU 才能執行虛擬機器"), _("安裝"))) install_pkgs(qemu_packages());
		pm_ui_end(u);
		pm_ui_note(u, _("會開一個終端機視窗安裝（要輸入你的密碼），只裝執行虛擬機器需要的部分。裝好後這裡會自動更新。"));
		return 0;
	}
	int k = kvm_status();
	if (k == KVM_PERM) {
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, _("你的帳號還不能使用硬體虛擬化（KVM）"), _("啟用"))) run_in_terminal(_("啟用 KVM"), "sudo usermod -aG kvm \"$USER\"");
		pm_ui_end(u);
		pm_ui_note(u, _("按「啟用」把你加進 kvm 群組，登出再登入後生效。沒有 KVM 也能執行，只是會慢很多。"));
	} else if (k == KVM_NODEV) {
		pm_ui_note(u, cpu_has_virt() ? _("⚠ 硬體虛擬化（KVM）沒有啟用：虛擬機器還是能跑，但會慢很多。可能是 BIOS 裡的 Intel VT-x／AMD-V 沒有開。")
		                             : _("⚠ 這台電腦沒有硬體虛擬化（KVM），或是本身就是虛擬機器：虛擬機器還是能跑，但會慢很多。"));
	}
	return 1;
}

static void build_list(PmUI *u)
{
	pm_ui_title(u, _("虛擬機器"), _("在這台電腦裡執行別的作業系統"));
	env_notes(u);
	if (nvms) {
		pm_ui_group(u, _("我的虛擬機器"));
		for (int i = 0; i < nvms; i++) {
			Vm *v = &vms[i];
			char sub[300], mem[24];
			fmt_mb(mem, sizeof mem, v->mem);
			snprintf(sub, sizeof sub, _("%s · 記憶體 %s · %d 核心"), os_label(v->os), mem, v->cpus);
			Stats *st = stats_for(v->id);
			if (st && st->n && (v->state == ST_RUNNING || v->state == ST_PAUSED)) {
				char used[24], disk[24], net[24];
				human_bytes(used, sizeof used, (unsigned long long)st->rss);
				fmt_rate(disk, sizeof disk, st->rdr[HIST - 1] + st->wrr[HIST - 1]);
				fmt_rate(net, sizeof net, st->rxr[HIST - 1] + st->txr[HIST - 1]);
				snprintf(sub, sizeof sub, _("CPU %.0f%% · 記憶體 %s／%s · 磁碟 %s · 網路 %s"), st->cpu[HIST - 1], used, mem, disk, net);
			}
			if (pm_ui_item(u, os_icon(v->os), v->name, sub, state_label(v->state), 0)) {
				if (u->dbl) {
					if (v->state == ST_RUNNING || v->state == ST_PAUSED) open_console(v);
					else if (v->state != ST_STARTING && qemu_bin()) start_vm(v, NULL);
				} else { open_vm(v); list_click_ms = pm_now_ms(); }
			}
		}
		pm_ui_end(u);
	}
	pm_ui_group(u, NULL);
	if (pm_ui_button(u, nvms ? _("再建立一台虛擬機器") : _("建立第一台虛擬機器，準備好作業系統的安裝光碟（.iso）"), _("新增虛擬機器"))) start_new();
	pm_ui_end(u);
	if (nvms) pm_ui_note(u, _("點一下看設定，點兩下開機或顯示畫面。關掉這個視窗不會影響正在執行的虛擬機器。"));
	status_note(u, NULL);
}

static void build_new(PmUI *u)
{
	if (pm_ui_back(u, _("虛擬機器"))) { go(PG_LIST); return; }
	pm_ui_title(u, _("新增虛擬機器"), NULL);

	pm_ui_group(u, NULL);
	pm_ui_entry(u, _("名稱"), nv.name, sizeof nv.name, 0);
	if (pm_ui_item(u, NULL, _("安裝光碟"), nv.iso[0] ? base_name(nv.iso) : _("還沒選（可以之後再放）"), _("選擇… ›"), 0)) {
		char path[PATH_MAX];
		if (pick_file(_("選擇安裝光碟"), 1, nv.iso, path, sizeof path)) {
			snprintf(nv.iso, sizeof nv.iso, "%s", path);
			int arch = guess_arch(path);
			if (arch >= 0) vm_set_arch(&nv, arch);
			int os = guess_os(path);
			if (os >= 0 && os != nv.os) {
				char name[128], iso[PATH_MAX];
				snprintf(name, sizeof name, "%s", nv.name);
				snprintf(iso, sizeof iso, "%s", nv.iso);
				int keep = nv.arch;
				vm_defaults(&nv, os);
				vm_set_arch(&nv, keep);
				snprintf(nv.name, sizeof nv.name, "%s", name);
				snprintf(nv.iso, sizeof nv.iso, "%s", iso);
				nv_disk = os == OS_WINDOWS ? 2 : 1;
			}
			if (!nv.name[0] || !strcmp(nv.name, nv_autoname)) {
				name_from_iso(nv_autoname, sizeof nv_autoname, path);
				snprintf(nv.name, sizeof nv.name, "%s", nv_autoname);
			}
		}
		u->again = 1;
	}
	int os = nv.os;
	if (ui_os(u, &os)) {
		char name[128], iso[PATH_MAX];
		snprintf(name, sizeof name, "%s", nv.name);
		snprintf(iso, sizeof iso, "%s", nv.iso);
		int keep = nv.arch;
		vm_defaults(&nv, os);
		vm_set_arch(&nv, keep);
		snprintf(nv.name, sizeof nv.name, "%s", name);
		snprintf(nv.iso, sizeof nv.iso, "%s", iso);
		nv_disk = os == OS_WINDOWS ? 2 : 1;
	}
	int arch = nv.arch;
	const char *aopts[] = { "x86_64", "ARM64" };
	if (pm_ui_segment(u, _("處理器架構"), aopts, 2, &arch)) {
		if (arch == ARCH_X86) {   /* 從 ARM 換回來：硬體設定回到這個作業系統的建議值 */
			char name[128], iso[PATH_MAX];
			snprintf(name, sizeof name, "%s", nv.name);
			snprintf(iso, sizeof iso, "%s", nv.iso);
			vm_defaults(&nv, nv.os);
			snprintf(nv.name, sizeof nv.name, "%s", name);
			snprintf(nv.iso, sizeof nv.iso, "%s", iso);
		}
		vm_set_arch(&nv, arch);
	}
	if (nv.arch == ARCH_ARM && !qemu_arm_bin()) {
		const char *pk = qemu_arm_packages();
		if (!pk) pm_ui_info(u, _("這個系統沒有 ARM64 模擬器"), NULL);
		else if (pm_ui_button(u, _("要先安裝 ARM64 模擬器"), _("安裝"))) install_pkgs(pk);
	}
	pm_ui_end(u);
	pm_ui_note(u, nv.arch == ARCH_ARM && host_arch() != ARCH_ARM
	              ? _("ARM64 在這台電腦上是用軟體模擬的，比 x86_64 慢很多（大約 5～20 倍），適合裝伺服器或測試，不適合跑桌面。")
	              : _("選了光碟會從檔名猜作業系統與架構，下面的建議設定跟著換。"));

	pm_ui_group(u, _("硬體"));
	ui_mem(u, &nv.mem);
	ui_cpus(u, &nv.cpus);
	pm_ui_switch(u, _("使用現有的虛擬硬碟"), &nv_existing);
	if (nv_existing) {
		if (pm_ui_item(u, NULL, _("虛擬硬碟"), nv.disk[0] ? base_name(nv.disk) : _("還沒選"), _("選擇… ›"), 0)) {
			char path[PATH_MAX];
			if (pick_file(_("選擇虛擬硬碟"), 0, nv.disk, path, sizeof path)) snprintf(nv.disk, sizeof nv.disk, "%s", path);
			u->again = 1;
		}
	} else {
		static char labels[LEN(disk_gb)][24];
		const char *opts[LEN(disk_gb)];
		for (size_t i = 0; i < LEN(disk_gb); i++) {
			if (disk_gb[i] >= 1024) snprintf(labels[i], sizeof labels[i], "%d TB", disk_gb[i] / 1024);
			else snprintf(labels[i], sizeof labels[i], "%d GB", disk_gb[i]);
			opts[i] = labels[i];
		}
		pm_ui_choice(u, _("硬碟大小"), opts, LEN(disk_gb), &nv_disk);
	}
	pm_ui_end(u);
	if (!nv_existing) {
		char fs[32], note[300];
		human_bytes(fs, sizeof fs, free_space());
		snprintf(note, sizeof note, _("硬碟檔一開始很小，虛擬機器用多少才佔多少空間。這台電腦目前還有 %s 可用。"), fs);
		pm_ui_note(u, note);
	} else pm_ui_note(u, _("現有的硬碟檔會直接拿來用（不會複製）。qcow2 格式才能拍快照。"));

	if (nv.arch == ARCH_ARM) goto buttons;
	pm_ui_group(u, _("進階"));
	int fw = nv.uefi ? 0 : 1;
	const char *fwopts[] = { "UEFI", _("傳統 BIOS") };
	if (pm_ui_segment(u, _("開機韌體"), fwopts, 2, &fw)) { nv.uefi = fw == 0; if (!nv.uefi) nv.secboot = 0; }
	if (nv.uefi) pm_ui_switch(u, _("安全開機"), &nv.secboot);
	pm_ui_switch(u, _("TPM 2.0"), &nv.tpm);
	if (nv.tpm && !have_swtpm() && pm_ui_button(u, _("TPM 要另外安裝 swtpm"), _("安裝"))) install_pkgs("swtpm");
	pm_ui_end(u);
	pm_ui_note(u, nv.os == OS_WINDOWS ? _("Windows 11 需要 UEFI、安全開機與 TPM 2.0（已經幫你打開）。Windows 沒有 VirtIO 驅動程式，所以硬碟用 SATA、網路卡用 Intel e1000e。")
	                                  : _("一般選 UEFI 就好；很舊的系統（例如 32 位元）才需要傳統 BIOS。"));

buttons:;
	const char *btns[] = { _("取消"), _("建立"), _("建立並開機") };
	int r = pm_ui_buttons(u, btns, 3, 2);
	if (r == 0) { go(PG_LIST); return; }
	if (r > 0) {
		char err[600];
		if (!nv.name[0]) snprintf(nv.name, sizeof nv.name, "%s", nv.iso[0] ? nv_autoname : _("新的虛擬機器"));
		if (!nv.name[0]) snprintf(nv.name, sizeof nv.name, "%s", _("新的虛擬機器"));
		if (nv_existing && !nv.disk[0]) { say(NULL, 1, "%s", _("請選擇虛擬硬碟")); }
		else if (!nv_existing && !have_qemu_img()) { say(NULL, 1, "%s", _("要先安裝 QEMU（沒有 qemu-img）")); }
		else if (vm_create(&nv, nv_existing ? 0 : disk_gb[nv_disk], err, sizeof err)) say(NULL, 1, "%s", err);
		else {
			vms_load();
			Vm *v = vm_find(nv.id);
			if (v) {
				open_vm(v);
				if (r == 2) start_vm(v, NULL);
			} else go(PG_LIST);
			return;
		}
	}
	u->y += 16;
	status_note(u, NULL);
}

/* 執行中：設定只能看不能改 */
static void info_settings(PmUI *u, Vm *v)
{
	char mem[24], cpus[16];
	fmt_mb(mem, sizeof mem, v->mem);
	snprintf(cpus, sizeof cpus, "%d", v->cpus);
	pm_ui_group(u, _("{群組}設定"));
	pm_ui_info(u, _("作業系統"), os_label(v->os));
	pm_ui_info(u, _("處理器架構"), v->arch == ARCH_ARM ? "ARM64" : "x86_64");
	pm_ui_info(u, _("記憶體"), mem);
	pm_ui_info(u, _("處理器核心"), cpus);
	pm_ui_info(u, _("顯示卡"), v->display == DISP_VIRTIO ? "VirtIO" : _("標準 VGA"));
	char netinfo[200];
	if (v->net == NET_BRIDGE) snprintf(netinfo, sizeof netinfo, _("橋接（%s）"), v->bridge);
	else if (v->net == NET_NAT) snprintf(netinfo, sizeof netinfo, "NAT%s%s", v->ports[0] ? " · " : "", v->ports);
	else snprintf(netinfo, sizeof netinfo, "%s", _("不接網路"));
	pm_ui_info(u, _("網路"), netinfo);
	pm_ui_info(u, _("開機韌體"), v->uefi ? (v->secboot ? _("UEFI（安全開機）") : "UEFI") : _("傳統 BIOS"));
	pm_ui_end(u);
	pm_ui_note(u, v->state == ST_SAVED ? _("有儲存的狀態時不能改設定（硬體不一樣就繼續不了）。要改設定，先「繼續」再關機，或捨棄儲存的狀態。")
	                                   : _("關機後才能修改設定。光碟可以在畫面視窗上面的「光碟」換。"));
}

/* 網路：NAT（預設，不用設定）、橋接（跟這台電腦在同一個區域網路，別台電腦也連得到）、不接網路 */
static int safe_ifname(const char *n)
{
	for (const char *p = n; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') return 0;
	return *n != 0;
}

static void build_network(PmUI *u, Vm *v)
{
	pm_ui_group(u, _("網路"));
	const char *nopts[] = { _("不接網路"), _("NAT（跟這台電腦共用網路）"), _("橋接（跟這台電腦在同一個區域網路）") };
	pm_ui_choice(u, _("連線方式"), nopts, 3, &v->net);
	const char *note = NULL;
	if (v->net == NET_NAT) {
		pm_ui_entry(u, _("轉送連接埠"), v->ports, sizeof v->ports, 0);
		note = _("轉送連接埠寫成「本機:虛擬機器」，例如 2222:22 之後可以用 ssh -p 2222 localhost 連進虛擬機器（只有這台電腦連得到）。");
	} else if (v->net == NET_BRIDGE) {
		char brs[16][32], eth[16][32];
		int nb = list_ifaces(0, brs, 16);
		if (nb) {
			int cur = 0;
			const char *opts[16];
			for (int i = 0; i < nb; i++) { opts[i] = brs[i]; if (!strcmp(brs[i], v->bridge)) cur = i; }
			if (strcmp(v->bridge, brs[cur])) snprintf(v->bridge, sizeof v->bridge, "%s", brs[cur]);
			if (pm_ui_choice(u, _("橋接器"), opts, nb, &cur)) snprintf(v->bridge, sizeof v->bridge, "%s", brs[cur]);
			const char *helper = bridge_helper();
			int helper_ok = bridge_helper_ok(), allowed = bridge_allowed(v->bridge);
			if (!helper) pm_ui_info(u, _("找不到 qemu-bridge-helper"), NULL);
			else if ((!helper_ok || allowed == 0) && safe_ifname(v->bridge)) {
				char msg[200];
				snprintf(msg, sizeof msg, _("要先允許虛擬機器使用 %s"), v->bridge);
				if (pm_ui_button(u, msg, _("允許使用"))) {
					char cmd[600];
					snprintf(cmd, sizeof cmd, "sudo sh -c 'mkdir -p /etc/qemu && (grep -qx \"allow %s\" /etc/qemu/bridge.conf 2>/dev/null || echo \"allow %s\" >> /etc/qemu/bridge.conf)%s%s'",
					         v->bridge, v->bridge, helper_ok ? "" : " && chmod u+s ", helper_ok ? "" : helper);
					run_in_terminal(_("允許使用橋接器"), cmd);
				}
			}
			note = _("橋接時虛擬機器像網路上的另一台電腦，從路由器拿 IP，區域網路裡的其他電腦也連得到它。"
			         "「允許使用」會把橋接器寫進 /etc/qemu/bridge.conf，並讓 qemu-bridge-helper 能以管理員權限建立網路介面。");
		} else if ((v->bridge[0] = 0, list_ifaces(1, eth, 16)) && safe_ifname(eth[0])) {
			char msg[200];
			snprintf(msg, sizeof msg, _("這台電腦還沒有橋接器（可以用有線網路卡 %s 建一個）"), eth[0]);
			if (pm_ui_button(u, msg, _("建立橋接器"))) {
				char cmd[1600];
				const char *helper = bridge_helper();
				snprintf(cmd, sizeof cmd,
				         "echo '%s'; read x; dev=%s; old=$(nmcli -g GENERAL.CONNECTION device show $dev 2>/dev/null); "
				         "sudo nmcli connection add type bridge ifname br0 con-name br0 bridge.stp no && "
				         "sudo nmcli connection add type bridge-slave ifname $dev master br0 con-name br0-$dev && "
				         "{ [ -z \"$old\" ] || sudo nmcli connection modify \"$old\" connection.autoconnect no; } && "
				         "sudo nmcli connection up br0-$dev && sudo nmcli connection up br0 && "
				         "sudo sh -c 'mkdir -p /etc/qemu && (grep -qx \"allow br0\" /etc/qemu/bridge.conf 2>/dev/null || echo \"allow br0\" >> /etc/qemu/bridge.conf)%s%s'",
				         sh_safe(_("會建立橋接器 br0，把有線網路卡接上去，這台電腦的網路會中斷幾秒、IP 可能會變。按 Enter 繼續，Ctrl+C 取消。")),
				         eth[0], helper && !bridge_helper_ok() ? " && chmod u+s " : "", helper && !bridge_helper_ok() ? helper : "");
				run_in_terminal(_("建立橋接器"), cmd);
				snprintf(v->bridge, sizeof v->bridge, "br0");
			}
			note = _("建立橋接器要用 NetworkManager，只能用有線網路卡（Wi-Fi 沒辦法橋接）。如果是遠端連進這台電腦，建立時連線會中斷一下。");
		} else {
			pm_ui_info(u, _("沒有可以橋接的網路卡"), NULL);
			note = _("Wi-Fi 網路卡沒辦法橋接，請用 NAT。");
		}
	}
	pm_ui_end(u);
	if (note) pm_ui_note(u, note);
}

static void edit_settings(PmUI *u, Vm *v)
{
	pm_ui_group(u, _("一般"));
	pm_ui_entry(u, _("名稱"), v->name, sizeof v->name, 0);
	if (ui_os(u, &v->os)) { /* 只改標籤與時區，硬體不動（已經裝好的系統換驅動會開不了機） */ }
	pm_ui_info(u, _("處理器架構"), v->arch == ARCH_ARM ? (host_arch() == ARCH_ARM ? "ARM64" : _("ARM64（軟體模擬）")) : "x86_64");
	ui_mem(u, &v->mem);
	ui_cpus(u, &v->cpus);
	pm_ui_end(u);

	pm_ui_group(u, _("光碟機"));
	if (pm_ui_item(u, NULL, v->iso[0] ? base_name(v->iso) : _("沒有光碟"), v->iso[0] ? v->iso : NULL, _("選擇… ›"), 0)) {
		char path[PATH_MAX];
		if (pick_file(_("選擇光碟映像"), 1, v->iso, path, sizeof path)) snprintf(v->iso, sizeof v->iso, "%s", path);
		u->again = 1;
	}
	if (v->iso[0] && pm_ui_button(u, NULL, _("退出光碟"))) v->iso[0] = 0;
	int boot = v->boot_cd;
	const char *bopts[] = { _("硬碟優先"), _("光碟優先") };
	if (pm_ui_segment(u, _("開機順序"), bopts, 2, &boot)) v->boot_cd = boot;
	pm_ui_end(u);
	pm_ui_note(u, _("硬碟優先時，硬碟還沒裝系統會自動從光碟開機，裝好之後就從硬碟開，不用再改。"));

	pm_ui_group(u, _("硬碟"));
	char disk[PATH_MAX];
	vm_disk(v, disk, sizeof disk);
	if (disk_ii_ok) {
		char vs[32], as[32], line[100];
		human_bytes(vs, sizeof vs, disk_ii.vsize);
		human_bytes(as, sizeof as, disk_ii.asize);
		snprintf(line, sizeof line, _("%s（實際佔用 %s）"), vs, as);
		pm_ui_info(u, _("大小"), line);
		pm_ui_info(u, _("格式"), disk_ii.fmt);
	} else pm_ui_info(u, _("大小"), _("讀不到硬碟檔"));
	pm_ui_info(u, _("位置"), disk);
	if (disk_ii_ok) {
		const char *ropts[] = { _("不變"), "+8 GB", "+16 GB", "+32 GB", "+64 GB", "+128 GB" };
		static const char *radd[] = { NULL, "+8G", "+16G", "+32G", "+64G", "+128G" };
		pm_ui_choice(u, _("加大硬碟"), ropts, LEN(ropts), &resize_choice);
		if (resize_choice && pm_ui_button(u, NULL, _("加大"))) {
			run_job(v, JOB_RESIZE, radd[resize_choice], NULL, 0, fn_resize);
			resize_choice = 0;
		}
	}
	pm_ui_end(u);

	pm_ui_group(u, _("顯示與聲音"));
	const char *dopts[] = { _("標準 VGA（相容性最好）"), _("VirtIO（解析度可以跟著視窗）") };
	if (v->arch == ARCH_ARM) pm_ui_info(u, _("顯示卡"), "VirtIO");
	else pm_ui_choice(u, _("顯示卡"), dopts, 2, &v->display);
	pm_ui_switch(u, _("聲音"), &v->sound);
	int direct = qemu_has_audio("pa") || qemu_has_audio("pipewire");
	if (v->sound && !direct && pm_pkg_kind() == PM_PKG_APT && pm_ui_button(u, _("麥克風要另外安裝 QEMU 的聲音模組"), _("安裝")))
		install_pkgs("qemu-system-gui");
	pm_ui_end(u);
	if (v->sound) pm_ui_note(u, direct ? _("虛擬機器的聲音從這台電腦的喇叭播出，麥克風也可以用。")
	                                   : _("虛擬機器的聲音透過畫面視窗播出（畫面視窗開著才聽得到），沒有麥克風。要用麥克風請安裝 QEMU 的聲音模組（qemu-system-gui，會一起裝 GTK3，約 30 MB）。"));

	build_network(u, v);

	if (v->arch == ARCH_ARM) return;
	pm_ui_group(u, _("韌體與安全"));
	pm_ui_info(u, _("開機韌體"), v->uefi ? "UEFI" : _("傳統 BIOS"));
	if (v->uefi && pm_ui_switch(u, _("安全開機"), &v->secboot)) run_job(v, JOB_FW, NULL, NULL, 0, fn_fw);
	pm_ui_switch(u, _("TPM 2.0"), &v->tpm);
	if (v->tpm && !have_swtpm() && pm_ui_button(u, _("TPM 要另外安裝 swtpm"), _("安裝"))) install_pkgs("swtpm");
	pm_ui_end(u);
	if (v->uefi) pm_ui_note(u, _("切換安全開機會換一套 UEFI 韌體，UEFI 裡的開機選項會回到預設。"));
}

static void build_snaps(PmUI *u, Vm *v, int running)
{
	Snap s[MAX_SNAPS];
	int n = snaps_load(v, s, MAX_SNAPS);
	Job *busy_job = job_for(v);
	pm_ui_group(u, _("快照"));
	for (int i = n - 1; i >= 0; i--) {
		char sub[160], when[64];
		time_t t = (time_t)s[i].t;
		strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&t));
		snprintf(sub, sizeof sub, "%s · %s", when, s[i].ram ? _("含記憶體（回去時程式都還開著）") : _("只有硬碟"));
		if (pm_ui_item(u, NULL, s[i].name, sub, snap_sel == i ? NULL : "›", snap_sel == i)) {
			snap_sel = snap_sel == i ? -1 : i;
			snprintf(snap_name, sizeof snap_name, "%s", s[i].name);
		}
		if (snap_sel != i) continue;
		if (pm_ui_entry(u, _("名稱"), snap_name, sizeof snap_name, 0) || strcmp(snap_name, s[i].name)) {
			if (snap_name[0]) { snprintf(s[i].name, sizeof s[i].name, "%s", snap_name); snaps_save(v, s, n); }
		}
		if (!busy_job) {
			if (running && !s[i].ram) pm_ui_info(u, _("回到這個快照"), _("要先關機"));
			else if (v->state == ST_SAVED) pm_ui_info(u, _("回到這個快照"), _("要先捨棄儲存的狀態"));
			else if (pm_ui_button(u, running ? _("回到這個快照（現在的狀態會不見）") : s[i].ram ? _("回到這個快照並開機") : _("回到這個快照（硬碟會變回當時的樣子）"), _("回到快照"))) {
				if (!running && s[i].ram) start_vm(v, s[i].tag);
				else run_job(v, JOB_SNAP_RESTORE, s[i].tag, s[i].name, 0, fn_snap_restore);
				snap_sel = -1;
			}
			if (pm_ui_button(u, _("刪除這個快照"), _("刪除"))) { run_job(v, JOB_SNAP_DELETE, s[i].tag, s[i].name, 0, fn_snap_delete); snap_sel = -1; }
		}
	}
	if (busy_job && busy_job->kind != JOB_START) pm_ui_info(u, _("請稍候…"), busy_job->kind == JOB_SNAP_TAKE ? _("正在拍快照") : _("處理中"));
	else if (pm_ui_button(u, running ? _("把現在的樣子（含記憶體）存成一個快照") : _("把現在的硬碟存成一個快照"), _("拍快照"))) {
		time_t now = time(NULL);
		char tag[64], name[128], when[64];
		snprintf(tag, sizeof tag, "s%lld", (long long)now);
		strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&now));
		snprintf(name, sizeof name, "%s %s", _("快照"), when);
		run_job(v, JOB_SNAP_TAKE, tag, name, running, fn_snap_take);
	}
	pm_ui_end(u);
	pm_ui_note(u, _("快照存在硬碟檔裡，隨時可以回到拍的時候。執行中拍的連記憶體一起存，要拍久一點。"));
}

/* 執行中：使用量的即時圖表（最近一分鐘） */
static void build_usage(PmUI *u, Vm *v)
{
	Stats *st = stats_for(v->id);
	if (!st || !st->n) return;
	char label[200], a[24], b[24], c[24], mem[24], used[24];
	static const unsigned blue = 0x0a84ff, orange = 0xff9f0a, green = 0x30d158, purple = 0xbf5af2;
	pm_ui_group(u, _("使用量"));
	snprintf(label, sizeof label, "CPU　%.0f%%", st->cpu[HIST - 1]);
	const double *cpu[] = { st->cpu };
	pm_ui_graph(u, label, NULL, cpu, &blue, 1, HIST, 100);
	human_bytes(used, sizeof used, (unsigned long long)st->rss);
	fmt_mb(mem, sizeof mem, v->mem);
	char val[80];
	snprintf(val, sizeof val, _("%s／配置 %s"), used, mem);
	pm_ui_meter(u, _("記憶體"), val, (double)st->rss / ((double)v->mem * 1048576.0), green);
	fmt_rate(a, sizeof a, st->rdr[HIST - 1]);
	fmt_rate(b, sizeof b, st->wrr[HIST - 1]);
	fmt_rate(c, sizeof c, MAX(peak(st->rdr, NULL), peak(st->wrr, NULL)));
	snprintf(label, sizeof label, _("磁碟　讀 %s　寫 %s　（最高 %s）"), a, b, c);
	const char *dleg[] = { _("讀取"), _("寫入") };
	const double *disk[] = { st->rdr, st->wrr };
	unsigned dcol[] = { blue, orange };
	pm_ui_graph(u, label, dleg, disk, dcol, 2, HIST, 0);
	if (v->net != NET_NONE) {
		fmt_rate(a, sizeof a, st->rxr[HIST - 1]);
		fmt_rate(b, sizeof b, st->txr[HIST - 1]);
		fmt_rate(c, sizeof c, MAX(peak(st->rxr, NULL), peak(st->txr, NULL)));
		snprintf(label, sizeof label, _("網路　下載 %s　上傳 %s　（最高 %s）"), a, b, c);
		const char *nleg[] = { _("下載"), _("上傳") };
		const double *net[] = { st->rxr, st->txr };
		unsigned ncol[] = { green, purple };
		pm_ui_graph(u, label, nleg, net, ncol, 2, HIST, 0);
	}
	pm_ui_end(u);
	pm_ui_note(u, v->net == NET_NAT ? _("記憶體是 QEMU 實際佔用的（虛擬機器用過的才算）。NAT 的網路只算 TCP，是概略的數字。")
	                                : _("記憶體是 QEMU 實際佔用的（虛擬機器用過的才算）。"));
}

/* 快照要 qcow2：每次重畫都問 qemu-img 太慢，看檔名或剛剛讀到的格式 */
static int vm_can_snapshot_cached(const Vm *v)
{
	char path[PATH_MAX];
	vm_disk(v, path, sizeof path);
	size_t l = strlen(path);
	if (l > 6 && !strcmp(path + l - 6, ".qcow2")) return 1;
	return disk_ii_ok && !strcmp(disk_ii.fmt, "qcow2");
}

static void build_vm(PmUI *u)
{
	Vm *v = vm_find(cur_id);
	if (!v) { go(PG_LIST); return; }
	if (pm_ui_back(u, _("虛擬機器"))) { go(PG_LIST); return; }
	char sub[200];
	snprintf(sub, sizeof sub, "%s · %s", state_label(v->state), os_label(v->os));
	pm_ui_title(u, v->name, sub);
	int running = v->state == ST_RUNNING || v->state == ST_PAUSED;
	int can_start = env_notes(u);
	if (u->clicked && u->dbl && list_click_ms && pm_now_ms() - list_click_ms < 600) {   /* 清單上點兩下：開機或顯示畫面 */
		u->claimed = 1;
		list_click_ms = 0;
		if (running) open_console(v);
		else if (can_start && v->state != ST_STARTING) start_vm(v, NULL);
	}

	preview_load(v);
	if (preview) {
		int w = MIN(u->cw - 32, 480), h = w * cairo_image_surface_get_height(preview) / MAX(1, cairo_image_surface_get_width(preview));
		pm_ui_image(u, preview, w, MIN(h, 360));
		pm_ui_end(u);
	}

	pm_ui_group(u, NULL);
	if (v->state == ST_STARTING) pm_ui_info(u, _("正在開機…"), NULL);
	else if (running) {
		if (pm_ui_button(u, _("顯示虛擬機器的畫面"), _("顯示畫面"))) open_console(v);
		if (pm_ui_button(u, v->state == ST_PAUSED ? _("已暫停") : _("暫停（記憶體留著，隨時可以繼續）"), v->state == ST_PAUSED ? _("{執行}繼續") : _("暫停"))) {
			qmp_simple(v, v->state == ST_PAUSED ? "cont" : "stop");
			vm_refresh(v);
		}
		if (pm_ui_button(u, _("請虛擬機器關機（像按一下電源鍵）"), _("關機"))) {
			if (qmp_simple(v, "system_powerdown")) say(v, 1, "%s", _("跟虛擬機器的連線中斷了"));
			else say(v, 0, "%s", _("已經送出關機訊號。沒反應的話可以用「強制關機」。"));
		}
		if (!confirm_kill) { if (pm_ui_button(u, _("強制關機（像拔掉電源，沒存的資料會不見）"), _("強制關機"))) confirm_kill = 1; }
		else if (pm_ui_button(u, _("確定要強制關機嗎？"), _("強制關機"))) { qmp_simple(v, "quit"); confirm_kill = 0; usleep(200000); vm_refresh(v); }
	} else if (can_start) {
		if (v->state == ST_SAVED) {
			if (pm_ui_button(u, _("從上次儲存的狀態繼續"), _("{執行}繼續"))) start_vm(v, NULL);
			if (!confirm_discard) { if (pm_ui_button(u, _("捨棄儲存的狀態，下次重新開機"), _("捨棄"))) confirm_discard = 1; }
			else if (pm_ui_button(u, _("確定嗎？沒存的資料會不見"), _("捨棄"))) {
				v->resume[0] = 0;
				vm_save(v);
				saved = *v;
				run_job(v, JOB_SNAP_DELETE, RESUME_TAG, NULL, 0, fn_snap_delete);
				confirm_discard = 0;
				vm_refresh(v);
			}
		} else if (pm_ui_button(u, v->iso[0] ? _("開機（有放安裝光碟）") : _("開機"), _("開機"))) start_vm(v, NULL);
	}
	pm_ui_end(u);
	status_note(u, v);
	if (running) build_usage(u, v);

	if (running || v->state == ST_STARTING || v->state == ST_SAVED) info_settings(u, v);
	else {
		edit_settings(u, v);
		/* 有改就存（輸入框每打一個字也存，跟系統設定一樣不用按「儲存」） */
		saved.state = v->state;
		if (memcmp(&saved, v, sizeof saved)) { vm_save(v); saved = *v; }
	}

	if (vm_can_snapshot_cached(v)) build_snaps(u, v, running);

	pm_ui_group(u, _("其他"));
	if (pm_ui_button(u, _("在檔案管理員打開這台虛擬機器的資料夾"), _("打開"))) {
		char cmd[PATH_MAX + 32];
		snprintf(cmd, sizeof cmd, "exec pomelo-open '%s'", v->dir);
		pm_spawn(cmd);
	}
	if (!running && v->state != ST_STARTING) {
		if (!confirm_delete) { if (pm_ui_button(u, _("刪除這台虛擬機器（硬碟、快照都會刪掉）"), _("刪除"))) confirm_delete = 1; }
		else if (pm_ui_button(u, _("確定要刪除嗎？刪掉就救不回來了"), _("確定刪除"))) {
			if (vm_delete(v)) say(NULL, 1, "%s", _("無法刪除"));
			vms_load();
			go(PG_LIST);
		}
	}
	pm_ui_end(u);
}

static void build(PmUI *u)
{
	ui = u;
	if (page == PG_NEW) build_new(u);
	else if (page == PG_VM) build_vm(u);
	else build_list(u);
}

static void tick(PmUI *u)
{
	int changed = poll_jobs();
	long now = pm_now_ms();
	static long next_state;
	if (now >= next_state) {
		next_state = now + 1000;
		if (now - last_reload > 5000 && page == PG_LIST) {   /* 指令列建立或刪除的也看得到 */
			Vm keep[MAX_VMS];
			int nk = nvms;
			memcpy(keep, vms, sizeof keep);
			vms_load();
			for (int i = 0; i < nvms; i++)
				for (int k = 0; k < nk; k++)
					if (!strcmp(vms[i].id, keep[k].id) && keep[k].state == ST_STARTING) vms[i].state = ST_STARTING;
			last_reload = now;
			changed = 1;
		}
		for (int i = 0; i < nvms; i++) {
			Vm *v = &vms[i];
			int old = v->state;
			if (v->state == ST_STARTING && !job_for(v)) v->state = ST_OFF;
			vm_refresh(v);
			stats_sample(v);
			if (v->state == ST_RUNNING || v->state == ST_PAUSED) changed = 1;   /* 使用量每秒更新 */
			if (v->state == old) continue;
			changed = 1;
			/* 執行中畫面視窗可能改了設定（換光碟、儲存狀態）：重讀一次，免得等一下把舊的寫回去 */
			if (old != ST_STARTING) {
				Vm fresh;
				if (!vm_load(&fresh, v->dir, v->id)) {
					fresh.state = v->state;
					*v = fresh;
					if (!v->resume[0] && v->state == ST_SAVED) v->state = ST_OFF;
					if (v->resume[0] && v->state == ST_OFF) v->state = ST_SAVED;
				}
			}
			if (page == PG_VM && !strcmp(v->id, cur_id)) {
				saved = *v;
				char disk[PATH_MAX];
				vm_disk(v, disk, sizeof disk);
				if (old == ST_RUNNING || old == ST_PAUSED) disk_ii_ok = !img_info(disk, &disk_ii);   /* 關機後大小變了 */
			}
		}
		if (!qemu_bin() && qemu_recheck()) changed = 1;   /* 剛在終端機裝好 */
		if (page == PG_VM) {
			Vm *v = vm_find(cur_id);
			if (v) {
				struct stat st;
				char p[PATH_MAX];
				vm_file(v, "screen.png", p, sizeof p);
				if (!stat(p, &st) && st.st_mtime != preview_mtime) changed = 1;
			}
		}
	}
	if (changed) pm_ui_redraw(u);
}

/* ------------------------------------------------------------------ */
/* 指令列                                                               */
/* ------------------------------------------------------------------ */

static void usage(void)
{
	printf("%s:\n  pomelo-vm\n  pomelo-vm --show %s\n  pomelo-vm --start %s\n  pomelo-vm --console %s\n  pomelo-vm --stop %s\n  pomelo-vm --kill %s\n  pomelo-vm --list\n",
	       _("用法"), _("名稱"), _("名稱"), _("名稱"), _("名稱"), _("名稱"));
}

static Vm *cli_vm(const char *key)
{
	vms_load();
	Vm *v = vm_find(key);
	if (!v) fprintf(stderr, "pomelo-vm: %s: %s\n", key, _("沒有這台虛擬機器"));
	return v;
}

static int cli_start(Vm *v)
{
	char err[600];
	vm_refresh(v);
	if (v->state != ST_RUNNING && v->state != ST_PAUSED) {
		const char *resume = v->resume[0] ? v->resume : NULL;
		int fd = vm_start(v, resume, err, sizeof err);
		if (fd < 0) { fprintf(stderr, "pomelo-vm: %s\n", err); return 1; }
		fcntl(fd, F_SETFL, 0);
		char out[4096];
		size_t n = 0;
		ssize_t r;
		while ((r = read(fd, out + n, sizeof out - 1 - n)) > 0) n += (size_t)r;
		out[n] = 0;
		close(fd);
		if (!vm_pid(v)) {
			explain_qemu_error(out, sizeof out);
			fprintf(stderr, "pomelo-vm: %s：%s\n", _("無法開機"), out);
			return 1;
		}
		if (resume) {
			v->resume[0] = 0;
			vm_save(v);
			char e2[256];
			hmp_ok(v, "delvm " RESUME_TAG, 60000, e2, sizeof e2);
		}
	}
	if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) return 0;   /* 沒有圖形環境就不開畫面視窗（原生 Wayland 不需要 DISPLAY） */
	return console_main(v);
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { usage(); return 0; }
	setlocale(LC_ALL, "");
	struct sigaction sa = { .sa_handler = SIG_IGN, .sa_flags = SA_NOCLDWAIT };
	sigaction(SIGCHLD, &sa, NULL);   /* 背景啟動 QEMU 的子行程自動回收 */
	if (argc > 1 && !strcmp(argv[1], "--list")) {
		vms_load();
		for (int i = 0; i < nvms; i++) {
			vm_refresh(&vms[i]);
			printf("%s\t%s\t%s\n", vms[i].id, vms[i].name, state_label(vms[i].state));
		}
		return 0;
	}
	if (argc > 2 && (!strcmp(argv[1], "--stop") || !strcmp(argv[1], "--kill"))) {
		Vm *v = cli_vm(argv[2]);
		if (!v) return 1;
		if (!vm_pid(v)) return 0;
		return qmp_simple(v, argv[1][2] == 's' ? "system_powerdown" : "quit") ? 1 : 0;
	}
	if (argc > 2 && !strcmp(argv[1], "--start")) {
		Vm *v = cli_vm(argv[2]);
		return v ? cli_start(v) : 1;
	}
	if (argc > 2 && !strcmp(argv[1], "--console")) {
		Vm *v = cli_vm(argv[2]);
		return v ? console_main(v) : 1;
	}
	vms_load();
	if (argc > 2 && !strcmp(argv[1], "--show")) {
		Vm *v = vm_find(argv[2]);
		if (v) { vm_refresh(v); open_vm(v); }
	} else if (argc > 1) { usage(); return 1; }
	for (int i = 0; i < nvms; i++) vm_refresh(&vms[i]);
	last_reload = pm_now_ms();
	pm_ui_add_icon("@vm-linux", vm_icon(0xff9f0a, 0xe8590c));
	pm_ui_add_icon("@vm-windows", vm_icon(0x3d9bff, 0x0a5fd6));
	pm_ui_add_icon("@vm-other", vm_icon(0x8e8e93, 0x5a5a60));
	pm_ui_native = 1;   /* 管理視窗沒有直接呼叫 X（畫面視窗是另一個 --console 程序，照舊走 X11）：Wayland 工作階段直接當原生程式 */
	return pm_ui_run(_("虛擬機器"), "pomelo-vm", 680, 720, build, tick, 500, icon);
}
