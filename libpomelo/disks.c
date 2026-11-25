/* SPDX-License-Identifier: GPL-3.0-or-later */
/* libpomelo — 磁碟與外接裝置（透過 udisks2）
 *
 * 讀 `udisksctl dump` 取得所有磁碟、分割區、檔案系統與健康狀態；
 * 掛載、退出、格式化也都交給 udisks2，外接裝置不需要管理員密碼（polkit 預設允許）。
 */
#define _GNU_SOURCE
#include "pomelo.h"
#include <ctype.h>

static PmDrive drives[32];
static PmVolume vols[128];
static int ndrives, nvols;

/* "  鍵:   值" → 鍵與去掉前後空白／引號的值 */
static int split_kv(char *line, char **k, char **v)
{
	char *c = strchr(line, ':');
	if (!c) return 0;
	*c = 0;
	*k = line;
	while (**k == ' ') (*k)++;
	char *s = c + 1;
	while (*s == ' ' || *s == '\t') s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0;
	if (n >= 2 && s[0] == '\'' && s[n - 1] == '\'') { s[n - 1] = 0; s++; }
	*v = s;
	return 1;
}

static const char *base_obj(const char *path)
{
	const char *b = strrchr(path, '/');
	return b ? b + 1 : path;
}

int pm_disks_scan(void)
{
	ndrives = nvols = 0;
	char *t = pm_run("udisksctl dump");
	if (!t) return -1;
	/* 沒有輸出：沒裝 udisks2，或是真的一個裝置都沒有 */
	if (!*t) { free(t); return pm_have("udisksctl") ? 0 : -1; }
	enum { O_NONE, O_BLOCK, O_DRIVE } otype = O_NONE;
	char iface[64] = "";
	PmVolume *v = NULL;
	PmDrive *d = NULL;
	char *save = NULL;
	for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		if (l[0] == '/') {   /* 新物件 */
			otype = O_NONE;
			v = NULL; d = NULL;
			iface[0] = 0;
			l[strcspn(l, ":")] = 0;
			if (strstr(l, "/block_devices/") && nvols < (int)LEN(vols)) {
				v = &vols[nvols++];
				memset(v, 0, sizeof *v);
				snprintf(v->obj, sizeof v->obj, "%s", l);
				otype = O_BLOCK;
			} else if (strstr(l, "/drives/") && ndrives < (int)LEN(drives)) {
				d = &drives[ndrives++];
				memset(d, 0, sizeof *d);
				snprintf(d->obj, sizeof d->obj, "%s", l);
				d->temp_c = -1000;
				d->power_on_hours = -1;
				otype = O_DRIVE;
			}
			continue;
		}
		if (otype == O_NONE) continue;
		if (!strncmp(l, "  org.freedesktop.UDisks2.", 26) && l[2] != ' ') {
			snprintf(iface, sizeof iface, "%s", l + 26);
			iface[strcspn(iface, ":")] = 0;
			if (v && !strcmp(iface, "Filesystem")) v->is_fs = 1;
			if (v && !strcmp(iface, "Partition")) v->is_part = 1;
			if (v && !strcmp(iface, "PartitionTable")) v->has_table = 1;
			if (v && !strcmp(iface, "Loop")) v->is_loop = 1;
			if (d && !strcmp(iface, "NVMe.Controller")) d->nvme = 1;
			continue;
		}
		char *k, *val;
		if (strncmp(l, "    ", 4) || l[4] == ' ' || !split_kv(l, &k, &val)) continue;
		if (v && !strcmp(iface, "Block")) {
			if (!strcmp(k, "Device")) snprintf(v->dev, sizeof v->dev, "%s", val);
			else if (!strcmp(k, "Drive") && strcmp(val, "/")) snprintf(v->drive, sizeof v->drive, "%s", val);
			else if (!strcmp(k, "HintIgnore")) v->ignore = !strcmp(val, "true");
			else if (!strcmp(k, "HintSystem")) v->system = !strcmp(val, "true");
			else if (!strcmp(k, "HintAuto")) v->hint_auto = !strcmp(val, "true");
			else if (!strcmp(k, "HintName") && *val) snprintf(v->label, sizeof v->label, "%s", val);
			else if (!strcmp(k, "IdLabel") && *val) snprintf(v->label, sizeof v->label, "%s", val);
			else if (!strcmp(k, "IdType")) snprintf(v->fstype, sizeof v->fstype, "%s", val);
			else if (!strcmp(k, "IdUUID")) snprintf(v->uuid, sizeof v->uuid, "%s", val);
			else if (!strcmp(k, "IdUsage")) v->crypto = !strcmp(val, "crypto");
			else if (!strcmp(k, "Size")) v->size = strtoull(val, NULL, 10);
		} else if (v && !strcmp(iface, "Filesystem")) {
			if (!strcmp(k, "MountPoints") && *val) snprintf(v->mount, sizeof v->mount, "%s", val);
		} else if (v && !strcmp(iface, "Partition")) {
			if (!strcmp(k, "Table")) snprintf(v->table, sizeof v->table, "%s", val);
		} else if (d && !strcmp(iface, "Drive")) {
			if (!strcmp(k, "Model")) snprintf(d->model, sizeof d->model, "%s", val);
			else if (!strcmp(k, "Vendor")) snprintf(d->vendor, sizeof d->vendor, "%s", val);
			else if (!strcmp(k, "ConnectionBus")) snprintf(d->bus, sizeof d->bus, "%s", val);
			else if (!strcmp(k, "Removable")) d->removable |= !strcmp(val, "true");
			else if (!strcmp(k, "MediaRemovable")) d->removable |= !strcmp(val, "true");
			else if (!strcmp(k, "Ejectable")) d->ejectable = !strcmp(val, "true");
			else if (!strcmp(k, "CanPowerOff")) d->can_power_off = !strcmp(val, "true");
			else if (!strcmp(k, "RotationRate")) d->rotation = atoi(val);
			else if (!strcmp(k, "Size")) d->size = strtoull(val, NULL, 10);
		} else if (d && !strcmp(iface, "Drive.Ata")) {
			if (!strcmp(k, "SmartSupported")) d->smart = !strcmp(val, "true");
			else if (!strcmp(k, "SmartFailing")) d->failing = !strcmp(val, "true");
			else if (!strcmp(k, "SmartNumBadSectors")) d->bad_sectors = atol(val);
			else if (!strcmp(k, "SmartNumAttributesFailing")) d->attrs_failing = atoi(val);
			else if (!strcmp(k, "SmartPowerOnSeconds") && atol(val) > 0) d->power_on_hours = atol(val) / 3600;
			else if (!strcmp(k, "SmartTemperature") && atof(val) > 200) d->temp_c = atof(val) - 273.15;
		} else if (d && !strcmp(iface, "NVMe.Controller")) {
			d->smart = 1;
			if (!strcmp(k, "SmartCriticalWarning")) d->failing = *val != 0;   /* 有任何警告旗標 */
			else if (!strcmp(k, "SmartPowerOnHours")) d->power_on_hours = atol(val);
			else if (!strcmp(k, "SmartTemperature") && atoi(val) > 200) d->temp_c = atoi(val) - 273.15;
		}
	}
	free(t);

	/* 補上關聯：每個磁區屬於哪顆硬碟、整顆硬碟的裝置名稱、是不是外接 */
	for (int i = 0; i < nvols; i++) {
		PmVolume *x = &vols[i];
		x->whole = !x->is_part;
		for (int j = 0; j < ndrives; j++) {
			if (strcmp(x->drive, drives[j].obj)) continue;
			x->d = &drives[j];
			if (x->whole && !drives[j].dev[0]) snprintf(drives[j].dev, sizeof drives[j].dev, "%s", x->dev);
		}
	}
	for (int j = 0; j < ndrives; j++) {
		PmDrive *y = &drives[j];
		y->external = y->removable || !strcmp(y->bus, "usb") || !strcmp(y->bus, "sdio") || !strcmp(y->bus, "ieee1394");
		if (!y->dev[0]) snprintf(y->dev, sizeof y->dev, "%s", base_obj(y->obj));
	}
	for (int i = 0; i < nvols; i++) {
		PmVolume *x = &vols[i];
		/* 系統磁碟上不能動的（例如 / 所在的硬碟）一律不算外接 */
		x->external = x->d && x->d->external && !x->system && !x->is_loop;
	}
	/* 硬碟上有沒有系統正在用的分割區（/、/boot、/home、swap…）→ 不能格式化 */
	for (int i = 0; i < nvols; i++) {
		PmVolume *x = &vols[i];
		if (!x->d) continue;
		int sysmnt = x->mount[0] && (!strcmp(x->mount, "/") || !strncmp(x->mount, "/boot", 5) ||
		             !strcmp(x->mount, "/home") || !strncmp(x->mount, "/usr", 4) || !strncmp(x->mount, "/var", 4));
		/* LVM、RAID、加密容器的成員：上面可能是系統分割區，同樣不能動 */
		if (sysmnt || !strcmp(x->fstype, "swap") || !strcmp(x->fstype, "LVM2_member") ||
		    !strcmp(x->fstype, "linux_raid_member") || (x->system && x->crypto))
			x->d->in_use_by_system = 1;
	}
	return 0;
}

