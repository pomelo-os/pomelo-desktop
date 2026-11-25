# pomelo-archive — Pomelo 壓縮管理員

壓縮、解壓縮、加密。一個程式管 zip、7z、tar（gz／bz2／xz／zst／lz／lz4）、cpio、iso、squashfs、磁碟映像（img）等格式，
有圖形介面（瀏覽、右鍵、拖放）也有命令列（腳本、測試、小幫手用）。

## 支援的格式

| 格式 | 讀 | 建立 | 加密 | 說明 |
|---|---|---|---|---|
| zip、jar、apk、epub | ✔ | ✔ | AES-256（舊式 ZipCrypto 可讀可寫，標示較不安全） | libarchive。檔名編碼自動偵測（Big5／GBK／Shift-JIS／EUC-KR） |
| 7z | ✔ | ✔ | AES-256，可連檔名一起加密 | 加密的 7z 要有 7-Zip；沒加密的 libarchive 就能讀寫 |
| tar、tar.gz、tar.bz2、tar.xz、tar.zst、tar.lz、tar.lz4 | ✔ | ✔ | OpenPGP 對稱加密（gpg，存成 `.gpg`） | libarchive |
| cpio（newc／crc）、ar、deb、rpm | ✔ | cpio ✔ | cpio 可（gpg） | |
| iso（ISO 9660，含 Joliet／Rock Ridge） | ✔ | ✔ | ✘ | |
| squashfs（.sq／.sfs） | ✔（7-Zip） | ✔（mksquashfs） | ✘ | |
| 磁碟映像（FAT／ext／NTFS 的 .img） | ✔（7-Zip） | ✘ | ✘ | 讀裡面的檔案 |
| 單一檔案壓縮：gz、bz2、xz、zst、lz、lzma、lz4、Z | ✔ | gz bz2 xz zst lz ✔ | gpg | |
| cab、lha、xar、dmg、vhd、wim | ✔ | ✘ | ✘ | libarchive 或 7-Zip |
| `.gpg`／`.pgp`（整包加密的 tar 等） | ✔ | — | — | 輸入密碼後解密再讀 |

實際能做什麼要看這台電腦裝了什麼：`pomelo-archive --formats` 會列出 libarchive 版本、7-Zip／gpg 有沒有、每個格式能不能建立與加密，
缺的會說缺什麼、怎麼裝；介面上做不到的選項會顯示原因，不會默默失敗。

## 後端

1. **libarchive**（行程內）：絕大部分的讀與寫。
2. **7-Zip**（外部程式 `7zz`／`7z`／`7za`）：SquashFS、磁碟映像、加密的 7z（讀、寫、連檔名加密）。
3. **gpg**：tar／cpio 等格式的加密（`gpg --symmetric --cipher-algo AES256`，密碼用 `--passphrase-fd`）。

密碼**從不**放在命令列（ps 看得到）：7z 從標準輸入讀、gpg 用 passphrase-fd、libarchive 在行程內傳；用完就清掉，也不寫進日誌或設定檔。

## 安全

解壓縮是最容易被惡意檔案攻擊的地方，這裡的做法：

- 路徑：`..`、絕對路徑一律擋掉（略過並告訴使用者略過了幾個）。
- 符號連結：指到解壓目錄外面的、絕對路徑的、`..` 夾在中間的，一律不還原；整包只要有項目要「穿過」同一個封存檔裡的符號連結寫入，整包擋下（結束碼 6）；
  解壓時再用 libarchive 的 SECURE_SYMLINKS 擋一次。
- 權限：去掉 setuid／setgid／sticky；裝置檔、管線、socket 略過；不套用封存檔裡的擁有者。
- 硬連結：指到外面的略過。
- 壓縮炸彈：解壓前比對解開後大小與壓縮檔大小（超過 1 GB 而且壓縮比超過 100 倍，或項目超過 200 萬個，先警告，`--force` 才繼續），解壓時累計寫入量超過標示大小太多就停。
- 磁碟空間：解壓前先看目的地的剩餘空間，不夠就先講（結束碼 7）。
- 先解到暫存資料夾（目的地旁邊的 `.pomelo-extract-XXXXXX`，同一個檔案系統），全部成功才搬過去；失敗、取消都不留半成品。
- 同名衝突：先問（取代／兩個都留／略過／取消），搬動時不跟著符號連結走。
- 外部程式一律用 argv 執行（沒有 shell），輸出入用管子。

## 命令列

