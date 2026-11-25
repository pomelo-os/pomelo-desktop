# Pomelo 🍊

輕量、省資源的 X11 桌面。全部用 C 寫成，只依賴 Xlib、cairo 等基本函式庫；
每個元件都是獨立套件，可以只裝需要的部分。

## 元件與相依關係

```
libpomelo0  共用函式庫：繪圖、圖示、選單、設定檔、設定介面元件、圖片（PNG/JPEG，依 EXIF 轉正）
 ├─ pomelo-wm               視窗管理器：圓角外框、搜尋、視窗總覽、多螢幕
 ├─ pomelo-panel            頂端選單列 + Dock
 ├─ pomelo-control-center   控制中心（選單列右上角的快速設定）
 ├─ pomelo-terminal         終端機
 ├─ pomelo-files            檔案管理員（圖示／列表、側邊欄；可拖放；右鍵可新增資料夾／文件）
 ├─ pomelo-edit             文字編輯器：選單列、多個分頁、左邊的檔案清單、尋找與取代、行號、Big5 自動轉碼
 │                          （.txt 等文字檔預設用它開，從檔案管理員再打開的會變成同一個視窗的新分頁）
 ├─ pomelo-calc             計算機（基本／工程模式、邊打邊算、計算紀錄）
 ├─ pomelo-calendar         行事曆（月／週／日／議程、重複與提醒、iCalendar、台灣假日與農曆、CalDAV；背景提醒）
 ├─ pomelo-doctor           主機醫生（體檢歷史與趨勢預測、為什麼變慢診斷、安全總覽、入侵檢查、日誌檢視與事件時間線、被攻擊時的處置；每天採樣一次、不常駐）
 ├─ pomelo-backup           備份（rsync 硬連結快照、排程、保留策略、還原與誤刪救援；主機醫生檢查有沒有備份）
 ├─ pomelo-vpn              VPN 伺服器精靈（OpenVPN 伺服器、Tailscale；下載安裝、憑證與客戶端管理、診斷）
├─ pomelo-firewall         防火牆管理員（iptables 的圖形管理介面：白話規則、風險建議、30 秒自動還原、攻擊防護與自動封鎖；不取代 ufw／firewalld）
 ├─ pomelo-netscan          網路探索（盤點設備與埠、SNMP v1／v2c／v3、衝突與異常偵測、埠政策、封包異常、CVE 保守比對、整體網路安全評估、可互動拓樸圖與歷史比較；唯讀、只掃私有網段）
 ├─ pomelo-gpu              顯卡監視（顯卡使用率、顯示記憶體、溫度、功耗、處理程序與所屬容器、載入在顯示記憶體裡的 AI 模型；NVML／nvidia-smi／sysfs，唯讀）
 ├─ pomelo-packets          封包分析器（pcap／pcapng 分析與擷取、RTP／VoIP 品質、SIP 流程圖；預設只做統計，還原需逐次授權）
 ├─ pomelo-viewer           看圖（PNG、JPEG、GIF／WebP 動畫、BMP；轉正、縮放、幻燈片、垃圾桶可復原）
 │                          相依 libgif、libwebp（各約 0.1～0.3 MB）；HEIC／AVIF 第一次打開時才安裝 libheif
 ├─ pomelo-paint            繪圖（鉛筆、筆刷、噴槍、橡皮擦、油漆桶、取色、文字、直線、箭頭、矩形、橢圓；選取搬移縮放、
 │                          拷貝貼上、裁切、旋轉翻轉、調整大小、復原重做；開檔跟看圖共用解碼，存 PNG／JPEG／BMP）
 ├─ pomelo-player           影音播放器（播放清單、音樂庫〔歌手／專輯〕、封面、隨機／重複、字幕、選單列小圖示、媒體鍵）
 │                          MP3／FLAC／Ogg／Opus／WAV 用系統本來就有的 libmpg123、libsndfile；
 │                          mp4／mkv／webm／avi／mov 影片與 M4A／AAC 需要 ffmpeg（使用者自己另外裝，不在相依裡；第一次遇到時程式會問、按一下就裝好）；
 │                          ffmpeg 缺 HEVC 等解碼器時（Fedora、Rocky 的 ffmpeg-free）畫面上直接說明怎麼補
 ├─ pomelo-pdf              PDF 閱讀器（PDF 核心自己寫：縮圖、書籤、搜尋、選取拷貝文字、連結、密碼保護的檔案、列印、分頁；
 │                          註解、簽名、整理頁面、合併、加頁碼、數位簽章（.p12 簽署與驗證）、改頁面上的文字、移除浮水印，附加更新存檔、復原）
 │                          只用系統本來就有的 zlib、FreeType、libjpeg、libcrypto；建議 poppler-data（中日韓 CMap 資料）
 ├─ pomelo-remote           遠端桌面（RDP：連到 Windows 電腦的桌面）：常用電腦清單、密碼存鑰匙圈、解析度跟著視窗／全螢幕／自訂、
 │                          顏色深度與網路品質（頻寬）、剪貼簿文字雙向、聲音、分享本機資料夾／印表機給遠端電腦；
 │                          協定用 FreeRDP 函式庫，視窗與介面自己畫，不用 xfreerdp         建議 pomelo-keyring
 ├─ pomelo-vm               虛擬機器（QEMU／KVM）：建立、開關機、暫停、換光碟、快照（可含記憶體）、儲存狀態下次接著用；
 │                          x86_64 用 KVM 硬體加速，ARM64 用軟體模擬；UEFI／傳統 BIOS、安全開機、TPM 2.0（swtpm）；
 │                          自己寫的畫面視窗（VNC 用戶端：解析度跟著視窗、全螢幕、聲音）、USB 裝置穿透、
 │                          NAT（可轉送連接埠）或橋接網路、每台的 CPU／記憶體／磁碟／網路使用量圖表；
 │                          不用 libvirt，QEMU 第一次使用時才安裝（只裝需要的部分，不裝 QEMU 的 GTK 介面）
 ├─ pomelo-docker           容器（Docker 管理）：一鍵安裝 Docker；容器清單（含 compose 專案）、開始／停止／重新啟動／暫停／刪除、
 ├─ pomelo-cluster          容器叢集（Kubernetes 管理，選裝）：引擎 k3s；體檢硬體給角色建議、一台或多台、Server／Client／NFS 儲存；儀表板、Pod 日誌與終端機、套用 YAML、匯入 kubeconfig
 │                          從映像建立（連接埠、資料夾、環境變數、自動啟動）、在終端機進到容器、看紀錄；映像下載與清理、資料卷；
 │                          備份（容器設定＋映像＋資料卷裡的檔案，可以在別台電腦還原）與還原；每個容器的 CPU／記憶體／網路／磁碟圖表；
 │                          直接對 Docker 的 socket 講 Engine API，Docker 第一次使用時才安裝
 ├─ pomelo-archive          壓縮管理員：zip／7z／tar（gz、bz2、xz、zst、lz、lz4）／cpio／iso／squashfs／磁碟映像等格式的瀏覽、壓縮、解壓縮與加密
 │                          （ZIP、7z 用 AES-256；tar 系列用 gpg）；檔案管理員右鍵「解壓縮到這裡」「壓縮…」、雙擊壓縮檔直接開；
 │                          有進度、同名衝突、加密壓縮檔密碼；解壓縮前先擋路徑穿越、符號連結逃逸、壓縮炸彈；不支援 RAR（授權）；
 │                          讀寫用 libarchive，7-Zip、gnupg、squashfs-tools 只是選配（見 docs/壓縮管理員.md）
 ├─ pomelo-writer           文書處理器：開啟、編輯、存檔 Word（.docx）與 OpenDocument（.odt）文件；
 │                          圖形化開啟檔案（點資料夾、點檔案）、標題樣式＋大綱面板、粗體／斜體／底線／字型／字級／顏色、
 │                          段落對齊、清單、簡單表格、插入 PNG／JPEG 圖片、頁碼、尋找與取代、版面設定（紙張大小／邊界）、
 │                          列印（先看頁面預覽）、復原重做、剪貼簿；
 │                          文件模型、排版引擎、.docx／.odt 讀寫都自己寫，只依賴系統本來就有的 zlib 與 cairo
 ├─ pomelo-sheet            試算表：開啟、編輯、存檔 Excel（.xlsx）與 OpenDocument（.ods）試算表；
 │                          儲存格選取與編輯、公式（SUM／IF／VLOOKUP…等常用函式）、粗體／斜體／底線／字型／字級／
 │                          文字與背景顏色／對齊、多工作表（新增、切換、重新命名、刪除）、插入刪除列欄、
 │                          圖形化開啟檔案、尋找、復原重做、剪貼簿；
 │                          試算表模型、公式引擎、.xlsx／.ods 讀寫都自己寫，只依賴系統本來就有的 zlib 與 cairo
 ├─ pomelo-slides           簡報：開啟、編輯、存檔 PowerPoint（.pptx）與 OpenDocument（.odp）簡報；
 │                          投影片縮圖側欄（新增、切換、拖曳排序）、文字方塊／矩形／橢圓／線條／圖片、物件選取拖曳與
 │                          調整大小、粗體／斜體／底線／字型／字級／對齊、外框顏色／填滿顏色／投影片背景色、
 │                          圖形化開啟檔案、全螢幕播放、復原重做；
 │                          簡報模型、.pptx／.odp 讀寫都自己寫，只依賴系統本來就有的 zlib 與 cairo
 ├─ pomelo-theme            外觀設定        （深色模式、強調色、桌布、Dock）
 ├─ pomelo-sound            音效設定        相依 pulseaudio-utils
 ├─ pomelo-network          網路設定        相依 network-manager；Wi-Fi、企業 Wi-Fi（PEAP／TTLS）、VPN（WireGuard 匯入 .conf、OpenVPN 匯入 .ovpn，建議 network-manager-openvpn）
 ├─ pomelo-launcher         啟動器（全螢幕應用程式選單，可搜尋、分類頁籤，圖示可拖曳改分類、排順序、加入 Dock）
 ├─ pomelo-software         軟體中心（推薦軟體、搜尋、安裝 / 移除、系統更新）
 ├─ pomelo-display          顯示器（解析度、更新頻率、旋轉、多螢幕排列；15 秒未確認自動還原）
 ├─ pomelo-bluetooth        藍牙（搜尋、配對、連線、電量）   相依 bluez
 ├─ pomelo-monitor         活動監視器（CPU、記憶體、磁碟 I/O、網路即時圖表，程序列表、結束程序）
 ├─ pomelo-storage         儲存空間（磁碟用量、硬碟健康、外接裝置掛載／退出／格式化、清理、資料夾分析）
 │                          + pomelo-automount：插入隨身碟自動掛載並通知     相依 udisks2
 ├─ pomelo-polkit          密碼認證視窗（需要管理員權限時輸入密碼；取代 policykit-1-gnome，不需要 GTK）
 ├─ pomelo-keyring         鑰匙圈：幫 Chrome、VS Code 等程式保管密碼（標準的 Secret Service，程式不用改）
 │                          整個檔案含標籤與屬性都用登入密碼加密；PAM 模組讓登入時自動解鎖；取代 gnome-keyring
 ├─ pomelo-assistant       小幫手「小柚」：在本機執行的 AI，以 3D Q 版機器人的樣子在桌面上走動（純軟體繪製），
 │                          點牠或 Super+Shift+Space 才停下來聽你說；聊天、開程式、找檔案、調設定
 │                          六種配色＋深色版、音效（程式即時合成）、可以把回答念出來（離線的 Piper 語音）
 │                          用說的：按麥克風或 Super+Shift+V，在本機辨識（sherpa-onnx＋SenseVoice，離線），說完自動送出
 │                          快速指令：開程式、開網站、開檔案、音量、深色模式、提醒…不用等模型，不到 0.1 秒
 │                          模型第一次使用時下載（約 1.1～2.5 GB），詳見 docs/AI小幫手.md
 │                          選用「線上大腦」（OpenAI 相容／Claude／Gemini，自己的 API 金鑰、同意後才啟用，預設關閉；常用指令仍只在本機）
 ├─ pomelo-language         語言與輸入法（介面語言、地區格式、鍵盤配置、輸入法框架）
 ├─ pomelo-input            輸入法：XIM 伺服器＋自己畫的選字列，不用 fcitx／ibus（約 4 MB）；注音（libchewing）內建，
 │                          拼音／倉頡／速成／粵拼（librime）、日文（Anthy）、韓文（libhangul）執行時才載入，越南文自己寫；
 │                          Shift 輕點或 Ctrl+Space 切輸入法／英文，Ctrl+Shift+Space 換輸入法（見 docs/輸入法.md）
 ├─ pomelo-screenshot       螢幕截圖與錄影：Super+Shift+3／4／5、右下角縮圖、拷貝到剪貼簿；
 │                          錄影：整個螢幕／視窗／選取範圍，存成 MP4（H.264）、WebM（VP9／Opus）或 MKV，品質小／標準／高；
│                          可錄麥克風與電腦的聲音、疊上視訊鏡頭小畫面、暫停與繼續；選單列顯示紅色計時（右鍵：暫停、停止、放棄）；
│                          錄影才需要 ffmpeg（使用者自己另外安裝；沒有 H.264 編碼器時自動改存 WebM）
 ├─ pomelo-lock             螢幕鎖定（登入密碼解鎖，透過 PAM）
 ├─ pomelo-notify           通知服務（freedesktop 通知規格，notify-send 等都能用）
 ├─ pomelo-power            電源管理（閒置鎖定 / 關螢幕 / 休眠、休眠前先鎖定、電量警告）
 ├─ pomelo-screensaver      螢幕保護程式（時鐘、照片幻燈片、小柚、流動色彩、星空、氣泡、文字、純黑；閒置時由 pomelo-power 啟動、結束就消失，不常駐）
 └─ pomelo-system           系統設定（一般〔關於、軟體更新、儲存空間、日期與時間、語言與地區、
                             裝置管理〕、外觀、背景圖片、桌面與 Dock、選單列、顯示器、搜尋、應用程式選單、
                             通知、聲音、鍵盤、滑鼠與觸控板、視訊、螢幕使用時間、使用者與群組、登入畫面、
                             印表機與掃描器、網路、藍牙、電源）
pomelo-session              工作階段        相依 pomelo-wm、pomelo-panel、dbus
pomelo-dm                   登入管理員（取代 GDM／LightDM）：只靠 libc＋libpam，常駐約 2 MB   相依 pomelo-greeter、pomelo-compositor
 └─ pomelo-greeter          登入畫面（大時鐘、頭像、睡眠／重新開機／關機），以沒有權限的帳號執行，
                             登入後就結束；安裝後不會自動換掉原本的登入畫面，詳見 docs/登入管理員.md

pomelo-shell    中繼套件：wm + panel + session + control-center + lock + notify + power + storage + polkit + keyring（桌面外殼）
pomelo-desktop  中繼套件：pomelo-shell + 所有應用程式與設定
```

