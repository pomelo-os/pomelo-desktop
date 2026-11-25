/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-check.h — 電腦健康檢查（0.40 起）
 *
 * 八類：網路、安全性、系統與軟體更新、開機與系統穩定、系統服務（伺服器）、電腦變慢、硬體與驅動、電池。每一項檢查都是寫死的程式碼，只讀、不改任何東西；
 * 結果用燈號（正常／提醒／注意／有問題）加一句白話說明與建議。需要修的項目附一個「修正動作」，畫面先問過使用者才執行
 * （需要管理員權限的走 pkexec）。磁碟健康與清理在「儲存空間」，這裡只在磁碟快滿或硬碟故障時提一句、指過去。
 *
 * 前端：網路設定（網路）、活動監視器（電腦變慢）、電源（電池）、系統設定的「健康檢查」（全部）；
 * 命令列 pomelo-system --check [類別] [--json]；小柚的 health_check 工具也是呼叫它。
 *
 * 測試：POMELO_CHECK_ROOT＝假的根目錄（讀 /sys、/proc、/etc、/var 都從這裡讀），外部指令（nmcli、ping…）
 * 用 PATH 前面放假的程式。不會真的連到外面，也不會動到測試主機的設定。 */
#ifndef POMELO_CHECK_H
#define POMELO_CHECK_H
#include <stdio.h>
#include "pomelo-widgets.h"

enum { PM_CHK_OK, PM_CHK_INFO, PM_CHK_WARN, PM_CHK_BAD, PM_CHK_SKIP };   /* 燈號：綠、藍、黃、紅、灰（不適用） */
enum { PM_CHK_NETWORK, PM_CHK_SECURITY, PM_CHK_UPDATES, PM_CHK_STABILITY, PM_CHK_SERVICES, PM_CHK_PERFORMANCE, PM_CHK_HARDWARE, PM_CHK_BATTERY, PM_CHK_NCAT };
#define PM_CHK_ADMIN 1   /* flags：也跑需要管理員權限的項目（會跳出 polkit 密碼視窗） */

typedef struct {
	char id[32];          /* 例如 net.dns（不翻譯：給小柚、測試、記錄用） */
	int level;            /* PM_CHK_OK… */
	char title[160];      /* 一句話的結論（已翻譯） */
	char detail[1024];    /* 找到了什麼（已翻譯，可以多行） */
	char advice[512];     /* 建議怎麼做（已翻譯，可以空的） */
	char fix[32];         /* 修正動作代號，空＝沒有（見 pm_check_fix_argv） */
	char fix_arg[256];    /* 修正動作的參數（例如連線名稱、程序編號、通訊埠） */
	char fix_label[64];   /* 修正按鈕的文字（已翻譯） */
} PmCheck;

typedef struct {
	int cat;
	PmCheck *c;
	int n, cap;
	long ms;              /* 花了多少毫秒 */
	int admin;            /* 有跑需要管理員權限的項目 */
} PmReport;

const char *pm_check_cat_name(int cat);         /* 「網路」…（已翻譯） */
const char *pm_check_cat_id(int cat);           /* network、security、updates、stability、services、performance、hardware、battery */
int pm_check_cat_from_id(const char *id);       /* 不認得回傳 -1 */

/* 同步執行一類檢查（網路與電腦變慢要幾秒；在背景執行緒跑請用 pm_check_start） */
void pm_check_run(int cat, PmReport *r, int flags);
void pm_report_free(PmReport *r);
int pm_report_worst(const PmReport *r);         /* 最嚴重的燈號 */
int pm_report_count(const PmReport *r, int level);
void pm_report_json(const PmReport *r, FILE *f);
void pm_report_text(const PmReport *r, FILE *f);
const char *pm_check_level_name(int level);     /* 「正常」…（已翻譯） */

/* 背景執行：pm_check_start 之後在 tick 裡看 pm_check_done；完成後 pm_check_take 拿走報告（之後由呼叫者 free） */
typedef struct PmCheckJob PmCheckJob;
PmCheckJob *pm_check_start(int cat, int flags);
int pm_check_done(PmCheckJob *j);
PmReport *pm_check_take(PmCheckJob *j);          /* 會釋放 j；報告用 pm_report_free 後再 free() */

/* 修正動作：寫出要執行的指令（argv 指向靜態或 c 裡的字串）、需不需要管理員權限、給確認視窗的說明。
 * 回傳 argv 個數；0＝沒有修正動作或這台電腦做不到。特殊情況：argv[0] 是 "@open" 時是開啟程式或網址（argv[1]）。 */
int pm_check_fix_argv(const PmCheck *c, char **argv, int max, int *admin, char *msg, size_t n);

/* 直接執行修正動作（呼叫者要先問過使用者；pm_ui_check 會在畫面上問）。需要管理員權限時經過 pkexec。
 * 回傳 >= 0：背景工作的 fd，用 pm_check_fix_poll 等它做完；-2：已經做完（例如開了程式或網頁）；-1：做不到。 */
int pm_check_fix_exec(const PmCheck *c);
/* 還在跑回傳 -1；做完回傳結束碼並關掉 fd（pkexec：126＝取消、127＝密碼錯誤） */
int pm_check_fix_poll(int fd);

