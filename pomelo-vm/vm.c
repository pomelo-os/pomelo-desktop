/* SPDX-License-Identifier: GPL-3.0-or-later */
/* vm.c — 虛擬機器的設定檔、檔案位置、QEMU 指令、開機與控制（被 pomelo-vm.c #include）
 *
 * 每台虛擬機器一個資料夾：$XDG_DATA_HOME/pomelo/vm/<id>/
 *   vm.conf        設定（「鍵 = 值」）
 *   disk.qcow2     硬碟（也可以指到別的地方的映像檔，設定裡存絕對路徑）
 *   vars.qcow2     UEFI 的變數（開機選項、安全開機的金鑰）；用 qcow2 才能連記憶體一起拍快照
 *   tpm/           TPM 2.0 的狀態（swtpm）
 *   snapshots.tsv  快照的名稱與時間（快照本身在 qcow2 裡）
 *   screen.png     最後的畫面（清單、詳細資料頁的縮圖）
 * 執行時的 socket 在 $XDG_RUNTIME_DIR/pomelo-vm/<id>/（UNIX socket 路徑不能超過 107 位元組，所以用短的 id）：
 *   qmp.sock  畫面視窗用的 QMP（一直連著，收關機等事件）
 *   ctl.sock  管理視窗與指令列用的 QMP（每次連上、送指令、斷開）
 *   vnc.sock  畫面（VNC），audio 檔寫著聲音是 QEMU 自己播（direct）還是透過畫面視窗播（vnc）
 */

#define MAX_VMS 64
#define RESUME_TAG "pomelo-resume"

enum { OS_LINUX, OS_WINDOWS, OS_OTHER };
enum { ST_OFF, ST_RUNNING, ST_PAUSED, ST_STARTING, ST_SAVED };
enum { DISP_STD, DISP_VIRTIO };
enum { BUS_VIRTIO, BUS_SATA };
enum { NIC_VIRTIO, NIC_E1000E };
enum { NET_NONE, NET_NAT, NET_BRIDGE };
enum { ARCH_X86, ARCH_ARM };
enum { KVM_OK, KVM_NODEV, KVM_PERM };

typedef struct {
	char id[32], dir[PATH_MAX];
	char name[128];
	int arch;                        /* ARCH_X86（x86_64）或 ARCH_ARM（ARM64） */
	int os, mem, cpus;               /* mem：MiB */
	char disk[PATH_MAX], iso[PATH_MAX];
	int uefi, secboot, tpm;
	char fw_code[PATH_MAX], fw_fmt[16];
	int fw_smm;
	int display, bus, nic, net, sound, boot_cd;   /* net：NET_* */
	char ports[128];                 /* NAT 的轉送連接埠：「2222:22 8080:80」（本機:虛擬機器） */
	char bridge[32];                 /* 橋接：接到這台電腦的哪個橋接器（例如 br0） */
	char resume[64];                 /* 儲存了狀態：下次開機從這個快照繼續 */
	long long created;
	int state;
} Vm;

static Vm vms[MAX_VMS];
static int nvms;

/* ------------------------------------------------------------------ */
/* 檔案位置                                                             */
/* ------------------------------------------------------------------ */

static void data_root(char *out, size_t n)
{
	const char *d = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	if (d && *d) snprintf(out, n, "%s/pomelo/vm", d);
	else snprintf(out, n, "%s/.local/share/pomelo/vm", home ? home : "/tmp");
}

static void run_root(char *out, size_t n)
{
	const char *r = getenv("XDG_RUNTIME_DIR");
	if (r && *r && !access(r, W_OK)) snprintf(out, n, "%s/pomelo-vm", r);
	else snprintf(out, n, "/tmp/pomelo-vm-%u", (unsigned)getuid());
}

static void vm_file(const Vm *v, const char *f, char *out, size_t n) { snprintf(out, n, "%s/%s", v->dir, f); }

static void vm_run(const Vm *v, const char *f, char *out, size_t n)
{
	char r[PATH_MAX];
	run_root(r, sizeof r);
	snprintf(out, n, "%s/%s/%s", r, v->id, f);
}

static void vm_disk(const Vm *v, char *out, size_t n)
{
	if (v->disk[0] == '/') snprintf(out, n, "%s", v->disk);
	else vm_file(v, v->disk[0] ? v->disk : "disk.qcow2", out, n);
}

static int mkdirs(const char *path, mode_t mode)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s", path);
	for (char *s = p + 1; *s; s++)
		if (*s == '/') { *s = 0; mkdir(p, mode); *s = '/'; }
	return mkdir(p, mode) == 0 || errno == EEXIST ? 0 : -1;
}

static const char *base_name(const char *path)
{
	const char *s = strrchr(path, '/');
	return s ? s + 1 : path;
}

/* ------------------------------------------------------------------ */
/* 設定檔                                                               */
/* ------------------------------------------------------------------ */

static void vm_defaults(Vm *v, int os)
{
	long mb = sysconf(_SC_PHYS_PAGES) / 1024 * sysconf(_SC_PAGESIZE) / 1024;
	int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	v->os = os;
	v->mem = os == OS_WINDOWS ? (mb >= 16000 ? 8192 : 4096) : (mb >= 8000 ? 4096 : 2048);
	v->cpus = ncpu >= 8 ? 4 : ncpu >= 4 ? 2 : 1;
	v->uefi = 1;
	v->secboot = v->tpm = os == OS_WINDOWS;
	v->display = os == OS_LINUX ? DISP_VIRTIO : DISP_STD;
	v->bus = os == OS_WINDOWS ? BUS_SATA : BUS_VIRTIO;
	v->nic = os == OS_WINDOWS ? NIC_E1000E : NIC_VIRTIO;
	v->net = NET_NAT;
	v->sound = 1;
}

/* ARM64：virt 機型沒有 VGA、IDE、PS/2，一律 VirtIO；韌體只有 UEFI，不做安全開機與 TPM */
static void vm_set_arch(Vm *v, int arch)
{
	v->arch = arch;
	if (arch != ARCH_ARM) return;
	v->uefi = 1;
	v->secboot = v->tpm = 0;
	v->display = DISP_VIRTIO;
	v->bus = BUS_VIRTIO;
	v->nic = NIC_VIRTIO;
}

