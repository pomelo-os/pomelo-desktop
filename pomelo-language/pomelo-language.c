/* SPDX-License-Identifier: GPL-3.0-or-later */
/* pomelo-language — Pomelo 語言與輸入法
 *
 *  - 介面語言、地區格式：寫入 settings.conf（region.lang / region.formats），重新登入後由 pomelo-session 套用；
 *    系統還沒有的語言，會開終端機產生（Ubuntu：sudo locale-gen；Fedora、Rocky：裝 glibc-langpack-<語言>）
 *  - 鍵盤配置：立即用 setxkbmap 套用，並同步到 fcitx5
 *  - 輸入法：選擇 fcitx5 / ibus（input.framework），啟用或停用個別輸入法；沒安裝的提供安裝按鈕
 */
#define _GNU_SOURCE
#include <pomelo.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ------------------------------------------------------------------ */
/* 語言                                                                */
/* ------------------------------------------------------------------ */

typedef struct { const char *code, *name; } LangName;
static const LangName lang_names[] = {
	{ "zh_TW", "繁體中文（台灣）" }, { "zh_HK", "繁體中文（香港）" }, { "zh_CN", "简体中文（中国）" },
	{ "zh_SG", "简体中文（新加坡）" }, { "en_US", "English (US)" }, { "en_GB", "English (UK)" },
	{ "C", "English" }, { "ja_JP", "日本語" }, { "ko_KR", "한국어" }, { "fr_FR", "Français" },
	{ "de_DE", "Deutsch" }, { "es_ES", "Español" }, { "it_IT", "Italiano" }, { "pt_BR", "Português (Brasil)" },
	{ "ru_RU", "Русский" }, { "vi_VN", "Tiếng Việt" }, { "th_TH", "ไทย" }, { "id_ID", "Bahasa Indonesia" },
};

typedef struct { char code[32]; const char *name; int installed; } Lang;
static Lang langs[64];
static int nlangs;
static const char *lang_opts[64];
static int lang_sel, fmt_sel, need_relogin;

static const char *name_of(const char *code)
{
	for (unsigned i = 0; i < LEN(lang_names); i++)
		if (!strcmp(lang_names[i].code, code)) return lang_names[i].name;
	return NULL;
}

/* 把 zh_TW.utf8 / zh_TW.UTF-8 統一成 zh_TW */
static void base_code(const char *loc, char *out, size_t n)
{
	snprintf(out, n, "%s", loc);
	out[strcspn(out, ".@")] = 0;
}

static void scan_langs(void)
{
	nlangs = 0;
	char *t = pm_run("locale -a");
	char *save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l && nlangs < (int)LEN(langs); l = strtok_r(NULL, "\n", &save)) {
		if (!strcasestr(l, ".utf8") && !strcasestr(l, ".utf-8")) continue;
		char code[32];
		base_code(l, code, sizeof code);
		const char *nm = name_of(code);
		if (!nm) continue;   /* 只列出常見的語言 */
		snprintf(langs[nlangs].code, sizeof langs[0].code, "%s", code);
		langs[nlangs].name = nm;
		langs[nlangs].installed = 1;
		nlangs++;
	}
	free(t);
	/* 還沒產生的常見語言也列出來 */
	for (unsigned i = 0; i < LEN(lang_names) && nlangs < (int)LEN(langs); i++) {
		int have = 0;
		for (int k = 0; k < nlangs; k++) if (!strcmp(langs[k].code, lang_names[i].code)) have = 1;
		if (have || !strcmp(lang_names[i].code, "C")) continue;
		snprintf(langs[nlangs].code, sizeof langs[0].code, "%s", lang_names[i].code);
		langs[nlangs].name = lang_names[i].name;
		langs[nlangs].installed = 0;
		nlangs++;
	}
	static char labels[64][96];
	for (int i = 0; i < nlangs; i++) {
		snprintf(labels[i], sizeof labels[i], "%s%s", langs[i].name, langs[i].installed ? "" : _("（未安裝）"));
		lang_opts[i] = labels[i];
	}
	/* 目前的設定 */
	char cur[32], curf[32];
	const char *env = getenv("LANG");
	base_code(pm_conf_get("region.lang", env ? env : "zh_TW"), cur, sizeof cur);
	base_code(pm_conf_get("region.formats", pm_conf_get("region.lang", env ? env : "zh_TW")), curf, sizeof curf);
	lang_sel = fmt_sel = 0;
	for (int i = 0; i < nlangs; i++) {
		if (!strcmp(langs[i].code, cur)) lang_sel = i;
		if (!strcmp(langs[i].code, curf)) fmt_sel = i;
	}
}

static void locale_value(const Lang *l, char *out, size_t n)
{
	snprintf(out, n, "%s.UTF-8", l->code);
}

/* 在終端機裡執行需要 sudo 的指令，讓使用者輸入密碼 */
static void run_in_terminal(const char *cmd)
{
	char full[1024];
	snprintf(full, sizeof full, "%s; echo; echo '%s'; read x", cmd, _("完成，按 Enter 關閉這個視窗"));
	if (fork() == 0) {
		if (dpy) close(ConnectionNumber(dpy));   /* 原生 Wayland（pm_ui_native）時沒有 X 連線 */
		setsid();
		execlp("pomelo-terminal", "pomelo-terminal", "-t", _("安裝"), "-e", "sh", "-c", full, (char *)NULL);
		execlp("x-terminal-emulator", "x-terminal-emulator", "-e", "sh", "-c", full, (char *)NULL);
		_exit(127);
	}
}

/* ------------------------------------------------------------------ */
/* 鍵盤配置                                                            */
/* ------------------------------------------------------------------ */

static const char *layouts[] = { "us", "gb", "jp", "kr", "de", "fr", "es", "it" };
static const char *layout_names[] = {
	N_("美式英文"), N_("英式英文"), N_("日文"), N_("韓文"), N_("德文"), N_("法文"), N_("西班牙文"), N_("義大利文"),
};
static int layout_sel;

/* ------------------------------------------------------------------ */
/* 輸入法                                                              */
/* ------------------------------------------------------------------ */

static const char *fw_keys[] = { "auto", "pomelo", "fcitx5", "ibus", "none" };
static const char *fw_names[] = { N_("自動"), N_("Pomelo 輸入法（內建）"), "Fcitx 5", "IBus", N_("不使用") };
static int fw_sel;

typedef struct { char id[64], name[96], icon[64]; int enabled; } IM;
static IM ims[64];
static int nims;