選單列右側是狀態列：系統匣（背景程式的圖示，傳統的 XEmbed 與新式的 StatusNotifierItem／AppIndicator 都支援）、網路、音量（點一下開音量面板、滾輪可調）、電池、控制中心、時鐘，
有外接裝置時會出現 ⏏（開啟、退出），選單列最右邊的 ⏻ 是系統選單（使用者、電量、鎖定螢幕、睡眠、登出、重新開機、關機）。
Dock 最右邊是垃圾桶（有東西時圖示會變滿，右鍵可以清空）。
點 Dock 圖示會切換到已開的視窗；要再開一個（例如第二個終端機）：Ctrl 或 Shift＋點圖示、中鍵點圖示、
圖示右鍵選「新視窗」、選單列左上角程式名稱選單的「新視窗」，終端機裡也可以按 Ctrl+Shift+N。

元件之間只透過標準協定溝通（EWMH、XDG），所以 pomelo-panel 也能搭配其他視窗管理器，
pomelo-wm 也能搭配其他面板。

## 不會裝上 GNOME

Pomelo 不相依、也不建議安裝任何 GNOME 元件（gnome-keyring、policykit-1-gnome、zenity…）：
密碼視窗用自己的 pomelo-polkit、鑰匙圈用自己的 pomelo-keyring；輸入法、印表機（CUPS）、掃描器、額外驅動程式都是在設定裡
按「安裝」時才裝，而且加上 --no-install-recommends，不會連帶裝一堆東西。
在乾淨的 Ubuntu 24.04 上 `sudo apt install ./*.deb` 約 584 個套件（含 Xorg、PipeWire、NetworkManager、FreeRDP），
`--no-install-recommends` 約 340 個；對照 ubuntu-desktop-minimal 約 1270 個、xfce4 約 930 個。
計算機、看圖、影音播放器三個程式加起來只多 4 個小函式庫（libgif7、libwebp7、libwebpdemux2、libsharpyuv0）；
換成現成的 ristretto＋galculator＋audacious 要多 162 個。