PmDrive *pm_drives(int *n) { *n = ndrives; return drives; }
PmVolume *pm_volumes(int *n) { *n = nvols; return vols; }

PmVolume *pm_volume_by_dev(const char *dev)
{
	for (int i = 0; i < nvols; i++) if (!strcmp(vols[i].dev, dev)) return &vols[i];
	return NULL;
}

const char *pm_volume_name(const PmVolume *v, char *buf, size_t n)
{
	if (v->label[0]) snprintf(buf, n, "%s", v->label);
	else if (v->d && v->d->model[0]) {
		char sz[32];
		pm_human_size(sz, sizeof sz, (double)v->size);
		snprintf(buf, n, "%s %s", sz, v->d->external ? pm_tr("外接磁碟") : pm_tr("磁碟"));
	} else snprintf(buf, n, "%s", base_obj(v->dev));
	return buf;
}

void pm_human_size(char *out, size_t n, double b)
{
	/* 磁碟容量用 1000 進位，跟包裝上寫的一樣 */
	const char *u[] = { "B", "KB", "MB", "GB", "TB", "PB" };
	int i = 0;
	while (b >= 1000 && i < 5) { b /= 1000; i++; }
	snprintf(out, n, i == 0 ? "%.0f %s" : b < 10 ? "%.1f %s" : "%.0f %s", b, u[i]);
}