/* 建議安裝的輸入法套件 */
typedef struct { const char *fcitx_pkg, *ibus_pkg, *name, *desc; } Suggest;
static const Suggest suggests[] = {
	{ "fcitx5-chewing", "ibus-chewing", N_("新酷音（注音）"), N_("繁體中文注音輸入") },
	{ "fcitx5-table-extra", "ibus-table-extraphrase", N_("行列、大易、嘸蝦米等字根"), N_("繁體中文字根輸入") },
	{ "fcitx5-chinese-addons", "ibus-libpinyin", N_("拼音、倉頡、速成"), N_("中文拼音與倉頡") },
	{ "fcitx5-rime", "ibus-rime", N_("中州韻（Rime）"), N_("可自訂的中文輸入法") },
	{ "fcitx5-mozc", "ibus-mozc", N_("日文（Mozc）"), N_("日文輸入") },
	{ "fcitx5-hangul", "ibus-hangul", N_("韓文"), N_("韓文輸入") },
	/* Qt 程式的輸入法前端（IBus 由 Qt 內建支援） */
	{ "fcitx5-frontend-qt5", "ibus", N_("在 Qt 程式裡打字"), N_("例如 VLC、VirtualBox") },
};

static int pkg_installed(const char *pkg) { return pm_pkg_installed(pkg); }