## 資源用量（實測，PSS）

| 元件 | 記憶體 |
|---|---|
| pomelo（視窗管理器） | 約 9 MB |
| pomelo-panel | 約 5 MB |
| pomelo-session | 約 0.4 MB |
| pomelo-terminal | 約 3.5 MB |
| pomelo-files | 約 7.5 MB |
| pomelo-theme | 約 9 MB |
| pomelo-calc | 約 4 MB |
| pomelo-calendar（視窗） | 約 14 MB（月檢視、三個行事曆） |
| pomelo-calendar --daemon（背景提醒） | 約 1.2 MB |
| pomelo-viewer | 約 4 MB＋圖片本身（1200 萬像素的照片約 48 MB，下一張會先準備好） |
| pomelo-paint | 約 23 MB（1200×800 的圖）；大圖每 100 萬像素約 4～8 MB（圖本身＋畫的時候的備份），6000×4000 的照片約 240 MB；復原紀錄最多 400 MB |
| pomelo-player | 約 9 MB（播 720p 影片時另有 ffmpeg 解碼，約 35% 的一個 CPU 核心） |
| pomelo-pdf | 約 20 MB（兩頁的文件）；翻閱時會快取畫好的頁面，最多約 256 MB |
| pomelo-remote | 約 74 MB（連線畫面 1280×720）～113 MB（1920×1080），跟著遠端桌面的解析度變 |
| pomelo-writer | 約 12 MB（一頁的文件），跟著頁數與圖片增加；復原紀錄最多約 50 MB |
| pomelo-sheet | 約 26 MB（一張預設大小的空白工作表），跟著儲存格數量與工作表數增加；復原紀錄最多約 16 MB |
| pomelo-slides | 約 14 MB（一張空白投影片），跟著投影片張數與物件（尤其是圖片）增加；圖片不常駐快取，每次畫面重繪才解碼；復原紀錄最多約 16 MB |