/* 設定程式用的完整畫面：報告、按修正按鈕後在上方問「要…嗎？」（取消／確定）、執行中的狀態、做完的結果。
 * 狀態由呼叫者保存（PM_CHECK_UI_INIT 初始化）。修正做完回傳 1：呼叫者應該重新檢查。執行中請在 tick 裡 pm_ui_redraw。 */
typedef struct { int pending, fd, last, admin; char label[64]; } PmCheckUI;
#define PM_CHECK_UI_INIT { -1, -1, -1, 0, "" }
int pm_ui_check(PmUI *u, const PmReport *r, PmCheckUI *st);

/* 畫面：畫一份報告（每一項一列：燈號、結論、說明與建議、修正按鈕）。按了修正按鈕回傳那一項的 index，否則 -1。
 * 正常（綠燈）的項目預設收起來，只顯示一行「另外 N 項正常」，點了才展開。 */
int pm_ui_report(PmUI *u, const PmReport *r);
/* 一列狀態（pm_ui_report 用的元件，也可以單獨用）：button 不是 NULL 時右邊有按鈕，按了回傳 1 */
int pm_ui_status(PmUI *u, int level, const char *title, const char *detail, const char *advice, const char *button);
/* 大的狀態卡：左邊彩色圓圈、大標題、一行說明；button 不是 NULL 時右邊有按鈕，按了回傳 1 */
int pm_ui_hero(PmUI *u, int level, const char *title, const char *sub, const char *button);
/* 圖示卡片格：icons 是 pm_ui_add_icon 的名稱，levels（可以是 NULL）決定狀態字的顏色；點了回傳 index，否則 -1 */
int pm_ui_tiles(PmUI *u, const char *const *icons, const char *const *titles, const char *const *subs, const int *levels, int n, int cols);
/* 健康檢查各類的圖示（「@chk-network」…「@chk-battery」，用 pm_check_cat_id 的名稱）；呼叫一次就好 */
void pm_check_icons(void);

/* ---- 套件清單（SBOM）與已知漏洞（sbom.c） ----
 * 套件名、版本、授權、說明，加上跟發行版公開漏洞資料比對的結果。漏洞資料要使用者按「更新」才下載（pm_sbom_update），
 * 整份存在 ~/.cache/pomelo/vuln.tsv、在本機比對：電腦上裝了什麼不會送出去。 */
enum { PM_VULN_SRC_NONE, PM_VULN_SRC_DEBSECAN, PM_VULN_SRC_OVAL, PM_VULN_SRC_OSV, PM_VULN_SRC_DNF };
enum { PM_VULN_FIXABLE, PM_VULN_OPEN, PM_VULN_PRO };      /* 有更新可裝、尚未修補、修正只在 Ubuntu Pro */
enum { PM_SBOM_TEXT, PM_SBOM_CYCLONEDX, PM_SBOM_CSV };

typedef struct {
	char id[32];          /* CVE-2026-1234（Rocky 沒有 CVE 編號時是公告編號） */
	int sev;              /* 0 不明、1 低、2 中、3 高、4 嚴重 */
	int status;           /* PM_VULN_* */
	char fixed[96];       /* 修正版本（空＝還沒有） */
	char desc[240];       /* 一句說明（來源給的，英文） */
} PmVuln;

typedef struct {
	char name[96], version[80], source[96], srcver[80], arch[16], license[128], summary[200];
	PmVuln *v;
	int nv, nfix, nopen, worst;   /* 已知漏洞數、其中有更新可裝、尚未修補（含 Pro）、最嚴重的 sev */
} PmPkg;

typedef struct {
	PmPkg *p;             /* 依名稱排序 */
	int n;
	int kind;             /* PM_PKG_APT／PM_PKG_DNF */
	char os[96];          /* PRETTY_NAME */
	int matched;          /* 1＝已經跟漏洞資料比對過 */
	time_t data_time;     /* 漏洞資料的時間 */
	char data_src[160];   /* 漏洞資料來源（例如「Ubuntu Security OVAL（noble）」） */
	int nvuln_pkgs, nfix_pkgs, nopen_pkgs, ncve;
} PmSbom;

PmSbom *pm_sbom_load(void);                       /* 讀套件清單（約 0.3 秒；deb 要讀每個套件的 copyright） */
void pm_sbom_free(PmSbom *s);
int pm_sbom_match(PmSbom *s);                     /* 跟本機的漏洞資料比對；-1＝還沒有資料 */
int pm_sbom_update(char *err, size_t n);          /* 下載並轉換漏洞資料（同步，要幾秒到一分鐘）；回傳筆數，-1 失敗（err 是原因） */
int pm_sbom_source(char *url, size_t un, char *label, size_t ln, long *size_mb);   /* 這個發行版的資料來源；PM_VULN_SRC_* */
int pm_sbom_data_info(time_t *when, char *src, size_t n);   /* 本機有沒有漏洞資料；1＝有 */
void pm_sbom_export(const PmSbom *s, FILE *f, int fmt);     /* PM_SBOM_* */
const char *pm_vuln_sev_name(int sev);            /* 「低」…（已翻譯） */
const char *pm_vuln_status_name(int status);      /* 「有更新可裝」…（已翻譯） */
int pm_vercmp(int kind, const char *a, const char *b);      /* dpkg／rpm 的版本比較：<0、0、>0 */

#endif