/* 真的 ibus-daemon：pomelo-input 給 Qt 用的佔位檔（.../pomelo-input/qt-ibus/ibus-daemon）不算 */
static int have_ibus(void)
{
	char buf[4096];
	snprintf(buf, sizeof buf, "%s", getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
	for (char *save = NULL, *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
		if (strstr(d, "pomelo-input/qt-ibus")) continue;
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/ibus-daemon", d);
		if (!access(p, X_OK)) return 1;
	}
	return 0;
}

static void install_in_terminal(const char *debs);

/* ---- Pomelo 輸入法（pomelo-input）的引擎：跟 pomelo-input/engine.h 對應 ---- */
/* lib：dlopen 的函式庫（「a|b」＝哪個有都可以）；pkgs：Debian 的套件名稱，「a|b」＝這個系統有哪個裝哪個；
 * rime：要下載的 Rime 方案資料（下面 rime_files 的組別）、schema：下載好了沒看這個方案檔 */
typedef struct { const char *id, *name, *desc, *lib, *pkgs, *rime, *schema; } Eng;
#define RIME_LIB "librime.so.1", "librime1t64|librime1"
static const Eng engs[] = {
	{ "chewing", N_("注音"), N_("新酷音引擎，內建"), NULL, NULL, NULL, NULL },
	{ "rime-pinyin", N_("拼音（簡體）"), N_("全拼，打簡體字（Rime）"), RIME_LIB, "base", "luna_pinyin_simp" },
	{ "rime-cangjie", N_("倉頡"), N_("倉頡第五代，香港常用（Rime）"), RIME_LIB, "base cangjie", "cangjie5" },
	{ "rime-quick", N_("速成"), N_("簡易：只打倉頡碼的頭尾（Rime）"), RIME_LIB, "base cangjie quick", "quick5" },
	{ "rime-jyutping", N_("粵拼"), N_("用粵語拼音打繁體字（Rime）"), RIME_LIB, "base cangjie jyutping", "jyut6ping3" },
	{ "anthy", N_("日文"), N_("羅馬字輸入，轉換成漢字（Anthy）"), "libanthy.so.1|libanthy-unicode.so.0", "libanthy1t64|libanthy1", NULL, NULL },
	{ "hangul", N_("韓文"), N_("兩排式、三排式（libhangul）"), "libhangul.so.1", "libhangul1", NULL, NULL },
	{ "viet", N_("越南文"), N_("Telex 或 VNI，內建"), NULL, NULL, NULL, NULL },
};

/* ---- Rime 方案資料：Fedora 的 librime 沒附，各發行版一律從 GitHub 下載固定版本，檢查大小與 SHA256。
 * 放在 $XDG_DATA_HOME/pomelo/rime-data（pomelo-input 的 eng-rime.c 從這裡讀）。
 * 授權：方案與字典 LGPL-3.0（rime-cantonese 的字典 CC BY 4.0），見使用手冊的第三方元件 ---- */
typedef struct { const char *grp, *path, *file; long long size; const char *sha; } RimeFile;
#define R_PRELUDE "rime/rime-prelude/082425ea0684bca36474415d4a0e8db9b016487e/"
#define R_ESSAY "rime/rime-essay/054920de4f54c9e5994276a96a4fc2a35cb51aa3/"
#define R_LUNA_PINYIN "rime/rime-luna-pinyin/56b934b099dfbeab842320f13aa8b461a6ab3e42/"
#define R_STROKE "rime/rime-stroke/1e8fff9b9494ddec23b0cbc526bcfd8171a6fd48/"
#define R_CANGJIE "rime/rime-cangjie/52d90a1b1312e74042b38c1cbc8142defbc53171/"
#define R_QUICK "rime/rime-quick/5dcdb9e353d314239e9c8cddc0f42d52da4837bb/"
#define R_CANTONESE "rime/rime-cantonese/259f0e48bba840c3a2e0d117539e96937f3d89bc/"
static const RimeFile rime_files[] = {
	{ "base", R_PRELUDE, "default.yaml", 1593, "0628ada16651d56c4cdfcb8e45ddba23d4d585cc8be0580a62c4e077c0bcc141" },
	{ "base", R_PRELUDE, "key_bindings.yaml", 3117, "ee2476598b4cae89068b823868196eb07b5f72007a600814fe5804b39008b3ed" },
	{ "base", R_PRELUDE, "punctuation.yaml", 2107, "d6f89592149098f0a2d0ed3e5c83f49adb6226cdf1d967b0bc0d293f4e5c496a" },
	{ "base", R_PRELUDE, "symbols.yaml", 28473, "6d08969525bd66c67abf1791c99928079356bd0e673386c49c1959d2a52fdbba" },
	{ "base", R_ESSAY, "essay.txt", 5887319, "3e8512ebaa6657e35961b1f5762060717c647e3bf471d5e3b86e783b4e934ffb" },
	{ "base", R_LUNA_PINYIN, "luna_pinyin.dict.yaml", 889896, "75bcf6eb3ff62b129882ed89cc22b2d80a5347aa72bcfa2ccc839bac298e7314" },
	{ "base", R_LUNA_PINYIN, "luna_pinyin.schema.yaml", 2809, "668b4d4957e4cc8f9cea32ed3f08f0ecf4a7be8cdba058700507487bd96dd5c7" },
	{ "base", R_LUNA_PINYIN, "luna_pinyin_simp.schema.yaml", 420, "403cf4c6abffe5fc6c1cec072b0142304ccd9ff032fef0cfffcc020ef2eb06ee" },
	{ "base", R_LUNA_PINYIN, "luna_quanpin.schema.yaml", 398, "1d2bc8f0a62179f149f6449185ed787f761e8f53c304e5e11a78461b9a8d7999" },
	{ "base", R_LUNA_PINYIN, "pinyin.yaml", 4895, "caa220b05172bf35775db07fed6ead7719c742e6139232d9797040dbe65f746c" },
	{ "base", R_STROKE, "stroke.dict.yaml", 3396330, "b3e93dce89c185f45c3d6e189b86b3a8626913352cc85e1094c786579a665791" },
	{ "base", R_STROKE, "stroke.schema.yaml", 3056, "06f680cf7c9d9a69948681ee25e9f6aeccfde4d8d590f93e179d4c4b3caee879" },
	{ "cangjie", R_CANGJIE, "cangjie5.schema.yaml", 2453, "93b3e81787447b9002885b38fb4024d36e4135741e9e765bd847268efb81aac1" },
	{ "cangjie", R_CANGJIE, "cangjie5.dict.yaml", 984, "1514afcfb56dc347db596a6602c8dfefee798fa504e35d10eaa1e11536648d3a" },
	{ "cangjie", R_CANGJIE, "cangjie5.base.dict.yaml", 219207, "8690f2ad8aafd38780846881aa916b5779e6d9247a351a1da426d3f3257afca4" },
	{ "cangjie", R_CANGJIE, "cangjie5.extended.dict.yaml", 564497, "a6a0b06427c599d9fa7fc1e02a618bf234eb1dc9fc9b6b92f3292fc02213b3cd" },
	{ "cangjie", R_CANGJIE, "cangjie5.stem.dict.yaml", 13399, "a15208351dbc33d5ade8d16ece75907ef2a7310e3962f4e54003dab9bbd49b8a" },
	{ "quick", R_QUICK, "quick5.schema.yaml", 2215, "0eed4b8b10cd50132a691c8027cb774f481eb571c9d5c0b901d54bc7c5867c53" },
	{ "quick", R_QUICK, "quick5.dict.yaml", 210, "47dee37657c75afce60b3c65e650dbcbd4df39e4a7aecba7153ab56649632cd0" },
	{ "quick", R_QUICK, "quick5.supplement.dict.yaml", 63512, "c7e38622084f4be1c64a240f64e41ec5571da22563e60ea14028945e3f76a4b7" },
	{ "jyutping", R_CANTONESE, "jyut6ping3.schema.yaml", 6734, "cd872bd8cc69ee9ee8b585080d5d1d99602cf44470eaf69efc2bcff63fe488b5" },
	{ "jyutping", R_CANTONESE, "jyut6ping3.dict.yaml", 344, "cfc8cda8175206bdada5fc9fc7f4fb492e59b7b9a3536a9b6c099a3f8ee03bbe" },
	{ "jyutping", R_CANTONESE, "jyut6ping3.chars.dict.yaml", 345329, "9b053a594c80eae76545bcdd83239884a49b1ba48b89ca0ab9e8daac79f33013" },
	{ "jyutping", R_CANTONESE, "jyut6ping3.words.dict.yaml", 2630257, "54d174ad2bb997e4a678b7b076b84e4dc5914481b32467cdea3b43e88b7d5474" },
	{ "jyutping", R_CANTONESE, "jyut6ping3.phrase.dict.yaml", 4958423, "3071d1ca43d6861aefcaa14e1b72780b21537b4e58b86ffce27ae0577c94e2ed" },
	{ "jyutping", R_CANTONESE, "jyut6ping3.lettered.dict.yaml", 23565, "61ba23bb0e5f6add9f9f4a2f8efa539c351dc4f086a3134283cf83688040d839" },
	{ "jyutping", R_CANTONESE, "jyut6ping3.maps.dict.yaml", 51820, "a28716c771d6adaccbb599268a92ddace085bbd6edea18e61f2af63b1a2e5c87" },
	{ "jyutping", R_CANTONESE, "essay-cantonese.txt", 3416812, "d08836175f598219f43c2f2f9e12e12711212dbefe08d57b2eddb7a9d9f22a5d" },
	{ "jyutping", R_CANTONESE, "symbols_cantonese.yaml", 18590, "06957788ef0a20859ad38aff7fc823670094907436a3851daea939e3ec5df766" },
};

static void rime_dir(char *out, size_t n)
{
	const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	if (x && *x) snprintf(out, n, "%s/pomelo/rime-data", x);
	else snprintf(out, n, "%s/.local/share/pomelo/rime-data", h ? h : "");
}

static long long file_size(const char *p)
{
	struct stat st;
	return stat(p, &st) ? -1 : (long long)st.st_size;
}

/* groups 是空白分隔的組別，這個檔案要不要 */
static int in_groups(const char *groups, const char *g)
{
	size_t n = strlen(g);
	for (const char *p = groups; (p = strstr(p, g)); p += n)
		if ((p == groups || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return 1;
	return 0;
}

/* 還要下載多少（位元組；0 = 都有了） */
static long long rime_missing(const char *groups)
{
	char dir[PATH_MAX], p[PATH_MAX + 64];
	rime_dir(dir, sizeof dir);
	long long need = 0;
	for (unsigned i = 0; i < LEN(rime_files); i++) {
		const RimeFile *f = &rime_files[i];
		if (!in_groups(groups, f->grp)) continue;
		snprintf(p, sizeof p, "%s/%s", dir, f->file);
		if (file_size(p) != f->size) need += f->size;
	}
	return need;
}

static volatile int dl_busy;
static const Eng *volatile dl_eng, *volatile dl_finished;   /* 下載中的、剛下載好（主執行緒幫它啟用）的 */
static volatile long long dl_total, dl_done;
static char dl_part[PATH_MAX + 80], dl_err[256];

/* 執行程式（不經過 shell）並等它結束；out 收標準輸出。
 * pm_ui_run 讓子行程自動回收（SIGCHLD 忽略），拿不到結束碼：成功與否由呼叫者看結果的檔案 */
static void run_argv(char *const argv[], char *out, size_t n)
{
	int fds[2];
	if (out && n) out[0] = 0;
	if (pipe(fds)) return;
	pid_t pid = fork();
	if (pid < 0) { close(fds[0]); close(fds[1]); return; }
	if (pid == 0) {
		dup2(fds[1], 1);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 2); }
		close(fds[0]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(fds[1]);
	size_t k = 0;
	ssize_t r;
	char junk[512];
	if (out && n) {
		while (k < n - 1 && (r = read(fds[0], out + k, n - 1 - k)) > 0) k += (size_t)r;
		out[k] = 0;
	}
	while (read(fds[0], junk, sizeof junk) > 0) ;   /* 讀到 EOF：程式結束了 */
	close(fds[0]);
	waitpid(pid, NULL, 0);
}

static void *rime_download(void *arg)
{
	const Eng *e = arg;
	char dir[PATH_MAX], dest[PATH_MAX + 64], url[512], sum[128];
	rime_dir(dir, sizeof dir);
	char parent[PATH_MAX];
	snprintf(parent, sizeof parent, "%s", dir);
	*strrchr(parent, '/') = 0;
	mkdir(parent, 0755);
	mkdir(dir, 0755);
	dl_err[0] = 0;
	for (unsigned i = 0; i < LEN(rime_files) && !dl_err[0]; i++) {
		const RimeFile *f = &rime_files[i];
		if (!in_groups(e->rime, f->grp)) continue;
		snprintf(dest, sizeof dest, "%s/%s", dir, f->file);
		if (file_size(dest) == f->size) continue;
		snprintf(dl_part, sizeof dl_part, "%s.part", dest);
		snprintf(url, sizeof url, "https://raw.githubusercontent.com/%s%s", f->path, f->file);
		unlink(dl_part);
		char *a[] = { "curl", "-fsSL", "--retry", "3", "-o", dl_part, url, NULL };
		run_argv(a, NULL, 0);
		if (file_size(dl_part) <= 0) { snprintf(dl_err, sizeof dl_err, "%s", _("下載失敗，請檢查網路後再試一次。")); break; }
		char *h[] = { "sha256sum", dl_part, NULL };
		run_argv(h, sum, sizeof sum);
		if (file_size(dl_part) != f->size || strncmp(sum, f->sha, 64) || rename(dl_part, dest)) {
			snprintf(dl_err, sizeof dl_err, "%s", _("下載的檔案驗證失敗（可能不完整或被竄改），已刪除，請再試一次。"));
			break;
		}
		dl_done += f->size;
	}
	unlink(dl_part);
	if (!dl_err[0]) dl_finished = e;
	dl_eng = NULL;
	dl_busy = 0;
	return NULL;
}

static void rime_start_download(const Eng *e)
{
	if (dl_busy) return;
	dl_err[0] = 0;
	if (!pm_have("curl")) { snprintf(dl_err, sizeof dl_err, "%s", _("要先安裝 curl 才能下載")); return; }
	dl_total = rime_missing(e->rime);
	dl_done = 0;
	dl_part[0] = 0;
	dl_eng = e;
	dl_busy = 1;
	pthread_t t;
	if (pthread_create(&t, NULL, rime_download, (void *)e)) { dl_busy = 0; dl_eng = NULL; return; }
	pthread_detach(t);
}

/* 引擎的狀態 */
enum { E_READY, E_NEED_PKG, E_NO_PKG, E_NEED_DATA };
static int lib_gen = 1;   /* tick 每 3 秒加一：沒找到的函式庫再試一次（可能剛裝好） */
static int eng_state(const Eng *e)
{
	/* 函式庫：找到過就記住（dlopen librime 會載入一大串相依） */
	static int found[LEN(engs)], tried[LEN(engs)];
	int i = (int)(e - engs);
	if (e->lib && !found[i] && tried[i] != lib_gen) {
		tried[i] = lib_gen;
		char libs[128];
		snprintf(libs, sizeof libs, "%s", e->lib);
		for (char *save = NULL, *t = strtok_r(libs, "|", &save); t && !found[i]; t = strtok_r(NULL, "|", &save)) {
			void *h = dlopen(t, RTLD_LAZY | RTLD_LOCAL);
			if (h) { found[i] = 1; dlclose(h); }
		}
	}
	if (e->lib && !found[i]) {
		char first[64];
		snprintf(first, sizeof first, "%.*s", (int)strcspn(e->pkgs, "| "), e->pkgs);
		return pm_pkg_name(first) ? E_NEED_PKG : E_NO_PKG;
	}
	if (e->rime && rime_missing(e->rime) > 0) return E_NEED_DATA;
	return E_READY;
}

static const Eng *eng_by_id(const char *id)
{
	for (unsigned i = 0; i < LEN(engs); i++) if (!strcmp(engs[i].id, id)) return &engs[i];
	return NULL;
}

/* 介面語言預設啟用的（跟 pomelo-input 的 default_engines 一樣） */
static const char *default_engines(void)
{
	const char *l = pm_lang();
	if (!strncmp(l, "zh_CN", 5) || !strncmp(l, "zh_SG", 5)) return "rime-pinyin";
	if (!strncmp(l, "zh_HK", 5) || !strncmp(l, "zh_MO", 5)) return "rime-cangjie,rime-quick";
	if (!strncmp(l, "zh", 2)) return "chewing";
	if (!strncmp(l, "ja", 2)) return "anthy";
	if (!strncmp(l, "ko", 2)) return "hangul";
	if (!strncmp(l, "vi", 2)) return "viet";
	return "";
}

static int eng_enabled(const char *id)
{
	char list[512];
	snprintf(list, sizeof list, ",%s,", pm_conf_get("input.engines", default_engines()));
	for (char *c = list; *c; c++) if (*c == ' ') *c = ',';
	char k[80];
	snprintf(k, sizeof k, ",%s,", id);
	return strstr(list, k) != NULL;
}

static void eng_toggle(const char *id)
{
	char list[512], out[512] = "";
	snprintf(list, sizeof list, "%s", pm_conf_get("input.engines", default_engines()));
	int was = eng_enabled(id);
	for (char *save = NULL, *t = strtok_r(list, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
		if (was && !strcmp(t, id)) continue;
		size_t o = strlen(out);
		snprintf(out + o, sizeof out - o, "%s%s", o ? "," : "", t);
	}
	if (!was) { size_t o = strlen(out); snprintf(out + o, sizeof out - o, "%s%s", o ? "," : "", id); }
	pm_conf_set("input.engines", out);
}

/* 「a|b」：apt 的系統上挑這個版本的 Ubuntu 有的那一個（例如 librime1t64 或 librime1） */
static void eng_install(const Eng *e)
{
	char list[256] = "", pk[256];
	snprintf(pk, sizeof pk, "%s", e->pkgs);
	for (char *save = NULL, *t = strtok_r(pk, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
		char *pick = t, *bar;
		while ((bar = strchr(pick, '|')) && pm_pkg_kind() == PM_PKG_APT) {
			*bar = 0;
			char cmd[160];
			snprintf(cmd, sizeof cmd, "apt-cache show %s >/dev/null 2>&1 && echo yes", pick);
			char *r = pm_run(cmd);
			int ok = r && !strncmp(r, "yes", 3);
			free(r);
			if (ok) break;
			pick = bar + 1;
		}
		if ((bar = strchr(pick, '|'))) *bar = 0;
		size_t o = strlen(list);
		snprintf(list + o, sizeof list - o, "%s%s", o ? " " : "", pick);
	}
	install_in_terminal(list);
}

/* 在終端機裝套件（apt 或 dnf；這個系統沒有的套件就說明一下） */
static void install_in_terminal(const char *debs)
{
	char *cmd = pm_pkg_cmdline("install-min", debs);
	if (cmd) run_in_terminal(cmd);
	else {
		char m[300];
		snprintf(m, sizeof m, "echo '%s'; read x", _("這個系統的套件庫沒有這個輸入法"));
		run_in_terminal(m);
	}
	free(cmd);
}

/* 系統還沒有的語言：Ubuntu 用 locale-gen 產生，Fedora／RHEL 系裝 glibc-langpack-<語言> */
static void add_locale(const char *code)
{
	char buf[160];
	if (pm_pkg_kind() == PM_PKG_DNF) snprintf(buf, sizeof buf, "sudo dnf install -y glibc-langpack-%.*s", (int)strcspn(code, "_"), code);
	else snprintf(buf, sizeof buf, "sudo locale-gen %s.UTF-8", code);
	run_in_terminal(buf);
}

static int running(const char *comm)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int pid = 0;
	while (d && (e = readdir(d)) && !pid) {
		if (!isdigit((unsigned char)e->d_name[0])) continue;
		char p[64], c[32] = "";
		snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		if (fgets(c, sizeof c, f)) c[strcspn(c, "\n")] = 0;
		fclose(f);
		struct stat st;
		snprintf(p, sizeof p, "/proc/%s", e->d_name);
		if (!strcmp(c, comm) && !stat(p, &st) && st.st_uid == getuid()) pid = atoi(e->d_name);
	}
	if (d) closedir(d);
	return pid;
}

/* 實際使用的框架（「自動」時看有裝哪個） */
static const char *active_fw(void)
{
	const char *k = fw_keys[fw_sel];
	if (strcmp(k, "auto")) return k;
	return pm_have("pomelo-input") ? "pomelo" : pm_have("fcitx5") ? "fcitx5" : have_ibus() ? "ibus" : "none";
}

static void fcitx_profile_path(char *out, size_t n)
{
	const char *cfg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (cfg && *cfg) snprintf(out, n, "%s/fcitx5/profile", cfg);
	else snprintf(out, n, "%s/.config/fcitx5/profile", home ? home : "");
}

static void scan_fcitx(void)
{
	nims = 0;
	/* 已啟用：profile 第一個群組裡的項目 */
	char enabled[32][64];
	int nen = 0;
	char path[PATH_MAX], line[512];
	fcitx_profile_path(path, sizeof path);
	FILE *f = fopen(path, "r");
	int in_items = 0;
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') { in_items = !strncmp(line, "[Groups/0/Items/", 16); continue; }
		if (in_items && !strncmp(line, "Name=", 5) && nen < 32 && strncmp(line + 5, "keyboard-", 9))
			snprintf(enabled[nen++], 64, "%s", line + 5);
	}
	if (f) fclose(f);
	/* 可用的輸入法：fcitx5 的 inputmethod 資料夾裡每個 .conf 一個 */
	const char *loc = setlocale(LC_MESSAGES, NULL);
	char kfull[48], kshort[48], lfull[16];
	snprintf(lfull, sizeof lfull, "%s", loc ? loc : "C");
	lfull[strcspn(lfull, ".@")] = 0;
	snprintf(kfull, sizeof kfull, "Name[%s]=", lfull);
	snprintf(kshort, sizeof kshort, "Name[%.2s]=", lfull);
	DIR *d = opendir("/usr/share/fcitx5/inputmethod");
	struct dirent *e;
	while (d && (e = readdir(d)) && nims < (int)LEN(ims)) {
		size_t l = strlen(e->d_name);
		if (l < 6 || strcmp(e->d_name + l - 5, ".conf")) continue;
		IM *im = &ims[nims];
		memset(im, 0, sizeof *im);
		snprintf(im->id, sizeof im->id, "%.*s", (int)(l - 5), e->d_name);
		char p[PATH_MAX + 300], name[96] = "", lname[96] = "";
		snprintf(p, sizeof p, "/usr/share/fcitx5/inputmethod/%s", e->d_name);
		FILE *g = fopen(p, "r");
		while (g && fgets(line, sizeof line, g)) {
			line[strcspn(line, "\r\n")] = 0;
			if (!strncmp(line, "Name=", 5)) snprintf(name, sizeof name, "%s", line + 5);
			else if (!strncmp(line, kfull, strlen(kfull)) || (!lname[0] && !strncmp(line, kshort, strlen(kshort))))
				snprintf(lname, sizeof lname, "%s", strchr(line, '=') + 1);
			else if (!strncmp(line, "Icon=", 5)) snprintf(im->icon, sizeof im->icon, "%s", line + 5);
		}
		if (g) fclose(g);
		snprintf(im->name, sizeof im->name, "%s", lname[0] ? lname : name[0] ? name : im->id);
		for (int k = 0; k < nen; k++) if (!strcmp(enabled[k], im->id)) im->enabled = k + 1;   /* 記住順序 */
		nims++;
	}
	if (d) closedir(d);
	/* 已啟用的排前面（依原本順序），其餘依名稱 */
	for (int i = 1; i < nims; i++)
		for (int j = i; j > 0; j--) {
			IM *a = &ims[j - 1], *b = &ims[j];
			int swap = a->enabled && b->enabled ? b->enabled < a->enabled : !a->enabled && b->enabled ? 1 :
			           !a->enabled && !b->enabled ? strcmp(b->name, a->name) < 0 : 0;
			if (!swap) break;
			IM t = *a; *a = *b; *b = t;
		}
}

/* 改寫 fcitx5 的 profile：先關掉 fcitx5（它結束時會寫回設定），寫好再啟動 */
static void write_fcitx_profile(void)
{
	int pid = running("fcitx5");
	if (pid) {
		kill(pid, SIGTERM);
		for (int i = 0; i < 40 && running("fcitx5"); i++) usleep(50000);
	}
	char path[PATH_MAX];
	fcitx_profile_path(path, sizeof path);
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", path);
	*strrchr(dir, '/') = 0;
	mkdir(dir, 0755);
	FILE *f = fopen(path, "w");
	if (f) {
		const char *lay = layouts[layout_sel];
		int first = -1, n = 0;
		for (int i = 0; i < nims; i++) if (ims[i].enabled && first < 0) first = i;
		fprintf(f, "[Groups/0]\nName=%s\nDefault Layout=%s\nDefaultIM=%s\n\n", _("預設"), lay,
		        first >= 0 ? ims[first].id : "");
		fprintf(f, "[Groups/0/Items/%d]\nName=keyboard-%s\nLayout=\n\n", n++, lay);
		for (int i = 0; i < nims; i++)
			if (ims[i].enabled) fprintf(f, "[Groups/0/Items/%d]\nName=%s\nLayout=\n\n", n++, ims[i].id);
		fprintf(f, "[GroupOrder]\n0=%s\n", _("預設"));
		fclose(f);
	}
	if (pid) pm_spawn("fcitx5 -d");
}

/* IBus：已啟用的輸入法存在 gsettings 的 preload-engines */
static void scan_ibus(void)
{
	nims = 0;
	char *en = pm_run("gsettings get org.freedesktop.ibus.general preload-engines");
	char *t = pm_run("ibus list-engine");
	char *save = NULL;
	for (char *l = t ? strtok_r(t, "\n", &save) : NULL; l && nims < (int)LEN(ims); l = strtok_r(NULL, "\n", &save)) {
		/* 格式：「  chewing - Chewing」；語言標題行沒有前置空白 */
		if (l[0] != ' ') continue;
		while (*l == ' ') l++;
		char *dash = strstr(l, " - ");
		if (!dash || !strncmp(l, "xkb:", 4)) continue;
		IM *im = &ims[nims++];
		memset(im, 0, sizeof *im);
		snprintf(im->id, sizeof im->id, "%.*s", (int)(dash - l), l);
		snprintf(im->name, sizeof im->name, "%s", dash + 3);
		char q[80];
		snprintf(q, sizeof q, "'%s'", im->id);
		im->enabled = en && strstr(en, q);
	}
	free(t);
	free(en);
}

static void write_ibus(void)
{
	char list[2048] = "['xkb:us::eng'";
	for (int i = 0; i < nims; i++)
		if (ims[i].enabled) {
			size_t l = strlen(list);
			snprintf(list + l, sizeof list - l, ", '%s'", ims[i].id);
		}
	strcat(list, "]");
	if (fork() == 0) {
		execlp("gsettings", "gsettings", "set", "org.freedesktop.ibus.general", "preload-engines", list, (char *)NULL);
		_exit(127);
	}
}

static void scan_ims(void)
{
	const char *fw = active_fw();
	if (!strcmp(fw, "fcitx5")) scan_fcitx();
	else if (!strcmp(fw, "ibus")) scan_ibus();
	else nims = 0;
}

/* ------------------------------------------------------------------ */
/* 畫面                                                                */
/* ------------------------------------------------------------------ */

static void build(PmUI *u)
{
	char buf[256];
	pm_ui_title(u, _("語言與輸入法"), NULL);

	pm_ui_group(u, _("語言與地區"));
	int old = lang_sel;
	if (pm_ui_choice(u, _("介面語言"), lang_opts, nlangs, &lang_sel)) {
		Lang *l = &langs[lang_sel];
		if (!l->installed) {
			add_locale(l->code);
			lang_sel = old;
		} else {
			locale_value(l, buf, sizeof buf);
			pm_conf_set("region.lang", buf);
			need_relogin = 1;
		}
	}
	old = fmt_sel;
	if (pm_ui_choice(u, _("日期、時間與數字格式"), lang_opts, nlangs, &fmt_sel)) {
		Lang *l = &langs[fmt_sel];
		if (!l->installed) {
			add_locale(l->code);
			fmt_sel = old;
		} else {
			locale_value(l, buf, sizeof buf);
			pm_conf_set("region.formats", buf);
			need_relogin = 1;
		}
	}
	pm_ui_end(u);
	if (need_relogin) {
		pm_ui_group(u, NULL);
		if (pm_ui_button(u, _("要登出再登入才會套用到所有程式"), _("立即登出"))) pm_logout();
		pm_ui_end(u);
	} else {
		pm_ui_note(u, _("標示為未安裝的語言，選擇後會開啟終端機產生語系（需要輸入密碼）。"));
	}

	pm_ui_group(u, _("鍵盤"));
	if (pm_ui_choice(u, _("鍵盤配置"), layout_names, LEN(layout_names), &layout_sel)) {
		pm_conf_set("input.layout", layouts[layout_sel]);
		if (fork() == 0) { execlp("setxkbmap", "setxkbmap", layouts[layout_sel], (char *)NULL); _exit(127); }
		if (!strcmp(active_fw(), "fcitx5")) write_fcitx_profile();
	}
	pm_ui_end(u);

	pm_ui_group(u, _("輸入法"));
	int ofw = fw_sel;
	if (pm_ui_choice(u, _("輸入法框架"), fw_names, LEN(fw_names), &fw_sel) && ofw != fw_sel) {
		pm_conf_set("input.framework", fw_keys[fw_sel]);
		need_relogin = 1;
		scan_ims();
	}
	const char *fw = active_fw();
	/* 還沒裝輸入法：只裝必要的部分（--no-install-recommends），
	 * 不然 fcitx5 會順便裝上 Qt 設定工具、所有程式框架的前端、im-config 和 zenity */
	const char *want = fw_keys[fw_sel];
	int use_ibus = !strcmp(want, "ibus");
	int missing = use_ibus ? !have_ibus() : !strcmp(want, "pomelo") ? !pm_have("pomelo-input")
	            : (strcmp(want, "none") && strcmp(want, "auto") && !pm_have("fcitx5"))
	            || (!strcmp(want, "auto") && !pm_have("pomelo-input") && !pm_have("fcitx5"));
	if (!strcmp(fw, "pomelo")) {
		pm_ui_end(u);
		/* 啟用哪些輸入法：已安裝的點一下啟用或停用，還沒安裝的點一下就安裝 */
		pm_ui_group(u, _("Pomelo 輸入法"));
		for (unsigned i = 0; i < LEN(engs); i++) {
			const Eng *e = &engs[i];
			int st = eng_state(e), on = st == E_READY && eng_enabled(e->id);
			char right[64];
			if (dl_busy && dl_eng == e) snprintf(right, sizeof right, _("下載中 %d%%"), dl_total > 0 ? (int)((dl_done + (dl_part[0] ? MAX(file_size(dl_part), 0) : 0)) * 100 / dl_total) : 0);
			else if (st == E_NEED_DATA) snprintf(right, sizeof right, _("要下載 %.1f MB"), rime_missing(e->rime) / 1e6);
			else snprintf(right, sizeof right, "%s", st == E_NEED_PKG ? _("要安裝") : st == E_NO_PKG ? _("這個系統沒有") : on ? _("已啟用") : "");
			if (pm_ui_item(u, NULL, _(e->name), _(e->desc), right[0] ? right : NULL, on)) {
				if (st == E_READY) eng_toggle(e->id);
				else if (st == E_NEED_PKG) eng_install(e);
				else if (st == E_NEED_DATA) { rime_start_download(e); pm_ui_redraw(u); }
			}
		}
		pm_ui_end(u);
		if (dl_err[0]) pm_ui_note(u, dl_err);
		pm_ui_note(u, _("輕點 Shift 或按 Ctrl+空白鍵 切換輸入法／英文，Ctrl+Shift+空白鍵 換下一個輸入法；選單列右側會顯示目前的輸入法（注、한…），點它也可以切換。不用 fcitx 或 ibus。"));
		pm_ui_note(u, _("拼音、倉頡、速成、粵拼使用 Rime（中州韻）引擎，字典從網路下載；第一次使用要先花十幾秒整理字典。"));
		pm_ui_group(u, _("切換與選字"));
		int sh = pm_conf_int("input.shift_toggle", 1);
		if (pm_ui_switch(u, _("輕點 Shift 切換輸入法／英文"), &sh)) pm_conf_set_int("input.shift_toggle", sh);
		static const char *cand_opts[] = { "5", "7", "9" };
		static const int cand_v[] = { 5, 7, 9 };
		int cn = pm_conf_int("input.cand_per_page", 9), cs = 2;
		for (unsigned i = 0; i < LEN(cand_v); i++) if (cand_v[i] == cn) cs = (int)i;
		if (pm_ui_choice(u, _("每頁候選字"), cand_opts, LEN(cand_opts), &cs)) pm_conf_set_int("input.cand_per_page", cand_v[cs]);
		pm_ui_end(u);
		if (eng_state(eng_by_id("hangul")) == E_READY && eng_enabled("hangul")) {
			pm_ui_group(u, _("韓文"));
			static const char *hk_keys[] = { "2", "3f", "39", "ro" };
			static const char *hk_names[] = { N_("兩排式"), N_("三排式（最終）"), N_("三排式（390）"), N_("羅馬字") };
			const char *hk = pm_conf_get("input.hangul_kb", "2");
			int hs = 0;
			for (unsigned i = 0; i < LEN(hk_keys); i++) if (!strcmp(hk_keys[i], hk)) hs = (int)i;
			const char *hn[LEN(hk_names)];
			for (unsigned i = 0; i < LEN(hk_names); i++) hn[i] = _(hk_names[i]);
			if (pm_ui_choice(u, _("鍵盤"), hn, LEN(hk_names), &hs)) pm_conf_set("input.hangul_kb", hk_keys[hs]);
			pm_ui_end(u);
		}
		if (eng_enabled("viet")) {
			pm_ui_group(u, _("越南文"));
			static const char *vm_keys[] = { "telex", "vni" };
			static const char *vm_names[] = { "Telex", "VNI" };
			int vs = !strcmp(pm_conf_get("input.viet", "telex"), "vni");
			if (pm_ui_choice(u, _("打字方式"), vm_names, LEN(vm_names), &vs)) pm_conf_set("input.viet", vm_keys[vs]);
			pm_ui_end(u);
			pm_ui_note(u, _("Telex：aa → â、ow → ơ、dd → đ，s f r x j 加聲調。VNI：6 7 8 9 加符號，1～5 加聲調。"));
		}
		if (eng_enabled("anthy")) pm_ui_note(u, _("日文：打羅馬字（nihongo），空白鍵轉換成漢字，Enter 送出；F7 片假名。日文鍵盤的「半角/全角」鍵切換日文／英文。"));
		pm_ui_group(u, _("注音"));
		/* 自家的注音：新酷音引擎的設定 */
		static const char *kb_keys[] = { "KB_DEFAULT", "KB_HSU", "KB_ET", "KB_ET26", "KB_IBM", "KB_GIN_YIEH", "KB_DVORAK", "KB_HANYU_PINYIN", "KB_THL_PINYIN", "KB_MPS2_PINYIN" };
		static const char *kb_names[] = { N_("標準"), N_("許氏"), N_("倚天"), N_("倚天 26 鍵"), "IBM", N_("精業"), "Dvorak", N_("漢語拼音"), N_("台灣華語拼音"), N_("注音二式") };
		const char *kb = pm_conf_get("input.kb", "KB_DEFAULT");
		int kb_sel = 0;
		for (unsigned i = 0; i < LEN(kb_keys); i++) if (!strcmp(kb_keys[i], kb)) kb_sel = (int)i;
		if (pm_ui_choice(u, _("注音鍵盤"), kb_names, LEN(kb_names), &kb_sel)) pm_conf_set("input.kb", kb_keys[kb_sel]);
		int sp = pm_conf_int("input.space_select", 1);
		if (pm_ui_switch(u, _("空白鍵選字"), &sp)) pm_conf_set_int("input.space_select", sp);
	} else if (missing) {
		pm_ui_info(u, _("還沒有安裝輸入法"), NULL);
		int use_pomelo = !strcmp(want, "pomelo") || !strcmp(want, "auto");
		if (pm_ui_button(u, use_ibus ? _("安裝 IBus 與新酷音注音") : use_pomelo ? _("安裝 Pomelo 注音（最輕量，建議）") : _("安裝 Fcitx 5 與新酷音注音"), _("安裝"))) {
			install_in_terminal(use_ibus ? "ibus ibus-chewing ibus-gtk3 ibus-gtk4"
			                    : use_pomelo ? "pomelo-input"
			                    : "fcitx5 fcitx5-chewing fcitx5-frontend-gtk3 fcitx5-frontend-gtk4");
			if (!strcmp(want, "auto")) { pm_conf_set("input.framework", use_ibus ? "ibus" : use_pomelo ? "pomelo" : "fcitx5"); need_relogin = 1; }
		}
	} else if (!strcmp(fw, "none")) {
		pm_ui_info(u, _("目前不使用輸入法"), NULL);
	} else {
		for (int i = 0; i < nims; i++) {
			if (pm_ui_item(u, NULL, ims[i].name, ims[i].id, NULL, ims[i].enabled)) {
				ims[i].enabled = ims[i].enabled ? 0 : 999;
				if (!strcmp(fw, "fcitx5")) { write_fcitx_profile(); scan_fcitx(); }
				else { write_ibus(); }
			}
		}
		if (!nims) pm_ui_info(u, !strcmp(fw, "ibus") ? _("IBus 沒有在執行") : _("找不到可用的輸入法"), NULL);
	}
	pm_ui_end(u);
	if (!strcmp(fw, "pomelo")) pm_ui_note(u, _("Shift+空白鍵 切換全形／半形；選字時 ←→ 移動、Enter 選定，也可以用滑鼠點、滾輪翻頁。"));
	else if (!strcmp(fw, "fcitx5")) pm_ui_note(u, _("按 Ctrl+空白鍵 切換輸入法。勾選的輸入法會依序輪流切換。"));
	else if (!strcmp(fw, "ibus")) pm_ui_note(u, _("IBus 預設用 Super+空白鍵 切換，這和「搜尋」的快速鍵衝突，建議改用 Fcitx 5。"));

	/* 還沒安裝的常用輸入法 */
	int shown = 0;
	for (unsigned i = 0; i < LEN(suggests) && strcmp(fw, "none") && strcmp(fw, "pomelo"); i++) {
		const char *pkg = !strcmp(fw, "ibus") ? suggests[i].ibus_pkg : suggests[i].fcitx_pkg;
		static int cache[LEN(suggests)][2];   /* 0 未查、1 已裝、2 未裝 */
		int k = !strcmp(fw, "ibus");
		if (!cache[i][k]) cache[i][k] = pkg_installed(pkg) ? 1 : 2;
		if (cache[i][k] == 1) continue;
		if (!shown++) pm_ui_group(u, _("加入其他輸入法"));
		snprintf(buf, sizeof buf, "%s（%s）", _(suggests[i].name), pkg);
		if (pm_ui_button(u, buf, _("安裝"))) {
			install_in_terminal(pkg);
			cache[i][k] = 0;
		}
	}
	if (shown) pm_ui_end(u);
}

/* 每 3 秒：語系或輸入法可能剛安裝好；下載中每秒更新進度 */
static void tick(PmUI *u)
{
	static int n;
	if (dl_finished) {   /* 方案資料下載好了：直接啟用（pomelo-input 看到設定改了就開始部署） */
		if (!eng_enabled(dl_finished->id)) eng_toggle(dl_finished->id);
		else {   /* 本來就啟用（例如介面語言預設的）：照樣寫一次設定，通知 pomelo-input 部署新的字典 */
			char cur[512];
			snprintf(cur, sizeof cur, "%s", pm_conf_get("input.engines", default_engines()));
			pm_conf_set("input.engines", cur);
		}
		dl_finished = NULL;
		pm_ui_redraw(u);
	}
	if (dl_busy) pm_ui_redraw(u);
	if (++n % 3) return;
	lib_gen++;
	int ol = lang_sel, of = fmt_sel;
	scan_langs();
	if (need_relogin) { lang_sel = ol; fmt_sel = of; }
	if (u->active == 0) scan_ims();
	pm_ui_redraw(u);
}

static cairo_surface_t *icon(int size)
{
	cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
	cairo_t *cr = cairo_create(s);
	cairo_scale(cr, size / 128.0, size / 128.0);
	pm_rrect(cr, 10, 10, 108, 108, 26, 26);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, 10, 0, 118);
	cairo_pattern_add_color_stop_rgb(g, 0, 0.35, 0.78, 0.55);
	cairo_pattern_add_color_stop_rgb(g, 1, 0.13, 0.60, 0.40);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	/* 地球 */
	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_set_line_width(cr, 5);
	cairo_arc(cr, 64, 64, 34, 0, 2 * M_PI);
	cairo_stroke(cr);
	cairo_save(cr);
	cairo_translate(cr, 64, 64);
	cairo_scale(cr, 0.45, 1);
	cairo_arc(cr, 0, 0, 34, 0, 2 * M_PI);
	cairo_restore(cr);
	cairo_stroke(cr);
	cairo_move_to(cr, 30, 64); cairo_line_to(cr, 98, 64);
	cairo_move_to(cr, 36, 46); cairo_line_to(cr, 92, 46);
	cairo_move_to(cr, 36, 82); cairo_line_to(cr, 92, 82);
	cairo_stroke(cr);
	cairo_destroy(cr);
	return s;
}

int main(int argc, char *argv[])
{
	if (argc > 2 && !strcmp(argv[1], "--icon")) return pm_ui_write_icon(icon, argv[2]);
	setlocale(LC_ALL, "");
	pm_conf_load();
	scan_langs();
	const char *lay = pm_conf_get("input.layout", "us");
	for (unsigned i = 0; i < LEN(layouts); i++) if (!strcmp(layouts[i], lay)) layout_sel = (int)i;
	const char *fw = pm_conf_get("input.framework", "auto");
	for (unsigned i = 0; i < LEN(fw_keys); i++) if (!strcmp(fw_keys[i], fw)) fw_sel = (int)i;
	scan_ims();
	pm_ui_native = 1;   /* build() 裡沒有直接呼叫 X：Wayland 工作階段直接當原生程式 */
	return pm_ui_run(_("語言與輸入法"), "pomelo-language", 660, 720, build, tick, 1000, icon);
}