閒置時 CPU 0%：全部是事件驅動，面板只在整分鐘醒來更新時鐘。

## 安裝

支援的發行版（64 位元 x86；Ubuntu 26.04 另有 arm64 版）：

| 發行版 | 套件 | 安裝 |
|---|---|---|
| Ubuntu 24.04、26.04 | .deb（版本例如 0.33+ubuntu24.04） | `sudo apt install ./*.deb` |
| Ubuntu 26.04 arm64（樹莓派 5、Ampere 等 aarch64 機器） | .deb（0.42+ubuntu26.04，_arm64） | 同上 |
| Fedora 44、45 | .rpm（0.42-1.fc44、0.42-1.fc45） | `sudo dnf install ./*.rpm` |
| Rocky Linux 9.8（AlmaLinux 9、RHEL 9） | .rpm（0.42-1.el9） | 先 `sudo dnf install epel-release && sudo dnf config-manager --set-enabled crb`，再 `sudo dnf install ./*.rpm` |

每個發行版的安裝包都附有自己的「使用手冊.md」（由 `docs/使用手冊.md.in` 產生）。

### 建置套件（用 Docker，不需要在本機裝開發套件）

```sh
make dist                                 # 六個發行版一起建：dist/<發行版>/ 與 pomelo-<版本>-<發行版>-<架構>.tar.gz
./packaging/build-all.sh fedora44         # 只建其中幾個
./packaging/build-all.sh ubuntu26.04-arm64  # arm64 版：x86 主機上用 qemu-user 模擬編譯（要先註冊 binfmt，見腳本開頭說明；約 30 分鐘）
```