/* 執行 udisksctl / busctl，成功回傳 0；錯誤訊息放在 err */
static int run_cmd(const char *cmd, char *err, size_t n)
{
	char full[1024];
	snprintf(full, sizeof full, "LC_ALL=C.UTF-8 %s 2>&1; echo \"@rc=$?\"", cmd);
	FILE *f = popen(full, "r");
	if (!f) { if (err) snprintf(err, n, "popen"); return -1; }
	char out[2048] = "";
	size_t len = fread(out, 1, sizeof out - 1, f);
	out[len] = 0;
	pclose(f);
	char *rc = strstr(out, "@rc=");
	int code = rc ? atoi(rc + 4) : -1;
	if (rc) *rc = 0;
	if (err) {
		/* udisksctl 的錯誤長這樣：Error mounting /dev/sdb1: GDBus.Error:...: 真正的原因 */
		char *msg = out, *p;
		while ((p = strstr(msg, ": ")) && strstr(p + 2, ": ")) msg = p + 2;
		if ((p = strstr(out, "GDBus.Error:")) && (p = strstr(p, ": "))) msg = p + 2;
		msg[strcspn(msg, "\n")] = 0;
		snprintf(err, n, "%s", msg);
	}
	return code;
}

/* 裝置路徑只允許安全字元，避免組指令時出問題 */
static int safe_dev(const char *s)
{
	if (strncmp(s, "/dev/", 5)) return 0;
	for (const char *p = s; *p; p++)
		if (!isalnum((unsigned char)*p) && !strchr("/_-.:", *p)) return 0;
	return 1;
}

int pm_volume_mount(const char *dev, char *mountpoint, size_t n, char *err, size_t en)
{
	if (!safe_dev(dev)) return -1;
	char cmd[256], out[512];
	snprintf(cmd, sizeof cmd, "udisksctl mount --no-user-interaction -b %s", dev);
	int rc = run_cmd(cmd, out, sizeof out);
	if (rc) { if (err) snprintf(err, en, "%s", out); return rc; }
	/* "Mounted /dev/sdb1 at /media/user/LABEL" */
	if (mountpoint) {
		pm_disks_scan();
		PmVolume *v = pm_volume_by_dev(dev);
		snprintf(mountpoint, n, "%s", v ? v->mount : "");
	}
	return 0;
}