static int vm_load(Vm *v, const char *dir, const char *id)
{
	char path[PATH_MAX];
	snprintf(path, sizeof path, "%s/vm.conf", dir);
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	memset(v, 0, sizeof *v);
	vm_defaults(v, OS_OTHER);
	snprintf(v->dir, sizeof v->dir, "%s", dir);
	snprintf(v->id, sizeof v->id, "%s", id);
	char line[PATH_MAX + 64];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *eq = strstr(line, " = ");
		if (line[0] == '#' || !eq) continue;
		*eq = 0;
		const char *k = line, *val = eq + 3;
		int i = atoi(val);
		if (!strcmp(k, "name")) snprintf(v->name, sizeof v->name, "%s", val);
		else if (!strcmp(k, "arch")) v->arch = !strcmp(val, "aarch64") ? ARCH_ARM : ARCH_X86;
		else if (!strcmp(k, "os")) v->os = !strcmp(val, "linux") ? OS_LINUX : !strcmp(val, "windows") ? OS_WINDOWS : OS_OTHER;
		else if (!strcmp(k, "memory")) v->mem = CLAMP(i, 128, 1 << 20);
		else if (!strcmp(k, "cpus")) v->cpus = CLAMP(i, 1, 256);
		else if (!strcmp(k, "disk")) snprintf(v->disk, sizeof v->disk, "%s", val);
		else if (!strcmp(k, "cdrom")) snprintf(v->iso, sizeof v->iso, "%s", val);
		else if (!strcmp(k, "firmware")) v->uefi = !strcmp(val, "uefi");
		else if (!strcmp(k, "secure_boot")) v->secboot = i;
		else if (!strcmp(k, "tpm")) v->tpm = i;
		else if (!strcmp(k, "fw_code")) snprintf(v->fw_code, sizeof v->fw_code, "%s", val);
		else if (!strcmp(k, "fw_format")) snprintf(v->fw_fmt, sizeof v->fw_fmt, "%s", val);
		else if (!strcmp(k, "fw_smm")) v->fw_smm = i;
		else if (!strcmp(k, "display")) v->display = !strcmp(val, "virtio") ? DISP_VIRTIO : DISP_STD;
		else if (!strcmp(k, "disk_bus")) v->bus = !strcmp(val, "sata") ? BUS_SATA : BUS_VIRTIO;
		else if (!strcmp(k, "nic")) v->nic = !strcmp(val, "e1000e") ? NIC_E1000E : NIC_VIRTIO;
		else if (!strcmp(k, "network")) v->net = !strcmp(val, "none") ? NET_NONE : !strcmp(val, "bridge") ? NET_BRIDGE : NET_NAT;
		else if (!strcmp(k, "bridge")) snprintf(v->bridge, sizeof v->bridge, "%s", val);
		else if (!strcmp(k, "ports")) snprintf(v->ports, sizeof v->ports, "%s", val);
		else if (!strcmp(k, "sound")) v->sound = i;
		else if (!strcmp(k, "boot")) v->boot_cd = !strcmp(val, "cdrom");
		else if (!strcmp(k, "resume")) snprintf(v->resume, sizeof v->resume, "%s", val);
		else if (!strcmp(k, "created")) v->created = atoll(val);
	}
	fclose(f);
	if (!v->name[0]) snprintf(v->name, sizeof v->name, "%s", id);
	return 0;
}

static int vm_save(const Vm *v)
{
	char path[PATH_MAX], tmp[PATH_MAX + 8];
	vm_file(v, "vm.conf", path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return -1;
	char name[128], ports[128];
	snprintf(name, sizeof name, "%s", v->name);
	snprintf(ports, sizeof ports, "%s", v->ports);
	for (char *p = name; *p; p++) if (*p == '\n') *p = ' ';
	for (char *p = ports; *p; p++) if (*p == '\n') *p = ' ';
	fprintf(f, "# Pomelo 虛擬機器的設定\n");
	fprintf(f, "name = %s\n", name);
	fprintf(f, "arch = %s\n", v->arch == ARCH_ARM ? "aarch64" : "x86_64");
	fprintf(f, "os = %s\n", v->os == OS_LINUX ? "linux" : v->os == OS_WINDOWS ? "windows" : "other");
	fprintf(f, "memory = %d\ncpus = %d\n", v->mem, v->cpus);
	fprintf(f, "disk = %s\ncdrom = %s\n", v->disk, v->iso);
	fprintf(f, "firmware = %s\nsecure_boot = %d\ntpm = %d\n", v->uefi ? "uefi" : "bios", v->secboot, v->tpm);
	if (v->fw_code[0]) fprintf(f, "fw_code = %s\nfw_format = %s\nfw_smm = %d\n", v->fw_code, v->fw_fmt[0] ? v->fw_fmt : "raw", v->fw_smm);
	fprintf(f, "display = %s\ndisk_bus = %s\nnic = %s\n", v->display == DISP_VIRTIO ? "virtio" : "std",
	        v->bus == BUS_SATA ? "sata" : "virtio", v->nic == NIC_E1000E ? "e1000e" : "virtio");
	fprintf(f, "network = %s\nports = %s\nbridge = %s\nsound = %d\nboot = %s\n",
	        v->net == NET_BRIDGE ? "bridge" : v->net == NET_NAT ? "nat" : "none", ports, v->bridge, v->sound, v->boot_cd ? "cdrom" : "disk");
	if (v->resume[0]) fprintf(f, "resume = %s\n", v->resume);
	fprintf(f, "created = %lld\n", v->created);
	if (fclose(f)) { unlink(tmp); return -1; }
	return rename(tmp, path);
}

static int vm_cmp(const void *a, const void *b)
{
	const Vm *x = a, *y = b;
	if (x->created != y->created) return x->created < y->created ? -1 : 1;
	return strcmp(x->id, y->id);
}

/* 重新讀所有虛擬機器（執行狀態另外用 vm_refresh 更新） */
static void vms_load(void)
{
	char root[PATH_MAX];
	data_root(root, sizeof root);
	nvms = 0;
	DIR *d = opendir(root);
	if (!d) return;
	struct dirent *e;
	while ((e = readdir(d)) && nvms < MAX_VMS) {
		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof vms[0].id) continue;
		char dir[PATH_MAX];
		snprintf(dir, sizeof dir, "%s/%s", root, e->d_name);
		if (!vm_load(&vms[nvms], dir, e->d_name)) nvms++;
	}
	closedir(d);
	qsort(vms, (size_t)nvms, sizeof vms[0], vm_cmp);
}

static Vm *vm_find(const char *key)
{
	for (int i = 0; i < nvms; i++) if (!strcmp(vms[i].id, key)) return &vms[i];
	for (int i = 0; i < nvms; i++) if (!strcmp(vms[i].name, key)) return &vms[i];
	for (int i = 0; i < nvms; i++) if (!strcasecmp(vms[i].name, key)) return &vms[i];
	return NULL;
}

/* 新的 id：vm1、vm2…（用過的不再用，免得舊的 socket 或縮圖搞混） */
static void vm_new_id(char *out, size_t n)
{
	char root[PATH_MAX], p[PATH_MAX];
	data_root(root, sizeof root);
	for (int i = 1; i < 100000; i++) {
		snprintf(out, n, "vm%d", i);
		snprintf(p, sizeof p, "%s/%s", root, out);
		if (access(p, F_OK)) return;
	}
}

/* ------------------------------------------------------------------ */
/* QEMU 在不在、有哪些功能                                               */
/* ------------------------------------------------------------------ */

static char qemu_path[PATH_MAX];
static int qemu_checked;
static char *qemu_help_cache[2];

static const char *qemu_bin(void)
{
	char *bin = qemu_path;
	if (qemu_checked) return bin[0] ? bin : NULL;
	qemu_checked = 1;
	const char *e = getenv("POMELO_VM_QEMU");   /* 測試用 */
	size_t n = sizeof qemu_path;
	if (e && *e) snprintf(bin, n, "%s", e);
	else if (pm_have("qemu-system-x86_64")) snprintf(bin, n, "qemu-system-x86_64");
	else if (!access("/usr/libexec/qemu-kvm", X_OK)) snprintf(bin, n, "/usr/libexec/qemu-kvm");   /* Rocky、RHEL */
	else if (pm_have("qemu-kvm")) snprintf(bin, n, "qemu-kvm");
	return bin[0] ? bin : NULL;
}

/* 剛裝好 QEMU：重新找一次，回傳 1 = 找到了 */
static int qemu_recheck(void)
{
	qemu_checked = 0;
	qemu_path[0] = 0;
	for (int i = 0; i < 2; i++) { free(qemu_help_cache[i]); qemu_help_cache[i] = NULL; }
	return qemu_bin() != NULL;
}