```
pomelo-archive [檔案]                      開視窗瀏覽
pomelo-archive --extract-here 檔案…        解壓到壓縮檔旁邊（頂層不只一項就建一個資料夾），有進度視窗
pomelo-archive --extract-to 檔案 資料夾 [--only 路徑]… [--password-stdin] [--charset 編碼]
pomelo-archive --compress 檔案或資料夾…     建立壓縮檔（視窗：名稱、位置、格式、壓縮程度、密碼）
pomelo-archive --add-gui 封存檔 檔案…      加入檔案（有進度視窗）
pomelo-archive --ui-password 檔案 [--wrong]  密碼視窗（把密碼印到標準輸出，給別的程式用）

無介面：
pomelo-archive --formats
pomelo-archive --list 壓縮檔 [--password-stdin] [--charset 編碼]    TSV：類型 大小 時間 路徑（第一行摘要在標準錯誤）
pomelo-archive --test 壓縮檔 [--password-stdin]
pomelo-archive --extract 壓縮檔 [-C 目錄] [--only 路徑]… [--password-stdin] [--overwrite=ask|yes|no|rename] [--into-folder] [--force]
pomelo-archive --create 輸出檔 [--format=…] [--level=1-9] [--password-stdin] [--encrypt-names] 檔案…
pomelo-archive --add 封存檔 檔案… [--password-stdin]
```

結束碼：0 成功、1 失敗、2 要密碼、3 密碼不對、4 缺外部程式、5 取消、6 內容不安全、7 空間不夠、8 疑似壓縮炸彈、9 目的地有同名項目。
`--format` 可以是 zip、zip-zipcrypto、7z、tar、tar.gz、tar.bz2、tar.xz、tar.zst、tar.lz、tar.lz4、cpio、iso、squashfs、gz、bz2、xz、zst、lz；沒指定就看輸出檔的副檔名。

## 程式結構

| 檔案 | 內容 |
|---|---|
| ac.h、ac-int.h | 核心 API（不含介面；不 `#include <pomelo.h>`，那邊有 `dpy`、`T`、`sh` 之類的巨集） |
| ac-util.c | 路徑整理、外部程式執行、工具偵測 |
| ac-read.c | 開啟、列出（libarchive／7-Zip／gpg）、編碼偵測、測試、安全解壓 |
| ac-write.c | 建立（libarchive／7-Zip／mksquashfs／gpg）、加入檔案、能力偵測（真的寫一個小檔進記憶體再讀回來） |
| cli.c | 命令列 |
| gui-dialogs.c | 密碼、解壓縮（進度、要密碼、衝突、炸彈警告）、建立壓縮檔、加入檔案：`pm_ui_run`，可以當原生 Wayland 程式（`pm_ui_native = 1`），背景執行緒做事 |
| gui-browse.c | 瀏覽視窗：X11 自繪（跟檔案管理員一樣；沒有 pm_ui 的右鍵、拖放與上萬列的虛擬捲動可以用） |
| gui-common.c | 圖示、子程序 |

瀏覽視窗裡耗時的事都另外開子程序（解壓縮、加入、測試、問密碼、把檔案解到暫存再用預設程式打開），視窗不會卡住。

## 測試

```
make test                                        # 核心：往返、加密、跨工具、編碼、選取與覆蓋、惡意封存檔、大檔
bash test/test-gui.sh                            # 介面：Xvfb＋xdotool 點過一遍（SHOTS=資料夾 會存截圖）
bash test/run-distro.sh ubuntu24.04              # 在乾淨的發行版容器裡裝套件、編譯、跑上面兩個（要有 ~/wm/dist/<發行版>/ 的 libpomelo 套件）
```

## 已知限制

- **不支援 RAR**（授權：RAR 的解壓縮碼有 unRAR 限制條款，不是自由軟體）。偵測到 RAR（開頭魔數 `Rar!` 或副檔名 .rar／.r00）一律回「不支援的格式」，不交給 libarchive 或 7-Zip，也不呼叫任何 rar／unrar 程式；測試裡有放假的 rar／unrar 進 PATH 確認沒被呼叫。
- 加密 ZIP 裡的符號連結：libarchive 讀不出連結目標，不會還原（會告訴使用者略過了幾個）。
- 瀏覽視窗沒有「拖出去」（從壓縮管理員拖檔案到檔案管理員）；要用「解壓縮到…」。
- 沒有分割壓縮（多個 .001 檔）、沒有修復損壞的壓縮檔、沒有刪除封存檔裡的單一項目。
- 「解壓縮到其他位置」借用「另存新檔」的對話窗輸入資料夾名稱（libpomelo 目前沒有選資料夾的對話窗）。
- 加入檔案（zip／tar 系列）是把整個封存檔重寫一遍，大檔要等一下。
- tar 系列的加密是整包用 gpg：看不到裡面的檔名，也不能直接「加入」。

## 授權

**pomelo-archive 本身是 GPL-3.0-or-later。不連結、不打包任何非自由或收費的元件**；外部程式都是用 `execvp` 呼叫系統上裝好的套件（沒有 shell），不是程式的一部分。
ZIP／7z 的加密只用 AES-256（libarchive 的 `zip:encryption=aes256`、7-Zip 的 AES-256），tar 系列用 gpg 的 AES-256 對稱加密；都是公開標準、沒有專利費。