int pm_volume_unmount(const char *dev, char *err, size_t en)
{
	if (!safe_dev(dev)) return -1;
	char cmd[256];
	snprintf(cmd, sizeof cmd, "udisksctl unmount --no-user-interaction -b %s", dev);
	return run_cmd(cmd, err, en);
}

/* 退出整顆外接硬碟：卸載上面所有分割區，再切斷電源（可以安全拔除） */
int pm_drive_eject(const char *drive_obj, char *err, size_t en)
{
	pm_disks_scan();
	PmDrive *d = NULL;
	for (int j = 0; j < ndrives; j++) if (!strcmp(drives[j].obj, drive_obj)) d = &drives[j];
	if (!d) { if (err) snprintf(err, en, "%s", pm_tr("找不到裝置")); return -1; }
	if (!d->external) { if (err) snprintf(err, en, "%s", pm_tr("這不是外接裝置")); return -1; }
	for (int i = 0; i < nvols; i++) {
		if (vols[i].d != d || !vols[i].mount[0]) continue;
		int rc = pm_volume_unmount(vols[i].dev, err, en);
		if (rc) return rc;
	}
	if (!safe_dev(d->dev)) return 0;
	char cmd[256];
	/* 讀卡機之類不能斷電的，卸載完就可以拔了 */
	if (d->can_power_off) {
		snprintf(cmd, sizeof cmd, "udisksctl power-off --no-user-interaction -b %s", d->dev);
		run_cmd(cmd, NULL, 0);
	}
	return 0;
}

/* 格式化整顆外接硬碟：建立新的 MBR 分割表，再建立一個佔滿整顆的分割區並格式化
 * fstype：exfat、vfat、ext4、ntfs */
int pm_drive_format(const char *drive_obj, const char *fstype, const char *label, char *err, size_t en)
{
	pm_disks_scan();
	PmDrive *d = NULL;
	for (int j = 0; j < ndrives; j++) if (!strcmp(drives[j].obj, drive_obj)) d = &drives[j];
	if (!d || !d->external || d->in_use_by_system) {
		if (err) snprintf(err, en, "%s", pm_tr("只能格式化外接裝置"));
		return -1;
	}
	const char *ptype = !strcmp(fstype, "vfat") ? "0x0c" : !strcmp(fstype, "ext4") ? "0x83" : "0x07";
	if (strcmp(fstype, "exfat") && strcmp(fstype, "vfat") && strcmp(fstype, "ext4") && strcmp(fstype, "ntfs")) return -1;
	/* 標籤只留安全字元；FAT 最多 11 字元 */
	char lab[64] = "";
	size_t k = 0;
	for (const char *p = label; *p && k < sizeof lab - 1; p++)
		if (isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == ' ') lab[k++] = *p;
	lab[k] = 0;
	if (!strcmp(fstype, "vfat")) { lab[11] = 0; for (char *p = lab; *p; p++) *p = toupper((unsigned char)*p); }
	for (int i = 0; i < nvols; i++)
		if (vols[i].d == d && vols[i].mount[0] && pm_volume_unmount(vols[i].dev, err, en)) return -1;
	/* 找整顆硬碟的 block 物件 */
	const char *whole = NULL;
	for (int i = 0; i < nvols; i++) if (vols[i].d == d && vols[i].whole) whole = vols[i].obj;
	if (!whole) { if (err) snprintf(err, en, "%s", pm_tr("找不到裝置")); return -1; }
	char cmd[1024];
	snprintf(cmd, sizeof cmd, "busctl --system --timeout=120 call org.freedesktop.UDisks2 '%s' "
	         "org.freedesktop.UDisks2.Block Format 'sa{sv}' dos 1 auth.no_user_interaction b true", whole);
	if (run_cmd(cmd, err, en)) return -1;
	snprintf(cmd, sizeof cmd, "busctl --system --timeout=600 call org.freedesktop.UDisks2 '%s' "
	         "org.freedesktop.UDisks2.PartitionTable CreatePartitionAndFormat 'ttssa{sv}sa{sv}' "
	         "0 0 %s '' 1 auth.no_user_interaction b true %s 2 label s '%s' auth.no_user_interaction b true",
	         whole, ptype, fstype, lab);
	return run_cmd(cmd, err, en) ? -1 : 0;
}