/* ARM64 的模擬器（Debian／Ubuntu：qemu-system-arm 套件；Fedora：qemu-system-aarch64-core） */
static const char *qemu_arm_bin(void)
{
	const char *e = getenv("POMELO_VM_QEMU_ARM");   /* 測試用 */
	if (e && *e) return e;
	return pm_have("qemu-system-aarch64") ? "qemu-system-aarch64" : NULL;
}

static int host_arch(void)
{
	static int a = -1;
	if (a < 0) {
		struct utsname u;
		a = !uname(&u) && (!strcmp(u.machine, "aarch64") || !strcmp(u.machine, "arm64")) ? ARCH_ARM : ARCH_X86;
	}
	return a;
}

static int have_qemu_img(void) { return pm_have("qemu-img"); }

/* qemu -device help、-audiodev help 的輸出（只問一次） */
static const char *qemu_help(int which)
{
	char **cache = qemu_help_cache;
	static const char *opt[2] = { "-device", "-audiodev" };
	if (!cache[which] && qemu_bin()) {
		char cmd[PATH_MAX + 64];
		snprintf(cmd, sizeof cmd, "'%s' %s help", qemu_bin(), opt[which]);
		cache[which] = pm_run(cmd);
	}
	return cache[which] ? cache[which] : "";
}

static int qemu_has_device(const char *dev)
{
	char k[96];
	snprintf(k, sizeof k, "name \"%s\"", dev);
	return strstr(qemu_help(0), k) != NULL;
}

static int qemu_has_audio(const char *drv)
{
	const char *h = qemu_help(1), *p = h;
	size_t l = strlen(drv);
	while ((p = strstr(p, drv))) {
		if ((p == h || p[-1] == '\n') && (p[l] == '\n' || !p[l])) return 1;
		p += l;
	}
	return 0;
}

static int kvm_status(void)
{
	if (getenv("POMELO_VM_NOKVM")) return KVM_NODEV;   /* 測試用 */
	if (access("/dev/kvm", F_OK)) return KVM_NODEV;
	int fd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (fd < 0) return KVM_PERM;
	close(fd);
	return KVM_OK;
}

/* CPU 有沒有硬體虛擬化（沒有 /dev/kvm 時判斷是 BIOS 沒開還是 CPU 根本沒有） */
static int cpu_has_virt(void)
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	if (!f) return 0;
	char line[4096];
	int found = 0;
	while (!found && fgets(line, sizeof line, f))
		if (!strncmp(line, "flags", 5) && (strstr(line, " vmx") || strstr(line, " svm"))) found = 1;
	fclose(f);
	return found;
}

static int have_swtpm(void) { return pm_have("swtpm"); }

/* ------------------------------------------------------------------ */
/* UEFI 韌體：照 QEMU 的韌體說明檔（/usr/share/qemu/firmware 裡的 .json）挑 */
/* ------------------------------------------------------------------ */

typedef struct { char code[PATH_MAX], code_fmt[16], vars[PATH_MAX], vars_fmt[16]; int smm; } Fw;

static int fw_match(const char *file, int secure, int arch, Fw *out)
{
	FILE *f = fopen(file, "r");
	if (!f) return 0;
	char buf[16384];
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	Js *j = js_parse(buf);
	int ok = 0;
	if (!j) return 0;
	Js *map = js_get(j, "mapping"), *exe = js_get(map, "executable"), *nv = js_get(map, "nvram-template");
	Js *feat = js_get(j, "features"), *tg = js_get(j, "targets");
	int arch_ok = 0;
	for (Js *t = tg ? tg->kids : NULL; t; t = t->next) {
		if (strcmp(js_str(js_get(t, "architecture"), ""), arch == ARCH_ARM ? "aarch64" : "x86_64")) continue;
		for (Js *m = js_get(t, "machines") ? js_get(t, "machines")->kids : NULL; m; m = m->next)
			if (m->type == JS_STR && !strncmp(m->str, arch == ARCH_ARM ? "virt" : "pc-q35", arch == ARCH_ARM ? 4 : 6)) arch_ok = 1;
	}
	const char *code = js_str(js_get(exe, "filename"), NULL), *vars = js_str(js_get(nv, "filename"), NULL);
	if (js_has(js_get(j, "interface-types"), "uefi") && !strcmp(js_str(js_get(map, "device"), ""), "flash") && arch_ok &&
	    code && vars && !access(code, R_OK) && !access(vars, R_OK) &&
	    !js_has(feat, "amd-sev-es") && !js_has(feat, "amd-sev-snp") && !js_has(feat, "intel-tdx") &&
	    (secure ? js_has(feat, "secure-boot") && js_has(feat, "enrolled-keys") : !js_has(feat, "secure-boot"))) {
		snprintf(out->code, sizeof out->code, "%s", code);
		snprintf(out->vars, sizeof out->vars, "%s", vars);
		snprintf(out->code_fmt, sizeof out->code_fmt, "%s", js_str(js_get(exe, "format"), "raw"));
		snprintf(out->vars_fmt, sizeof out->vars_fmt, "%s", js_str(js_get(nv, "format"), "raw"));
		out->smm = js_has(feat, "requires-smm");
		ok = 1;
	}
	js_free(j);
	return ok;
}

static int str_cmp(const void *a, const void *b) { return strcmp(base_name(*(char *const *)a), base_name(*(char *const *)b)); }

static int fw_find(int secure, int arch, Fw *out)
{
	memset(out, 0, sizeof *out);
	/* 說明檔依檔名排序（數字小的優先）；/etc 的同名檔蓋過 /usr/share 的 */
	char *files[64];
	int nf = 0;
	const char *test = getenv("POMELO_VM_FIRMWARE");   /* 測試用：多找一個資料夾 */
	const char *dirs[] = { test && *test ? test : "/etc/qemu/firmware", "/etc/qemu/firmware", "/usr/share/qemu/firmware" };
	for (size_t d = 0; d < LEN(dirs); d++) {
		DIR *dd = opendir(dirs[d]);
		struct dirent *e;
		while (dd && (e = readdir(dd)) && nf < (int)LEN(files)) {
			size_t l = strlen(e->d_name);
			if (l < 6 || strcmp(e->d_name + l - 5, ".json")) continue;
			int dup = 0;
			for (int i = 0; i < nf; i++) if (!strcmp(base_name(files[i]), e->d_name)) dup = 1;
			if (dup) continue;
			char p[PATH_MAX];
			snprintf(p, sizeof p, "%s/%s", dirs[d], e->d_name);
			files[nf++] = strdup(p);
		}
		if (dd) closedir(dd);
	}
	qsort(files, (size_t)nf, sizeof files[0], str_cmp);
	int found = 0;
	for (int i = 0; i < nf; i++) {
		if (!found) found = fw_match(files[i], secure, arch, out);
		free(files[i]);
	}
	if (found) return 1;
	/* 沒有說明檔（舊的發行版）：常見的位置 */
	static const struct { const char *code, *vars; int secure, smm, arch; } known[] = {
		{ "/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd", 0, 0, ARCH_X86 },
		{ "/usr/share/OVMF/OVMF_CODE.fd", "/usr/share/OVMF/OVMF_VARS.fd", 0, 0, ARCH_X86 },
		{ "/usr/share/edk2/ovmf/OVMF_CODE.fd", "/usr/share/edk2/ovmf/OVMF_VARS.fd", 0, 0, ARCH_X86 },
		{ "/usr/share/OVMF/OVMF_CODE_4M.ms.fd", "/usr/share/OVMF/OVMF_VARS_4M.ms.fd", 1, 1, ARCH_X86 },
		{ "/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd", "/usr/share/edk2/ovmf/OVMF_VARS.secboot.fd", 1, 1, ARCH_X86 },
		{ "/usr/share/AAVMF/AAVMF_CODE.fd", "/usr/share/AAVMF/AAVMF_VARS.fd", 0, 0, ARCH_ARM },
		{ "/usr/share/edk2/aarch64/QEMU_EFI-pflash.raw", "/usr/share/edk2/aarch64/vars-template-pflash.raw", 0, 0, ARCH_ARM },
	};
	for (size_t i = 0; i < LEN(known); i++)
		if (known[i].secure == secure && known[i].arch == arch && !access(known[i].code, R_OK) && !access(known[i].vars, R_OK)) {
			snprintf(out->code, sizeof out->code, "%s", known[i].code);
			snprintf(out->vars, sizeof out->vars, "%s", known[i].vars);
			snprintf(out->code_fmt, sizeof out->code_fmt, "raw");
			snprintf(out->vars_fmt, sizeof out->vars_fmt, "raw");
			out->smm = known[i].smm;
			return 1;
		}
	return 0;
}