### RAR：一律不支援
RAR 解壓縮碼的授權帶有 unRAR 限制條款（不能拿來做出和 RAR 競爭的壓縮程式），不是自由軟體。所以：

- 偵測到 RAR（開頭魔數 `Rar!`，或副檔名 `.rar`／`.r00`）一律回「不支援的格式」（`--list`、`--test`、`--extract`、瀏覽視窗都一樣，結束碼 1），
  **不管** libarchive 或 7-Zip 碰巧能不能讀。
- 不呼叫任何 `rar`／`unrar`／`unrar-nonfree` 執行檔；`test/test-core.sh` 的 `rar` 區段把假的 rar、unrar 放進 PATH 驗證沒被呼叫。
- 介面、`--formats`、`.desktop`（Comment、Keywords、MimeType）、歡迎頁都不宣傳 RAR。

### 相依套件的實際授權（在各發行版容器裡用 `dpkg` 的 copyright 檔、`rpm -qi` 與 7-Zip 自己的 `7z i` 查得）

| 套件 | 授權 | 備註 |
|---|---|---|
| libarchive（必要，連結） | 22.04 3.6.0、24.04 3.7.2、Debian 13 3.7.4、26.04 3.8.5：BSD-2-clause（另有少數檔案 Expat、BSD-4-clause-UCB、Apache-2.0、PD）；Rocky 9.6 3.5.3：BSD；Fedora 44 3.8.7：BSD-2-Clause 等（建置腳本 GPL-2.0-or-later WITH Libtool-exception） | 自由、寬鬆 |
| gnupg（選配，tar／cpio 加密） | Ubuntu／Debian：GPL-3+（含 LGPL、Expat 等）；Rocky 9.6 gnupg2 2.3.3：GPLv3+；Fedora 44 gnupg2 2.4.9：GPL-2.0-or-later／GPL-3.0-or-later 等 | 另外執行，不連結 |
| squashfs-tools（選配，建立 SquashFS） | GPL-2+（Ubuntu 22.04 4.5、24.04 4.6.1、26.04 4.7.5、Debian 13 4.6.1）；Rocky 9.6 4.4：GPLv2+；Fedora 44 4.6.1：GPL-2.0-or-later | 另外執行 |
| 7-Zip（選配，讀 SquashFS／磁碟映像、加密 7z） | 見下 | 另外執行 |

**7-Zip 各發行版實際情況**（`7z i` 列出格式的結果）：

| 發行版 | 套件 | 授權欄位 | 這個套件裡有沒有 RAR 編解碼器 |
|---|---|---|---|
| Ubuntu 22.04 | 7zip 21.07+dfsg | LGPL-2.1+、BSD-3-clause、public-domain（copyright 檔仍註明「RAR code is under the non-free unRAR license restriction」） | 沒有（`7zz i` 沒有 Rar 格式） |
| Ubuntu 24.04 | 7zip 23.01+dfsg | 同上 | **有**（Rar、Rar5） |
| Ubuntu 26.04 | 7zip 26.00+dfsg | 同上 | **有** |
| Debian 13 | 7zip 25.01+dfsg | 同上 | **有** |
| Fedora 44 | 7zip 26.04 | LGPL-2.1-or-later AND BSD-3-Clause AND BSD-2-Clause AND 公眾領域 | 沒有（90 種格式，無 Rar） |
| Rocky 9.6 | 7zip 26.04（EPEL；舊的 p7zip 16.02 在 EPEL，授權 LGPLv2 AND (LGPLv2+ OR CPL-1.0)） | 同 Fedora | 沒有（90 種格式，無 Rar） |

結論：
- Fedora 與 Rocky（EPEL）的 7-Zip 已經沒有 RAR 碼，沒有 unRAR 限制的問題。Rocky 9.6 要先裝 epel-release 與 crb 才有 7zip。
- Ubuntu 24.04／26.04 與 Debian 13 的 `7zip` 套件裡**仍含**受 unRAR 限制的 RAR 解壓縮碼（套件的 copyright 自己註明）。那是發行版的套件，不是我們打包的；
  pomelo-archive 不會用它讀 RAR。但如果要避免在系統上放這個套件，建議把 `7zip` 當**選配**（`Suggests`）而不是 `Depends`／`Recommends`：沒有它時，
  只有 SquashFS／磁碟映像讀取與加密 7z 這幾項功能會灰掉並說明原因，其他格式（zip、tar 系列、cpio、iso、未加密的 7z 讀寫…）都照常。
- libarchive 的 RAR 讀取程式是 BSD 授權的獨立實作（沒有 unRAR 限制），但依上面的決定一樣不放行。
