/* SPDX-License-Identifier: GPL-3.0-or-later */
/* check-hw.c — 硬體與驅動：這台電腦有什麼（CPU、記憶體、主機板與 BIOS、顯示卡）、顯示卡有沒有用上驅動（沒有的話畫面
 * 是軟體畫的）、沒有驅動的裝置、缺少的韌體、找不到音效輸出、USB 錯誤、硬碟健康（SMART，細節在「儲存空間」）。 */
#define _GNU_SOURCE
#include "internal.h"
#include "check-int.h"
#include <ctype.h>
#include <dirent.h>

/* pci.ids 查名稱（有裝 pciutils／hwdata 才有）；查不到就留空 */
static void pci_name(unsigned ven, unsigned dev, char *out, size_t n)
{
	out[0] = 0;
	const char *files[] = { "/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids" };
	for (size_t i = 0; i < LEN(files) && !out[0]; i++) {
		FILE *f = fopen(files[i], "r");
		if (!f) continue;
		char line[512], vname[200] = "";
		int in = 0;
		while (fgets(line, sizeof line, f)) {
			if (line[0] == '#' || line[0] == '\n') continue;
			if (line[0] != '\t') {
				unsigned v;
				in = sscanf(line, "%4x", &v) == 1 && v == ven;
				if (in) { snprintf(vname, sizeof vname, "%s", line + 6); vname[strcspn(vname, "\n")] = 0; }
				else if (vname[0]) break;
				continue;
			}
			unsigned d;
			if (in && line[1] != '\t' && sscanf(line + 1, "%4x", &d) == 1 && d == dev) {
				char *nm = line + 7;
				nm[strcspn(nm, "\n")] = 0;
				snprintf(out, n, "%s %s", vname, nm);
				break;
			}
		}
		if (!out[0] && vname[0]) snprintf(out, n, "%s", vname);
		fclose(f);
	}
}

static unsigned hexv(const char *path)
{
	char b[32];
	return chk_read(path, b, sizeof b) ? (unsigned)strtoul(b, NULL, 16) : 0;
}

static const char *vendor_short(unsigned v)
{
	switch (v) {
	case 0x8086: return "Intel";
	case 0x1002: return "AMD";
	case 0x10de: return "NVIDIA";
	case 0x1af4: return "virtio";
	case 0x1234: return "QEMU";
	case 0x15ad: return "VMware";
	case 0x80ee: return "VirtualBox";
	case 0x1414: return "Microsoft";
	default: return NULL;
	}
}