/* ------------------------------------------------------------------ */
/* 執行外部程式                                                          */
/* ------------------------------------------------------------------ */

/* 子行程裡：把 3 以上的檔案描述子都關掉（X 連線之類的不要漏給 QEMU） */
static void close_fds(void)
{
	long max = sysconf(_SC_OPEN_MAX);
	if (max < 0 || max > 65536) max = 65536;
	for (int fd = 3; fd < max; fd++) close(fd);
}

/* 執行 argv，等它結束，回傳結束碼（-1 = 執行不了）；輸出（stdout＋stderr）寫進 out */
static int run_argv(char *const argv[], char *out, size_t n)
{
	int p[2];
	if (out && n) out[0] = 0;
	if (pipe(p)) return -1;
	struct sigaction old, dfl = { .sa_handler = SIG_DFL };
	sigaction(SIGCHLD, &dfl, &old);   /* pm_ui_run 讓子行程自動回收；這裡要拿結束碼 */
	pid_t pid = fork();
	if (pid == 0) {
		dup2(p[1], 1); dup2(p[1], 2);
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0) dup2(nul, 0);
		close_fds();
		setenv("LC_ALL", "C.UTF-8", 1);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	size_t k = 0;
	char buf[1024];
	ssize_t r;
	while ((r = read(p[0], buf, sizeof buf)) > 0 || (r < 0 && errno == EINTR))
		if (r > 0 && out && k + 1 < n) {
			size_t c = MIN((size_t)r, n - 1 - k);
			memcpy(out + k, buf, c);
			k += c;
			out[k] = 0;
		}
	close(p[0]);
	int st = 0, rc = -1;
	if (pid > 0 && waitpid(pid, &st, 0) == pid) rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	sigaction(SIGCHLD, &old, NULL);
	if (rc == 127) rc = -1;
	return rc;
}

/* 錯誤訊息只留最後一行有內容的（qemu-img、QEMU 的錯誤在最後） */
static void last_line(char *s)
{
	size_t l = strlen(s);
	while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0;
	char *nl = strrchr(s, '\n');
	if (nl) memmove(s, nl + 1, strlen(nl + 1) + 1);
}

/* 映像檔的格式與大小（qemu-img info）；回傳 0 = 成功 */
typedef struct { char fmt[16]; unsigned long long vsize, asize; int nsnap; } ImgInfo;

static int img_info(const char *path, ImgInfo *ii)
{
	memset(ii, 0, sizeof *ii);
	char *argv[] = { "qemu-img", "info", "-U", "--output=json", (char *)path, NULL };
	char *out = malloc(65536);
	int rc = run_argv(argv, out, 65536);
	Js *j = rc == 0 ? js_parse(out) : NULL;
	free(out);
	if (!j) return -1;
	snprintf(ii->fmt, sizeof ii->fmt, "%s", js_str(js_get(j, "format"), "raw"));
	ii->vsize = (unsigned long long)js_num(js_get(j, "virtual-size"), 0);
	ii->asize = (unsigned long long)js_num(js_get(j, "actual-size"), 0);
	Js *sn = js_get(j, "snapshots");
	ii->nsnap = sn ? sn->n : 0;
	js_free(j);
	return 0;
}

/* ------------------------------------------------------------------ */
/* QMP（同步：連上、送一個指令、拿回應、斷開）                              */
/* ------------------------------------------------------------------ */

/* 讀一行（timeout 毫秒）；buf 裡可能已經有上次多讀的資料 */
static int qmp_readline(int fd, char *buf, size_t n, size_t *have, char *line, size_t ln, int timeout)
{
	long until = pm_now_ms() + timeout;
	for (;;) {
		char *nl = memchr(buf, '\n', *have);
		if (nl) {
			size_t l = (size_t)(nl - buf);
			snprintf(line, ln, "%.*s", (int)MIN(l, ln - 1), buf);
			memmove(buf, nl + 1, *have - l - 1);
			*have -= l + 1;
			return 0;
		}
		if (*have + 1 >= n) *have = 0;   /* 太長的行（大量事件）：丟掉 */
		long left = until - pm_now_ms();
		if (left <= 0) return -1;
		struct pollfd pf = { fd, POLLIN, 0 };
		if (poll(&pf, 1, (int)left) <= 0) return -1;
		ssize_t r = read(fd, buf + *have, n - 1 - *have);
		if (r <= 0) return -1;
		*have += (size_t)r;
	}
}

static int unix_connect(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof a.sun_path) return -1;
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
	return fd;
}

/* 送一個 QMP 指令（JSON），回傳回應那一行（呼叫者 free），連不上或逾時回傳 NULL */
static char *qmp_call(const Vm *v, const char *cmd, int timeout)
{
	char sock[PATH_MAX];
	vm_run(v, "ctl.sock", sock, sizeof sock);
	int fd = unix_connect(sock);
	if (fd < 0) return NULL;
	char buf[65536], line[65536];
	size_t have = 0;
	char *res = NULL;
	int hello = MIN(timeout, 3000);   /* 別人正連著（例如正在拍快照）時 QEMU 不會理我們：查狀態用短的逾時，介面不會卡住 */
	if (qmp_readline(fd, buf, sizeof buf, &have, line, sizeof line, hello)) goto out;   /* 打招呼 */
	const char *caps = "{\"execute\":\"qmp_capabilities\"}\n";
	if (write(fd, caps, strlen(caps)) < 0) goto out;
	for (int step = 0; step < 2; step++) {
		if (step == 1) {
			char c[4096];
			snprintf(c, sizeof c, "%s\n", cmd);
			if (write(fd, c, strlen(c)) < 0) goto out;
		}
		for (;;) {
			if (qmp_readline(fd, buf, sizeof buf, &have, line, sizeof line, step ? timeout : hello)) goto out;
			if (strstr(line, "\"event\"")) continue;
			if (step == 1) res = strdup(line);
			break;
		}
	}
out:
	close(fd);
	return res;
}

/* 執行 HMP 指令（savevm 之類 QMP 沒有直接對應的），回傳輸出（錯誤訊息）；NULL = 連不上 */
static char *hmp_call(const Vm *v, const char *cmdline, int timeout)
{
	char q[1024], c[1200];
	js_quote(q, sizeof q, cmdline);
	snprintf(c, sizeof c, "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":%s}}", q);
	char *line = qmp_call(v, c, timeout);
	if (!line) return NULL;
	Js *j = js_parse(line);
	free(line);
	char *out = strdup(js_str(js_get(j, "return"), js_str(js_get(js_get(j, "error"), "desc"), "")));
	js_free(j);
	return out;
}