- 程式在執行時判斷用 apt 還是 dnf（libpomelo 的 `pm_pkg_*`），同一份程式碼在每個發行版都能用；
  程式裡一律寫 Debian 的套件名稱，dnf 系統上照 `pm_pkg_name()` 換成那邊的名稱。
- `packaging/build-deb.sh`、`packaging/build-rpm.sh` 讀同一份 `packaging/packages.txt`；
  .rpm 的相依名稱照 `packaging/rpm-names.txt` 換，PAM 設定用 Red Hat 的寫法（`*.pam.redhat`），
  安裝腳本在 `packaging/scripts-rpm/`（pomelo-dm 會用 semanage 標成 xdm_exec_t）。
- Rocky 9 沒有 xcb-imdkit：建置映像從原始碼編譯，跟著 pomelo-input 裝在 `/usr/lib64/pomelo-input/`（不對外宣稱提供）。

只要部分元件也可以，例如只裝終端機：`sudo apt install ./libpomelo0_*.deb ./pomelo-terminal_*.deb`

### 直接安裝到 /usr/local

```sh
make && sudo make install && sudo ldconfig
```

安裝後在登入畫面選「Pomelo」；或在 `~/.xinitrc` 寫 `exec pomelo-session`。

### Pomelo 登入畫面（選用）

裝了 pomelo-dm 與 pomelo-greeter 之後，原本的登入畫面（GDM 等）不會被換掉：

```sh
sudo pomelo-dm-switch try    # 先試用：Ctrl+Alt+F7 開一個 Pomelo 登入畫面，不改設定；Ctrl+Alt+F1 回原本的
sudo pomelo-dm-switch on     # 之後開機都用 Pomelo 登入畫面
sudo pomelo-dm-switch off    # 換回原本的
```