void chk_hardware(PmReport *r, int flags)
{
	(void)flags;
	PmCheck *c;

	/* 1. 這台電腦有什麼 */
	{
		char cpu[256] = "", mi[256], vendor[128] = "", product[128] = "", bios[128] = "", bdate[64] = "";
		FILE *f = fopen(chk_path("/proc/cpuinfo"), "r");
		char line[512];
		while (f && fgets(line, sizeof line, f) && !cpu[0])
			if (!strncmp(line, "model name", 10) && strchr(line, ':')) { snprintf(cpu, sizeof cpu, "%s", strchr(line, ':') + 2); cpu[strcspn(cpu, "\n")] = 0; }
		if (f) fclose(f);
		long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
		chk_read("/proc/meminfo", mi, sizeof mi);
		double ram = strstr(mi, "MemTotal:") ? atof(strstr(mi, "MemTotal:") + 9) * 1024 : 0;
		chk_read("/sys/class/dmi/id/sys_vendor", vendor, sizeof vendor);
		chk_read("/sys/class/dmi/id/product_name", product, sizeof product);
		chk_read("/sys/class/dmi/id/bios_version", bios, sizeof bios);
		chk_read("/sys/class/dmi/id/bios_date", bdate, sizeof bdate);
		char rs[32];
		pm_human_size(rs, sizeof rs, ram);
		c = chk_add(r, "hw.info", PM_CHK_OK, "%s%s%s", vendor, vendor[0] ? " " : "", product[0] ? product : _("這台電腦"));
		chk_detail(c, _("CPU：%s（%ld 執行緒）"), cpu[0] ? cpu : "?", ncpu);
		chk_detail(c, _("記憶體：%s"), rs);
		if (bios[0]) chk_detail(c, "BIOS：%s %s", bios, bdate);
	}

	/* 2. 顯示卡與驅動、沒有驅動的裝置 */
	{
		char gpus[512] = "", nodrv[1024] = "";
		int ngpu = 0, nno = 0, real_gpu_driver = 0, nvidia_nouveau = 0, vm = 0;
		DIR *d = opendir(chk_path("/sys/bus/pci/devices"));
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (e->d_name[0] == '.') continue;
			char p[400], link[256] = "";
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/class", e->d_name);
			unsigned cls = hexv(p);
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/vendor", e->d_name);
			unsigned ven = hexv(p);
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/device", e->d_name);
			unsigned dev = hexv(p);
			snprintf(p, sizeof p, "/sys/bus/pci/devices/%s/driver", e->d_name);
			ssize_t l = readlink(chk_path(p), link, sizeof link - 1);
			const char *drv = l > 0 ? (link[l] = 0, strrchr(link, '/') + 1) : NULL;
			int base = (int)(cls >> 16), sub = (int)((cls >> 8) & 0xff);
			if (ven == 0x1af4 || ven == 0x1234 || ven == 0x15ad || ven == 0x80ee) vm = 1;
			char name[256];
			if (base == 0x03) {
				ngpu++;
				pci_name(ven, dev, name, sizeof name);
				if (!name[0]) snprintf(name, sizeof name, "%s %04x:%04x", vendor_short(ven) ? vendor_short(ven) : "PCI", ven, dev);
				if (strlen(gpus) < sizeof gpus - 200) snprintf(gpus + strlen(gpus), sizeof gpus - strlen(gpus), "%s%s（%s）", gpus[0] ? "\n" : "", name, drv ? drv : _("沒有驅動"));
				if (drv && strcmp(drv, "bochs-drm") && strcmp(drv, "simpledrm") && strcmp(drv, "efifb") && strcmp(drv, "vesafb")) real_gpu_driver = 1;
				if (ven == 0x10de && drv && !strcmp(drv, "nouveau")) nvidia_nouveau = 1;
			}
			/* 沒有驅動的：網路卡（02）、顯示卡（03）、音效（04 01／03）、無線（0d）、USB 控制器（0c 03） */
			int important = base == 0x02 || base == 0x03 || (base == 0x04 && (sub == 0x01 || sub == 0x03)) || base == 0x0d || (base == 0x0c && sub == 0x03);
			if (important && !drv) {
				nno++;
				pci_name(ven, dev, name, sizeof name);
				if (strlen(nodrv) < sizeof nodrv - 200) snprintf(nodrv + strlen(nodrv), sizeof nodrv - strlen(nodrv), "%s%s [%04x:%04x]", nodrv[0] ? "\n" : "", name[0] ? name : "?", ven, dev);
			}
		}
		if (d) closedir(d);
		char virt[64] = "";
		{ char *o = chk_run("systemd-detect-virt"); if (o) { sscanf(o, "%63s", virt); free(o); } }
		if (virt[0] && strcmp(virt, "none")) vm = 1;
		if (ngpu) {
			if (real_gpu_driver) {
				c = chk_add(r, "hw.gpu", nvidia_nouveau ? PM_CHK_INFO : PM_CHK_OK, "%s", _("顯示卡有用上驅動"));
				chk_detail(c, "%s", gpus);
				if (nvidia_nouveau) chk_advice(c, "%s", pm_pkg_kind() == PM_PKG_DNF ? _("NVIDIA 顯示卡用的是開源驅動；玩遊戲或用 CUDA 的話可以從 RPM Fusion 安裝 NVIDIA 官方驅動。")
				                                                                       : _("NVIDIA 顯示卡用的是開源驅動；玩遊戲或用 CUDA 的話可以安裝 NVIDIA 官方驅動（終端機：sudo ubuntu-drivers install）。"));
			} else if (vm) {
				c = chk_add(r, "hw.gpu", PM_CHK_INFO, _("這是虛擬機器（%s），畫面用軟體畫"), virt[0] ? virt : "?");
				chk_detail(c, "%s", gpus);
			} else {
				c = chk_add(r, "hw.gpu", PM_CHK_WARN, "%s", _("顯示卡沒有用上驅動，畫面是用 CPU 畫的"));
				chk_detail(c, "%s", gpus);
				chk_detail(c, "%s", _("畫面會比較慢、比較耗電，影片也可能不順。"));
				chk_advice(c, "%s", _("安裝顯示卡的驅動或韌體（見下面缺少韌體那一項）；新的顯示卡可能要更新的核心才認得。"));
			}
		}
		if (nno) {
			c = chk_add(r, "hw.nodriver", PM_CHK_WARN, _("有 %d 個裝置沒有驅動"), nno);
			chk_detail(c, "%s", nodrv);
			chk_advice(c, "%s", _("用方括號裡的編號上網搜尋「Linux 驅動」；Wi-Fi 網卡常常只是缺韌體。"));
		} else chk_add(r, "hw.nodriver", PM_CHK_OK, "%s", _("重要的裝置都有驅動"));
	}

	/* 3. kernel 訊息：缺少的韌體、USB 錯誤 */
	{
		char *o = chk_kernel_log(0);
		if (o) {
			char fw[1024] = "";
			int nfw = 0, usb = 0;
			for (char *l = o, *nx; *l; l = nx) {
				nx = chk_line(l);
				char *p = strstr(l, "Direct firmware load for ");
				if (p) {
					p += 25;
					char name[160];
					snprintf(name, sizeof name, "%.*s", (int)strcspn(p, " "), p);
					if (!strstr(fw, name)) { nfw++; if (strlen(fw) < sizeof fw - 200) snprintf(fw + strlen(fw), sizeof fw - strlen(fw), "%s%s", fw[0] ? "\n" : "", name); }
				}
				if (strstr(l, "over-current") || strstr(l, "device descriptor read") || strstr(l, "unable to enumerate USB device")) usb++;
			}
			free(o);
			if (nfw) {
				c = chk_add(r, "hw.firmware", PM_CHK_WARN, _("缺少 %d 個韌體檔"), nfw);
				chk_detail(c, "%s", fw);
				chk_advice(c, "%s", pm_os_like("debian") && !pm_os_like("ubuntu") ? _("安裝 firmware-linux-nonfree（要先在套件來源加上 non-free-firmware）。")
				                     : pm_pkg_kind() == PM_PKG_DNF ? _("安裝或更新 linux-firmware 套件（sudo dnf install linux-firmware）。")
				                                                   : _("更新 linux-firmware 套件（sudo apt install linux-firmware）。"));
			}
			if (usb >= 3) {
				c = chk_add(r, "hw.usb", PM_CHK_WARN, "%s", _("USB 裝置常常連線失敗"));
				chk_detail(c, _("本次開機以來出現 %d 次 USB 錯誤。"), usb);
				chk_advice(c, "%s", _("換一個 USB 孔或換一條線；耗電大的裝置（外接硬碟）可以改用有外接電源的 USB 集線器。"));
			}
		}
	}

	/* 4. 音效 */
	{
		char cards[1024];
		int have = chk_read("/proc/asound/cards", cards, sizeof cards) && !strstr(cards, "no soundcards");
		if (!have) {
			c = chk_add(r, "hw.audio", PM_CHK_WARN, "%s", _("找不到音效卡"));
			chk_advice(c, "%s", _("沒有喇叭或耳機輸出；可能是缺驅動或韌體（見上面），虛擬機器則要在設定裡加音效裝置。"));
		} else if (pm_have("pactl")) {
			char *o = chk_run("pactl list short sinks");
			int real = 0, any = o && *o;   /* 什麼都沒印：連不上聲音系統（例如從別的工作階段執行），判斷不了 */
			for (char *l = o, *nx; l && *l; l = nx) { nx = chk_line(l); if (*l && !strstr(l, "auto_null") && !strstr(l, "dummy")) real = 1; }
			free(o);
			if (any && !real) {
				c = chk_add(r, "hw.audio", PM_CHK_WARN, "%s", _("有音效卡，但聲音系統找不到輸出"));
				chk_advice(c, "%s", _("登出再登入一次；還是不行的話，在音效設定看看輸出裝置。"));
				chk_fix(c, "hw.sound", NULL, _("開啟音效設定"));
			} else chk_add(r, "hw.audio", PM_CHK_OK, "%s", _("有聲音輸出"));
		} else chk_add(r, "hw.audio", PM_CHK_OK, "%s", _("有音效卡"));
	}

	/* 5. 硬碟健康（SMART；細節與清理在「儲存空間」） */
	if (!pm_disks_scan()) {
		int nd = 0, nbad = 0;
		PmDrive *dr = pm_drives(&nd);
		char bad[512] = "";
		for (int i = 0; i < nd; i++) {
			if (dr[i].external) continue;
			if (dr[i].failing || dr[i].attrs_failing || dr[i].bad_sectors > 0) {
				nbad++;
				snprintf(bad + strlen(bad), sizeof bad - strlen(bad), "%s%s %s", bad[0] ? "\n" : "", dr[i].model, dr[i].dev);
			}
		}
		if (nbad) {
			c = chk_add(r, "hw.disk", PM_CHK_BAD, _("%d 顆硬碟的健康狀態有警告"), nbad);
			chk_detail(c, "%s", bad);
			chk_advice(c, "%s", _("請盡快備份重要的檔案，考慮換硬碟。"));
			chk_fix(c, "hw.storage", NULL, _("開啟儲存空間"));
		} else if (nd) chk_add(r, "hw.disk", PM_CHK_OK, "%s", _("硬碟健康狀態正常"));
	}
}

int chk_hw_fix(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n)
{
	(void)max; (void)admin; (void)msg; (void)n;
	int k = 0;
	if (!strcmp(c->fix, "hw.sound")) { argv[k++] = "@open"; argv[k++] = "pomelo-sound"; }
	else if (!strcmp(c->fix, "hw.storage")) { argv[k++] = "@open"; argv[k++] = "pomelo-storage"; }
	argv[k] = NULL;
	return k;
}