/* 送一個沒有參數的指令，回傳 0 = 成功 */
static int qmp_simple(const Vm *v, const char *exec)
{
	char c[256];
	snprintf(c, sizeof c, "{\"execute\":\"%s\"}", exec);
	char *r = qmp_call(v, c, 5000);
	int ok = r && strstr(r, "\"return\"");
	free(r);
	return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 執行狀態                                                             */
/* ------------------------------------------------------------------ */

/* QEMU 的行程編號（沒在跑回傳 0）：看 pid 檔，再確認那個行程真的是這台的 QEMU */
static pid_t vm_pid(const Vm *v)
{
	char path[PATH_MAX], sock[PATH_MAX];
	vm_run(v, "qemu.pid", path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	long pid = 0;
	if (fscanf(f, "%ld", &pid) != 1) pid = 0;
	fclose(f);
	if (pid <= 1 || kill((pid_t)pid, 0)) return 0;
	snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
	f = fopen(path, "r");
	if (!f) return 0;
	char cmd[16384];
	size_t n = fread(cmd, 1, sizeof cmd - 1, f);
	fclose(f);
	for (size_t i = 0; i < n; i++) if (!cmd[i]) cmd[i] = ' ';
	cmd[n] = 0;
	vm_run(v, "ctl.sock", sock, sizeof sock);
	return strstr(cmd, sock) ? (pid_t)pid : 0;
}

static void vm_refresh(Vm *v)
{
	if (v->state == ST_STARTING) return;
	if (!vm_pid(v)) { v->state = v->resume[0] ? ST_SAVED : ST_OFF; return; }
	char *r = qmp_call(v, "{\"execute\":\"query-status\"}", 300);
	if (r) v->state = strstr(r, "\"running\": false") ? ST_PAUSED : ST_RUNNING;
	else if (v->state != ST_PAUSED) v->state = ST_RUNNING;   /* 忙著（拍快照）：維持原來的狀態 */
	free(r);
}

static const char *state_label(int st)
{
	switch (st) {
	case ST_RUNNING: return _("執行中");
	case ST_PAUSED: return _("已暫停");
	case ST_STARTING: return _("正在開機…");
	case ST_SAVED: return _("已儲存狀態");
	default: return _("已關機");
	}
}

static const char *os_label(int os)
{
	return os == OS_LINUX ? "Linux" : os == OS_WINDOWS ? "Windows" : _("其他");
}

/* ------------------------------------------------------------------ */
/* 建立與刪除                                                            */
/* ------------------------------------------------------------------ */

/* 照目前的韌體選項準備 UEFI 變數檔（vars.qcow2）；換了韌體（例如打開安全開機）時重做 */
static int vm_prepare_fw(Vm *v, int fresh, char *err, size_t en)
{
	if (!v->uefi) return 0;
	char vars[PATH_MAX];
	vm_file(v, "vars.qcow2", vars, sizeof vars);
	if (!fresh && v->fw_code[0] && !access(v->fw_code, R_OK) && !access(vars, R_OK)) return 0;
	Fw fw;
	if (!fw_find(v->secboot, v->arch, &fw)) {
		snprintf(err, en, "%s", v->arch == ARCH_ARM ? _("找不到 ARM64 的 UEFI 韌體，請安裝 qemu-efi-aarch64 套件")
		                        : v->secboot ? _("找不到支援安全開機的 UEFI 韌體（OVMF）") : _("找不到 UEFI 韌體（OVMF），請安裝 ovmf 套件"));
		return -1;
	}
	if (fresh || access(vars, R_OK)) {
		char *argv[] = { "qemu-img", "convert", "-f", fw.vars_fmt, "-O", "qcow2", fw.vars, vars, NULL };
		char out[1024];
		unlink(vars);
		if (run_argv(argv, out, sizeof out)) {
			last_line(out);
			snprintf(err, en, "%s：%s", _("無法建立 UEFI 變數檔"), out);
			return -1;
		}
	}
	snprintf(v->fw_code, sizeof v->fw_code, "%s", fw.code);
	snprintf(v->fw_fmt, sizeof v->fw_fmt, "%s", fw.code_fmt);
	v->fw_smm = fw.smm;
	return 0;
}

/* 建立新的虛擬機器：v 的設定已經填好（id、dir 由這裡決定）；disk_gb > 0 = 建一個新的硬碟，否則用 v->disk */
static int vm_create(Vm *v, int disk_gb, char *err, size_t en)
{
	char root[PATH_MAX];
	data_root(root, sizeof root);
	vm_new_id(v->id, sizeof v->id);
	snprintf(v->dir, sizeof v->dir, "%s/%s", root, v->id);
	if (mkdirs(v->dir, 0700)) { snprintf(err, en, "%s：%s", _("無法建立資料夾"), strerror(errno)); return -1; }
	v->created = (long long)time(NULL);
	if (disk_gb > 0) {
		snprintf(v->disk, sizeof v->disk, "disk.qcow2");
		char path[PATH_MAX], size[32], out[1024];
		vm_disk(v, path, sizeof path);
		snprintf(size, sizeof size, "%dG", disk_gb);
		char *argv[] = { "qemu-img", "create", "-q", "-f", "qcow2", path, size, NULL };
		if (run_argv(argv, out, sizeof out)) {
			last_line(out);
			snprintf(err, en, "%s：%s", _("無法建立虛擬硬碟"), out[0] ? out : _("沒有 qemu-img"));
			goto fail;
		}
	}
	if (vm_prepare_fw(v, 1, err, en)) goto fail;
	if (vm_save(v)) { snprintf(err, en, "%s：%s", _("無法儲存設定"), strerror(errno)); goto fail; }
	return 0;
fail: {
		char p[PATH_MAX];
		const char *files[] = { "disk.qcow2", "vars.qcow2", "vm.conf" };
		for (size_t i = 0; i < LEN(files); i++) { vm_file(v, files[i], p, sizeof p); unlink(p); }
		rmdir(v->dir);
		return -1;
	}
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
	return remove(path);
}

/* 刪除虛擬機器的資料夾（裡面的硬碟一起刪；放在別的地方的映像檔不動） */
static int vm_delete(const Vm *v)
{
	char root[PATH_MAX];
	data_root(root, sizeof root);
	size_t l = strlen(root);
	if (strncmp(v->dir, root, l) || v->dir[l] != '/' || !v->id[0] || strstr(v->id, "..")) return -1;   /* 只刪自己的資料夾 */
	char run[PATH_MAX];
	vm_run(v, "", run, sizeof run);
	nftw(run, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
	return nftw(v->dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* ------------------------------------------------------------------ */
/* 快照（snapshots.tsv：標籤、時間、含記憶體、名稱）                         */
/* ------------------------------------------------------------------ */

#define MAX_SNAPS 64
typedef struct { char tag[64], name[128]; long long t; int ram; } Snap;

static int snaps_load(const Vm *v, Snap *s, int max)
{
	char path[PATH_MAX], line[512];
	vm_file(v, "snapshots.tsv", path, sizeof path);
	FILE *f = fopen(path, "r");
	int n = 0;
	while (f && n < max && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *c = strtok(NULL, "\t"), *d = strtok(NULL, "");
		if (!a || !b || !c || !strcmp(a, RESUME_TAG)) continue;
		snprintf(s[n].tag, sizeof s[n].tag, "%s", a);
		s[n].t = atoll(b);
		s[n].ram = atoi(c);
		snprintf(s[n].name, sizeof s[n].name, "%s", d ? d : a);
		n++;
	}
	if (f) fclose(f);
	return n;
}

static void snaps_save(const Vm *v, const Snap *s, int n)
{
	char path[PATH_MAX], tmp[PATH_MAX + 8];
	vm_file(v, "snapshots.tsv", path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) return;
	for (int i = 0; i < n; i++) {
		char name[128];
		snprintf(name, sizeof name, "%s", s[i].name);
		for (char *p = name; *p; p++) if (*p == '\t' || *p == '\n') *p = ' ';
		fprintf(f, "%s\t%lld\t%d\t%s\n", s[i].tag, s[i].t, s[i].ram, name);
	}
	fclose(f);
	rename(tmp, path);
}

/* 能不能拍快照：硬碟（還有 UEFI 變數檔）要是 qcow2 */
static int vm_can_snapshot(const Vm *v)
{
	char path[PATH_MAX];
	vm_disk(v, path, sizeof path);
	size_t l = strlen(path);
	if (l > 6 && !strcmp(path + l - 6, ".qcow2")) return 1;
	ImgInfo ii;
	return !img_info(path, &ii) && !strcmp(ii.fmt, "qcow2");
}

/* 關機時用 qemu-img 對硬碟與變數檔做快照動作：op 是 -c（拍）、-a（回到）、-d（刪除） */
static int img_snapshot(const Vm *v, const char *op, const char *tag, char *err, size_t en)
{
	char disk[PATH_MAX], vars[PATH_MAX], out[1024];
	vm_disk(v, disk, sizeof disk);
	vm_file(v, "vars.qcow2", vars, sizeof vars);
	const char *files[2] = { disk, v->uefi && !access(vars, R_OK) ? vars : NULL };
	for (int i = 0; i < 2; i++) {
		if (!files[i]) continue;
		char *argv[] = { "qemu-img", "snapshot", (char *)op, (char *)tag, (char *)files[i], NULL };
		if (run_argv(argv, out, sizeof out) && !(i == 1 && strcmp(op, "-c"))) {   /* 變數檔沒有這個快照（舊的）不算錯 */
			last_line(out);
			snprintf(err, en, "%s", out);
			return -1;
		}
	}
	return 0;
}

/* HMP 的 savevm／loadvm／delvm 成功時沒有輸出，有字就是錯誤訊息 */
static int hmp_ok(const Vm *v, const char *cmd, int timeout, char *err, size_t en)
{
	char *r = hmp_call(v, cmd, timeout);
	if (!r) { snprintf(err, en, "%s", _("跟虛擬機器的連線中斷了")); return -1; }
	int ok = !r[0];
	if (!ok) { last_line(r); snprintf(err, en, "%s", r); }
	free(r);
	return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* 開機                                                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 橋接                                                                 */
/* ------------------------------------------------------------------ */

static const char *bridge_helper(void)
{
	static const char *paths[] = { "/usr/lib/qemu/qemu-bridge-helper", "/usr/libexec/qemu-bridge-helper" };
	for (size_t i = 0; i < LEN(paths); i++) if (!access(paths[i], X_OK)) return paths[i];
	return NULL;
}

/* 一般使用者能不能用 qemu-bridge-helper：要 setuid root，或有 cap_net_admin（Fedora） */
static int bridge_helper_ok(void)
{
	const char *h = bridge_helper();
	struct stat st;
	if (!h || stat(h, &st)) return 0;
	if ((st.st_mode & S_ISUID) && st.st_uid == 0) return 1;
	char cap[64];
	return getxattr(h, "security.capability", cap, sizeof cap) > 0;
}

/* /etc/qemu/bridge.conf 有沒有允許這個橋接器：1 = 有、0 = 沒有、-1 = 讀不到（Fedora 只有 root 讀得到） */
static int bridge_allowed(const char *br)
{
	FILE *f = fopen("/etc/qemu/bridge.conf", "r");
	if (!f) return access("/etc/qemu/bridge.conf", F_OK) ? 0 : -1;
	char line[256];
	int ok = 0;
	while (!ok && fgets(line, sizeof line, f)) {
		char w[16], name[64];
		if (sscanf(line, "%15s %63s", w, name) == 2 && !strcmp(w, "allow") && (!strcmp(name, "all") || !strcmp(name, br))) ok = 1;
		else if (!strncmp(line, "include", 7)) ok = -1;   /* 另外引入的檔案不追：當作不知道 */
	}
	fclose(f);
	return ok;
}

static int net_is(const char *ifname, const char *what)
{
	char p[PATH_MAX];
	snprintf(p, sizeof p, "/sys/class/net/%s/%s", ifname, what);
	return !access(p, F_OK);
}

/* Docker、Podman 自己的橋接器（它們做 NAT，接上去也到不了區域網路）不列出來 */
static int foreign_bridge(const char *n)
{
	if (!strcmp(n, "docker0") || !strncmp(n, "cni", 3) || !strncmp(n, "podman", 6)) return 1;
	if (!strncmp(n, "br-", 3) && strlen(n) == 15) {
		for (const char *p = n + 3; *p; p++) if (!isxdigit((unsigned char)*p)) return 0;
		return 1;
	}
	return 0;
}

/* 這台電腦的橋接器（kind = 0）或可以拿來橋接的有線網路卡（kind = 1；Wi-Fi 沒辦法橋接） */
static int list_ifaces(int kind, char names[][32], int max)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	int n = 0;
	while (d && (e = readdir(d)) && n < max) {
		const char *nm = e->d_name;
		if (nm[0] == '.' || strlen(nm) >= 32) continue;
		if (kind == 0 ? net_is(nm, "bridge") && !foreign_bridge(nm)
		              : net_is(nm, "device") && !net_is(nm, "wireless") && !net_is(nm, "phy80211") && !net_is(nm, "bridge") && !net_is(nm, "brport"))
			snprintf(names[n++], 32, "%s", nm);
	}
	if (d) closedir(d);
	return n;
}

/* QEMU 選項裡的路徑：逗號要寫兩次 */
static void qesc(char *out, size_t n, const char *path)
{
	size_t k = 0;
	for (const char *p = path; *p && k + 3 < n; p++) {
		out[k++] = *p;
		if (*p == ',') out[k++] = ',';
	}
	out[k] = 0;
}

typedef struct { char *argv[96]; int n; char buf[16384]; size_t used; } Args;

static void arg(Args *a, const char *fmt, ...)
{
	if (a->n >= (int)LEN(a->argv) - 1) return;
	va_list ap;
	va_start(ap, fmt);
	int l = vsnprintf(a->buf + a->used, sizeof a->buf - a->used, fmt, ap);
	va_end(ap);
	if (l < 0 || a->used + (size_t)l + 1 > sizeof a->buf) return;
	a->argv[a->n++] = a->buf + a->used;
	a->argv[a->n] = NULL;
	a->used += (size_t)l + 1;
}

/* 聲音交給誰：QEMU 自己接 PulseAudio／PipeWire（Fedora、Rocky 內建；Ubuntu 要另外裝 qemu-system-gui），
 * 沒有的話 QEMU 用 none，把聲音透過 VNC 送給畫面視窗播 */
static const char *audio_driver(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR"), *force = getenv("POMELO_VM_AUDIO");   /* 測試用：none = 走畫面視窗 */
	char p[PATH_MAX];
	if (force && *force) return force;
	if (rt && *rt) {
		snprintf(p, sizeof p, "%s/pulse/native", rt);
		if (!access(p, F_OK) && qemu_has_audio("pa")) return "pa";
		snprintf(p, sizeof p, "%s/pipewire-0", rt);
		if (!access(p, F_OK) && qemu_has_audio("pipewire")) return "pipewire";
	}
	return "none";
}

/* 轉送連接埠：「2222:22, 8080:80」→ hostfwd=tcp:127.0.0.1:2222-:22（只聽本機） */
static void add_ports(char *nic, size_t n, const char *ports)
{
	const char *p = ports;
	while (*p) {
		while (*p && !isdigit((unsigned char)*p)) p++;
		if (!*p) break;
		char *e;
		long h = strtol(p, &e, 10), g = h;
		p = e;
		while (*p == ' ' || *p == ':' || *p == '-' || *p == '>') p++;
		if (isdigit((unsigned char)*p)) { g = strtol(p, &e, 10); p = e; }
		if (h > 0 && h < 65536 && g > 0 && g < 65536) {
			size_t l = strlen(nic);
			snprintf(nic + l, n - l, ",hostfwd=tcp:127.0.0.1:%ld-:%ld", h, g);
		}
	}
}

/* 組出 QEMU 的參數。loadvm：從這個快照開始（NULL = 正常開機） */
static int vm_args(Vm *v, Args *a, const char *loadvm, char *err, size_t en)
{
	char t[PATH_MAX * 2 + 64], e1[PATH_MAX * 2], run[PATH_MAX];
	int arm = v->arch == ARCH_ARM;
	const char *bin = arm ? qemu_arm_bin() : qemu_bin();
	if (!bin) { snprintf(err, en, "%s", arm ? _("還沒有安裝 ARM64 模擬器（qemu-system-aarch64）") : _("還沒有安裝 QEMU")); return -1; }
	int kvm = kvm_status() == KVM_OK && host_arch() == v->arch;   /* 不同架構只能軟體模擬 */
	memset(a, 0, sizeof *a);
	arg(a, "%s", bin);
	arg(a, "-name"); arg(a, "%s,process=pomelo-vm-%s", v->id, v->id);
	if (arm) { arg(a, "-machine"); arg(a, "virt,accel=%s%s", kvm ? "kvm" : "tcg", kvm ? ",gic-version=host" : ",gic-version=max"); }
	else { arg(a, "-machine"); arg(a, "q35,accel=%s%s", kvm ? "kvm" : "tcg", v->uefi && v->fw_smm ? ",smm=on" : ""); }
	arg(a, "-cpu");
	if (!kvm) arg(a, "max");
	else if (v->os == OS_WINDOWS && !arm) arg(a, "host,hv_relaxed,hv_vapic,hv_spinlocks=0x1fff,hv_time");
	else arg(a, "host");
	arg(a, "-smp"); arg(a, "%d,sockets=1,cores=%d,threads=1", v->cpus, v->cpus);   /* Windows 家用版只認一個 CPU 插槽 */
	arg(a, "-m"); arg(a, "%d", v->mem);
	arg(a, "-rtc"); arg(a, "base=%s", v->os == OS_WINDOWS ? "localtime" : "utc");
	if (v->uefi || arm) {
		char vars[PATH_MAX];
		if (vm_prepare_fw(v, 0, err, en)) return -1;
		vm_file(v, "vars.qcow2", vars, sizeof vars);
		qesc(e1, sizeof e1, v->fw_code);
		arg(a, "-drive"); arg(a, "if=pflash,format=%s,readonly=on,file=%s", v->fw_fmt[0] ? v->fw_fmt : "raw", e1);
		qesc(e1, sizeof e1, vars);
		arg(a, "-drive"); arg(a, "if=pflash,format=qcow2,file=%s", e1);
		if (v->fw_smm) { arg(a, "-global"); arg(a, "driver=cfi.pflash01,property=secure,value=on"); }
	}
	/* 硬碟 */
	char disk[PATH_MAX];
	vm_disk(v, disk, sizeof disk);
	if (access(disk, R_OK | W_OK)) { snprintf(err, en, "%s：%s", _("找不到虛擬硬碟或沒有權限"), disk); return -1; }
	ImgInfo ii;
	size_t dl = strlen(disk);
	if (dl > 6 && !strcmp(disk + dl - 6, ".qcow2")) snprintf(ii.fmt, sizeof ii.fmt, "qcow2");
	else if (img_info(disk, &ii)) snprintf(ii.fmt, sizeof ii.fmt, "raw");
	qesc(e1, sizeof e1, disk);
	arg(a, "-drive"); arg(a, "file=%s,if=none,id=disk0,format=%s,discard=unmap", e1, ii.fmt);
	if (v->bus == BUS_SATA && !arm) { arg(a, "-device"); arg(a, "ide-hd,bus=ide.0,drive=disk0,bootindex=%d", v->boot_cd ? 2 : 1); }
	else { arg(a, "-device"); arg(a, "virtio-blk-pci,drive=disk0,bootindex=%d", v->boot_cd ? 2 : 1); }
	/* 光碟機（一直都在，沒放光碟時是空的，開機後可以換） */
	if (v->iso[0] && !access(v->iso, R_OK)) {
		qesc(e1, sizeof e1, v->iso);
		arg(a, "-drive"); arg(a, "if=none,id=cd0,media=cdrom,readonly=on,format=raw,file=%s", e1);
	} else { arg(a, "-drive"); arg(a, "if=none,id=cd0,media=cdrom,readonly=on"); }
	if (arm) {   /* virt 機型沒有 IDE：光碟接在 VirtIO SCSI 上 */
		arg(a, "-device"); arg(a, "virtio-scsi-pci,id=scsi0");
		arg(a, "-device"); arg(a, "scsi-cd,bus=scsi0.0,drive=cd0,id=cd,bootindex=%d", v->boot_cd ? 1 : 2);
	} else { arg(a, "-device"); arg(a, "ide-cd,bus=ide.2,drive=cd0,id=cd,bootindex=%d", v->boot_cd ? 1 : 2); }
	/* 網路 */
	const char *model = v->nic == NIC_E1000E && !arm ? "e1000e" : "virtio-net-pci";
	if (v->net == NET_NAT) {
		char nic[512];
		snprintf(nic, sizeof nic, "user,model=%s", model);
		add_ports(nic, sizeof nic, v->ports);
		arg(a, "-nic"); arg(a, "%s", nic);
	} else if (v->net == NET_BRIDGE) {
		/* 橋接：QEMU 透過 qemu-bridge-helper（setuid）建 tap 接到橋接器，/etc/qemu/bridge.conf 要允許 */
		if (!v->bridge[0]) { snprintf(err, en, "%s", _("還沒選要接到哪個橋接器")); return -1; }
		const char *h = bridge_helper();
		if (h) { arg(a, "-nic"); arg(a, "bridge,br=%s,helper=%s,model=%s", v->bridge, h, model); }
		else { arg(a, "-nic"); arg(a, "bridge,br=%s,model=%s", v->bridge, model); }
	} else { arg(a, "-nic"); arg(a, "none"); }
	/* 顯示卡、滑鼠（USB 繪圖板：絕對座標，不用抓住滑鼠） */
	if (arm) { arg(a, "-device"); arg(a, "virtio-gpu-pci"); }
	else if (v->display == DISP_VIRTIO && qemu_has_device("virtio-vga")) { arg(a, "-vga"); arg(a, "none"); arg(a, "-device"); arg(a, "virtio-vga"); }
	else { arg(a, "-vga"); arg(a, "std"); }
	arg(a, "-device"); arg(a, "qemu-xhci,id=xhci");
	arg(a, "-device"); arg(a, "usb-tablet,bus=xhci.0");
	if (arm) { arg(a, "-device"); arg(a, "usb-kbd,bus=xhci.0"); }   /* virt 機型沒有 PS/2 鍵盤 */
	if (v->os == OS_LINUX && (arm || qemu_has_device("virtio-rng-pci"))) { arg(a, "-device"); arg(a, "virtio-rng-pci"); }
	/* 聲音 */
	const char *adrv = v->sound ? audio_driver() : NULL;
	if (adrv) {
		arg(a, "-audiodev"); arg(a, "%s,id=snd0", adrv);
		arg(a, "-device"); arg(a, arm ? "intel-hda" : "ich9-intel-hda");
		arg(a, "-device"); arg(a, "hda-duplex,audiodev=snd0");
	}
	/* TPM 2.0（swtpm 在開機前先啟動） */
	vm_run(v, "", run, sizeof run);
	if (v->tpm && !arm) {
		arg(a, "-chardev"); arg(a, "socket,id=chrtpm,path=%sswtpm.sock", run);
		arg(a, "-tpmdev"); arg(a, "emulator,id=tpm0,chardev=chrtpm");
		arg(a, "-device"); arg(a, "tpm-crb,tpmdev=tpm0");
	}
	/* 控制與畫面 */
	arg(a, "-qmp"); arg(a, "unix:%sqmp.sock,server=on,wait=off", run);
	arg(a, "-qmp"); arg(a, "unix:%sctl.sock,server=on,wait=off", run);
	arg(a, "-vnc"); arg(a, "unix:%svnc.sock%s", run, adrv && !strcmp(adrv, "none") ? ",audiodev=snd0" : "");
	arg(a, "-display"); arg(a, "none");
	arg(a, "-pidfile"); arg(a, "%sqemu.pid", run);
	arg(a, "-daemonize");
	if (loadvm) { arg(a, "-loadvm"); arg(a, "%s", loadvm); }
	/* 給畫面視窗看：聲音要不要自己播 */
	snprintf(t, sizeof t, "%saudio", run);
	FILE *f = fopen(t, "w");
	if (f) { fprintf(f, "%s\n", adrv && !strcmp(adrv, "none") ? "vnc" : "direct"); fclose(f); }
	return 0;
}

/* 開機：在背景啟動 QEMU（先 swtpm），回傳讀錯誤訊息用的 fd（QEMU 初始化完、讀到 EOF 就是結束了）；-1 = 失敗 */
static int vm_start(Vm *v, const char *loadvm, char *err, size_t en)
{
	char run[PATH_MAX], p[PATH_MAX];
	vm_run(v, "", run, sizeof run);
	if (mkdirs(run, 0700)) { snprintf(err, en, "%s：%s", _("無法建立執行資料夾"), strerror(errno)); return -1; }
	if (vm_pid(v)) { snprintf(err, en, "%s", _("已經在執行了")); return -1; }
	const char *stale[] = { "qmp.sock", "ctl.sock", "vnc.sock", "swtpm.sock", "qemu.pid", "swtpm.pid" };
	for (size_t i = 0; i < LEN(stale); i++) { vm_run(v, stale[i], p, sizeof p); unlink(p); }
	if (v->tpm && v->arch != ARCH_ARM && !have_swtpm()) { snprintf(err, en, "%s", _("TPM 需要 swtpm，請先安裝")); return -1; }
	Args a;
	if (vm_args(v, &a, loadvm, err, en)) return -1;
	if (getenv("POMELO_VM_DEBUG")) {
		for (int i = 0; i < a.n; i++) fprintf(stderr, "%s ", a.argv[i]);
		fprintf(stderr, "\n");
	}
	char tpmdir[PATH_MAX], tpmarg[PATH_MAX + 16], ctrl[PATH_MAX + 32], pidf[PATH_MAX + 16], logf[PATH_MAX + 16];
	vm_file(v, "tpm", tpmdir, sizeof tpmdir);
	if (v->tpm) mkdirs(tpmdir, 0700);
	snprintf(tpmarg, sizeof tpmarg, "dir=%s", tpmdir);
	snprintf(ctrl, sizeof ctrl, "type=unixio,path=%sswtpm.sock", run);
	snprintf(pidf, sizeof pidf, "file=%sswtpm.pid", run);
	snprintf(logf, sizeof logf, "file=%s/swtpm.log", v->dir);
	int pfd[2];
	if (pipe(pfd)) { snprintf(err, en, "%s", strerror(errno)); return -1; }
	pid_t pid = fork();
	if (pid < 0) { close(pfd[0]); close(pfd[1]); snprintf(err, en, "%s", strerror(errno)); return -1; }
	if (pid == 0) {
		setsid();
		struct sigaction dfl = { .sa_handler = SIG_DFL };
		sigaction(SIGCHLD, &dfl, NULL);
		int nul = open("/dev/null", O_RDWR);
		dup2(nul, 0); dup2(nul, 1); dup2(pfd[1], 2);
		close_fds();
		if (v->tpm) {
			pid_t t = fork();
			if (t == 0) {   /* swtpm 自己變成背景程式：輸出不能接在 QEMU 的錯誤訊息管道上，不然那邊永遠等不到結束 */
				int n2 = open("/dev/null", O_RDWR);
				if (n2 >= 0) dup2(n2, 2);
				execlp("swtpm", "swtpm", "socket", "--tpm2", "--tpmstate", tpmarg, "--ctrl", ctrl,
				       "--terminate", "--daemon", "--pid", pidf, "--log", logf, (char *)NULL);
				_exit(127);
			}
			int st = 0;
			if (t < 0 || waitpid(t, &st, 0) != t || !WIFEXITED(st) || WEXITSTATUS(st)) {
				fprintf(stderr, "swtpm: %s\n", "failed to start");
				_exit(1);
			}
		}
		execvp(a.argv[0], a.argv);
		fprintf(stderr, "%s: %s\n", a.argv[0], strerror(errno));
		_exit(127);
	}
	close(pfd[1]);
	fcntl(pfd[0], F_SETFL, O_NONBLOCK);
	fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
	return pfd[0];
}

/* QEMU 的錯誤訊息改成看得懂的話（找不到對應的就原文照登） */
static void explain_qemu_error(char *msg, size_t n)
{
	last_line(msg);
	const char *m = msg;
	const char *nice = NULL;
	if (strstr(m, "Could not access KVM") || strstr(m, "failed to initialize kvm") || strstr(m, "/dev/kvm"))
		nice = _("無法使用 KVM 硬體加速（/dev/kvm）");
	else if (strstr(m, "Cannot allocate memory") || strstr(m, "cannot set up guest memory"))
		nice = _("記憶體不夠：請關掉一些程式，或把虛擬機器的記憶體調小");
	else if (strstr(m, "Failed to get \"write\" lock") || strstr(m, "Is another process using the image"))
		nice = _("硬碟映像檔正被別的程式使用（可能有另一台虛擬機器在用同一個檔案）");
	else if (strstr(m, "No such file or directory") && strstr(m, "iso"))
		nice = _("找不到光碟映像檔");
	else if (strstr(m, "bridge helper") || strstr(m, "access denied by acl") || strstr(m, "bridge.conf") || strstr(m, "tun device"))
		nice = _("沒有權限使用橋接器：請在設定頁的「網路」按「允許使用」");
	else if (strstr(m, "swtpm"))
		nice = _("TPM（swtpm）啟動失敗");
	else if (strstr(m, "Snapshot") && strstr(m, "does not exist"))
		nice = _("儲存的狀態不見了，請改成一般開機");
	if (nice) {
		char orig[512];
		snprintf(orig, sizeof orig, "%s", msg);
		snprintf(msg, n, "%s（%s）", nice, orig);
	}
}