萬一登不進去：Ctrl+Alt+F3 文字登入後執行 `sudo pomelo-dm-switch off` 再重新開機。
登入畫面的背景、時鐘、文字大小、使用者清單、訊息、自動登入等，在「系統設定 → 登入畫面」調整（可以先預覽），
設定存在 /etc/pomelo/dm.conf。

### 應用程式選單

「系統設定 → 應用程式選單」或啟動器圖示的右鍵「編輯…」：改程式的名稱、說明、指令、圖示、分類，
從啟動器與搜尋隱藏，或加入自己的程式／指令。修改存成 ~/.local/share/applications 裡的副本
（標準的 XDG 做法，只影響自己），「還原」就回到安裝時的樣子；Dock、啟動器、搜尋立刻更新。

### 左上角的 Pomelo 選單

「系統設定 → 選單列」或在左上角圖示上按右鍵「編輯選單…」：可以加入程式、自己的指令（用 sh 執行，
例如 `pomelo-open https://www.cwa.gov.tw`）、內建項目（關於、系統設定、鎖定、睡眠、關機…）與分隔線，
也可以改名稱、上下移動、移除，「還原」回到預設選單。存在 ~/.config/pomelo/menu.conf
（一行一項：`種類<Tab>內容<Tab>名稱`，種類是 item／app／cmd／sep），下次點開選單就生效。

### 先試用

在現有桌面中開一個巢狀視窗試用整個桌面（不會啟動背景服務，不影響原本的桌面）：

```sh
make test
```

## 設定

大部分設定用設定程式（pomelo-theme 等）調整即可，改完所有程式立即套用。
設定存在 `~/.config/pomelo/settings.conf`，也可以手動編輯：

| 鍵 | 值 | 說明 |
|---|---|---|
| `appearance.dark` | 0 / 1 | 深色模式 |
| `appearance.accent` | `#rrggbb` | 強調色 |
| `appearance.font` / `appearance.font_size` | 字型名稱 / 數字 | 介面字型 |
| `appearance.icon_theme` | 主題名稱 | 圖示主題 |
| `appearance.compositor` | 0 / 1 | 只給舊的 X11 路徑（pomelo-wm）用：視窗陰影與動畫（pomelo-wm 啟動 picom，設定檔 `~/.config/pomelo/picom.conf` 或 `/usr/share/pomelo/picom.conf`；已經有別的合成器就不啟動）。pomelo-compositor 的陰影與動畫是內建的、一直開著，不看這個設定 |
| `appearance.buttons_right` | 1 / 0 | 視窗的關閉、縮小、放大按鈕放右邊（預設，由左到右：縮小、放大、關閉）或左邊 |
| `desktop.wallpaper` | `gradient` / `none` / 圖片路徑 | 桌布（`none` = 交給 feh 等工具） |
| `desktop.wallpaper_mode` | 0 填滿 / 1 完整顯示 / 2 延展 | |
| `session.wayland` | 1 / 0 | 用 Pomelo 登入畫面時，GTK、Firefox 等程式直接用原生 Wayland（預設，0.40 起）；0 = 全部走 XWayland（0.36 的方式），重新登入後生效 |
| `dock.show` / `dock.autohide` / `dock.size` | 0/1、0/1、32–96 | Dock |
| `bar.clock_format` | strftime 格式 | 選單列時鐘 |
| `terminal.theme` | `system` / `dark` / `light` | 終端機配色 |
| `terminal.font_size` | 數字 | 終端機字型大小 |
| `files.view` / `files.show_hidden` | 0 圖示、1 列表 / 0/1 | 檔案管理員預設 |
| `pdf.timestamp` / `pdf.tsa_url` | 0/1 / `http://…` | PDF 數位簽章加上時間戳記 / 時間戳記伺服器（預設 `http://timestamp.digicert.com`） |
| `pdf.pkcs11_module` | .so 路徑 | 讀卡機的 PKCS#11 模組（通常會自己找到：HiCOS、OpenSC、p11-kit 登記的） |
| `region.lang` / `region.formats` | 例如 `zh_TW.UTF-8`、`en_US.UTF-8` | 介面語言 / 日期與數字格式（重新登入後生效） |
| `input.framework` | `auto` / `pomelo` / `fcitx5` / `ibus` / `none` | 輸入法框架 |
| `input.engines` / `input.engine` | 例如 `chewing,rime-pinyin,anthy` / `anthy` | Pomelo 輸入法啟用的輸入法與目前的輸入法（其他鍵見 docs/輸入法.md） |
| `input.layout` | `us`、`jp`… | 鍵盤配置 |
| `power.lock_after` / `power.screen_off_after` | 分鐘，0 = 從不 | 閒置後鎖定 / 關閉螢幕（預設 10 / 15） |
| `power.suspend_after` / `power.suspend_after_battery` | 分鐘，0 = 從不 | 閒置後休眠：接電源 / 用電池（預設 0 / 30） |
| `notify.dnd` | 0 / 1 | 勿擾模式（只顯示緊急通知） |
| `display.<輸出>` | 由顯示器設定寫入 | 登入時自動套用的解析度與排列 |
| `dock.apps` | .desktop 檔名，逗號分隔 | Dock 固定的程式（在 Dock / 啟動器按右鍵調整） |

快捷鍵、Dock 固定的程式等在各元件的 `config.h`、`panel-config.h` 裡，修改後重新編譯。

開機自動執行：`~/.config/autostart/*.desktop`（標準格式）或可執行檔 `~/.config/pomelo/autostart`。

## 快捷鍵（Super = Win 鍵，相當於 ⌘）

| 按鍵 | 功能 |
|---|---|
| Super+Space / Super+D | 搜尋（應用程式、指令，可打中文） |
| Super+Tab / Super+E / Ctrl+Alt+↑ | 桌面總覽（所有桌面與視窗） |
| Ctrl+Alt+← / → | 切到前一個 / 下一個桌面 |
| Super+1～9 | 跳到第 N 個桌面 |
| Super+Alt+1～9 | 把目前的視窗移到第 N 個桌面 |
| Ctrl+Alt+Shift+← / → | 把目前的視窗移到前一個 / 下一個桌面 |
| 觸控板三指／四指左右滑 | 切換桌面（跟著手指；三指往上滑開桌面總覽） |
| Super+Return | 終端機 |
| Super+Shift+Space | 叫小幫手小柚 |
| Super+Shift+V | 跟小柚用說的（說完自動送出） |
| Super+Q / Super+W | 關閉視窗 |
| Super+M / Super+H | 縮到最小 |
| Super+F / Super+↑、Super+↓ | 縮放 / 還原 |
| Super+← / Super+→ | 貼齊左半 / 右半邊 |
| Super+Ctrl+F | 全螢幕 |
| Alt+Tab | 切換視窗（預設只在目前的桌面，設定可改） |
| 音量鍵 | 調整音量（pactl） |
| Super+A | 啟動器 |
| Super+L / Super+Ctrl+Q | 鎖定螢幕 |
| Ctrl+Space | 切換輸入法／英文 |
| Ctrl+Shift+Space | 換下一個輸入法（Pomelo 輸入法） |
| Super+Shift+R / Super+Shift+Q | 重新啟動 Pomelo / 登出 |
| Super+Shift+3 / Print | 截取整個螢幕（存到「圖片」，右下角出現縮圖） |
| Super+Shift+4 / Shift+Print | 拖曳選取範圍截圖；按空白鍵改成點選視窗 |
| Alt+Print | 截取目前的視窗 |
| Super+Shift+5 | 截圖與錄影工具列（整個螢幕、視窗、選取範圍、錄製整個螢幕、錄製視窗、錄製選取範圍、選項） |
| 以上再加 Ctrl | 只拷貝到剪貼簿，不存檔 |
| Super+Ctrl+Esc | 停止錄影（也可以點選單列上紅色的計時；在計時上按右鍵可以暫停、繼續、放棄） |

終端機：Super+C/V 複製貼上、Super+N 新視窗、Super+K 清除、Super+加號/減號 字型大小。
分頁：Ctrl+Shift+T（或 Super+T）開新分頁、Ctrl+Shift+W 關閉分頁、Ctrl+Tab／Ctrl+PageUp／PageDown 切換、
Super+1～9 跳到第幾個分頁；也可以點分頁列的「＋」「×」，中鍵點分頁關閉，在分頁列上捲動滾輪切換。
檔案管理員：Ctrl(或 Super)+C/X/V 拷貝剪下貼上、F2 重新命名、Delete 丟到垃圾桶、
Ctrl+L 前往路徑、Ctrl+F 搜尋、Ctrl+H 顯示隱藏檔、Ctrl+1/2 切換檢視。

## 語言

介面原文是繁體中文，另有英文、簡體中文（zh_CN，新加坡 zh_SG 共用）、日文、韓文、法文、德文、西班牙文、義大利文、巴西葡萄牙文、俄文、越南文、泰文、印尼文翻譯。翻譯檔在 `libpomelo/i18n/<語言>.tsv`，
每行「原文<Tab>譯文」，安裝到 `/usr/share/pomelo/i18n/`。

- 切換語言：「語言與輸入法」設定，或在 settings.conf 設 `region.lang`，重新登入後生效
- 泰文：Noto Sans CJK 沒有泰文字形，libpomelo 的 `textfb.c` 會讓含泰文的字串自動換用泰文字型（Noto Sans Thai，其次 Noto Sans Thai UI、Loma、Garuda…，用 cairo 實測有字形的第一個）；套件 `pomelo-desktop` 建議安裝 `fonts-tlwg-loma-ttf`（Debian／Ubuntu）或 `google-noto-sans-thai-fonts`（Fedora／Rocky）。限制：cairo 的文字 API 沒有 OpenType 排版，聲調符號疊放略不如完整排版
- 新增語言：複製 `en.tsv` 成例如 `ja.tsv` 並翻譯；`python3 libpomelo/i18n/check.py libpomelo/i18n/ja.tsv`
  會列出還沒翻譯的句子
- 程式中要翻譯的文字用 `_("…")` 標記（靜態陣列裡用 `N_("…")`，顯示時再翻譯）

## 開發

```
libpomelo/              共用函式庫（pkg-config 名稱：pomelo）
pomelo-*/               各元件，每個資料夾有自己的 Makefile，可單獨編譯：
                        make -C pomelo-files（需先安裝 libpomelo-dev）
packaging/packages.txt  套件清單與相依關係
packaging/build-deb.sh  打包腳本（.deb）；build-rpm.sh（.rpm）；build-all.sh（用 Docker 建所有發行版）
```

## 限制

- Pomelo 的程式還沒全部原生 Wayland：用 Pomelo 登入畫面時，13 個設定程式、控制中心、通知、啟動器、鎖定畫面、選單列、Dock、桌布、小柚、登入畫面直接用 Wayland（0.40 起），檔案管理員、終端機等還跑在 pomelo-compositor 內建的 XWayland 上；GTK、Firefox 這類程式 0.40 起直接用原生 Wayland（後續計畫見 docs/Wayland轉移計畫.md）
- 網路設定連線一般的 WPA-PSK Wi-Fi 時，密碼會出現在 nmcli 的指令參數中（同一台電腦的其他使用者用 `ps` 看得到）；VPN 與企業 Wi-Fi 的密碼走標準輸入／0600 暫存檔，不在命令列
- 網路設定的 VPN 只支援 WireGuard 與 OpenVPN（沒有 L2TP、PPTP、IPsec）；OpenVPN 密碼有逗號或反斜線時不能記住；把檔案拖進視窗只有 X11 能用
- X11 工作階段（GDM 等選 Pomelo）的螢幕鎖定和所有 X11 鎖定程式一樣：如果鎖定程式本身當掉，螢幕會被解開（用 Pomelo 登入畫面時由 pomelo-compositor 保證，鎖定程式當掉畫面也維持鎖住）

## 授權

Copyright © 2026 pake &lt;pake@fedora.tw&gt;

- 開發者：pake@fedora.tw
- 組織：<https://fedora.tw>

Pomelo 是自由軟體，以 GNU 通用公眾授權條款第 3 版或更新的版本（GPL-3.0-or-later）授權，全文見 [LICENSE](LICENSE)。
可以自由使用、修改、散布；散布修改過的版本時，也要用同樣的授權並提供原始碼。本軟體不提供任何擔保。

每個原始碼檔開頭都有 `SPDX-License-Identifier` 標示。例外：`pomelo-pdf/pdf-tables.c` 的字形名稱對照表取自
Adobe Glyph List（BSD-3-Clause，全文見 [pomelo-pdf/AGL-LICENSE.txt](pomelo-pdf/AGL-LICENSE.txt)）。
Rocky Linux／RHEL 9 的 pomelo-input 套件附了一份沒有修改的 xcb-imdkit 1.0.9（LGPL-2.1-only），授權與原始碼在
[packaging/third-party](packaging/third-party)。

`packaging/build-all.sh` 除了各發行版的安裝包，也會產生原始碼包 `pomelo-<版本>-source.tar.gz`；
散布安裝包時請把原始碼包放在一起（GPL 的要求）。
