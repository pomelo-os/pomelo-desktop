# Pomelo 轉移到原生 Wayland 的計畫（草案）

2026-11-02 擬定。目的：Pomelo 自己的程式與第三方程式（Firefox、Chromium、GTK、Qt、Electron）都直接用 Wayland，
不再全部經過 XWayland；XWayland 留著給還沒有 Wayland 版本的第三方程式（Wine、舊 Electron、Java Swing…）。

## 進度

- **0.40（2026-11-03）：階段 1 完成。** compositor 支援 xdg-shell、外框協定、輸入法（text-input-v3／input-method-v2，
  pomelo-input 同時當 input-method 用戶端）、滑鼠鎖定、快捷鍵交給程式、剪貼簿、要求到前景等；工作階段給程式
  `WAYLAND_DISPLAY`（socket 開在 `/run/user/UID`）。為了讓還是 X11 的桌面外殼看得到原生視窗，compositor 在 XWayland 那邊
  幫每個原生視窗放一個替身（`_POMELO_WL_CLIENTS`），並把原生程式的輸入算進 XWayland 的閒置時間——這兩件事在階段 3
  面板改成原生（foreign-toplevel、ext-idle-notify）之後就可以拿掉。
  後來補齊：layer-shell（第三方面板、通知、slurp）、foreign-toplevel、security-context＋全域篩選（沙箱看不到特權協定、
  XWayland 專用的只給 XWayland）、Qt 原生（IBus 的相對座標加上 compositor 給的視窗位置 `_POMELO_WL_ORIGIN`；
  pomelo-input 另外寫一份照 WAYLAND_DISPLAY 命名的 IBus 位址檔）、螢幕分享（xdg-desktop-portal-wlr＋pomelo-portals.conf）。
  **刻意不做**：Electron／Chromium 維持 XWayland（它們原生模式的輸入法用 text-input-v1，wlroots 0.17 沒有、改了會打不了中文）；
  分數縮放要等 Pomelo 自己的程式變原生（現在調整輸出縮放會讓所有 X 程式變糊）。

- **0.40（2026-11-03）：階段 2 完成＋階段 3 第 1～6 步。** 十三個設定程式、檔案對話窗、剪貼簿原生；控制中心、通知、
  啟動器、鎖定畫面（compositor 保證）、截圖錄影、關螢幕、顯示器設定改成走 Wayland。階段 3 第 7～9 步（選單列與 Dock、
  桌布、小柚、登入畫面）放到 下一版。
- **第 7 步完成（2026-11-08）**：pomelo-panel 的原生後端 panel-wl.c（選單列、Dock、感應區、提示框、桌布都是 layer 視窗；
  視窗清單走 wlr-foreign-toplevel；XEmbed 系統匣原生做不到，只留 SNI；X 上程式的狀態仍透過 X 根視窗屬性）。headless＋WLR_HEADLESS_OUTPUTS=2 測過雙螢幕。
- **第 9 步完成（2026-11-08）**：pomelo-greeter 原生（draw_to(cr) 共用、每個螢幕一張 overlay layer 視窗 translate 到全域座標、主螢幕 kb=1；
  --preview 在 headless 雙螢幕測過密碼錯誤與登入成功）。第 8 步也完成（2026-11-08）：pet.c 原生路徑（pet_native；wl_pet_show 邊距 defer＋pmwl_input_mask 形狀點擊；泡泡 on_demand 鍵盤）。
  階段 3 的 7～9 步都完成；xbridge 的 X 替身與 _POMELO_WL_* 屬性等 pomelo-input、小柚的工具執行緒也不用 X 時再拆。

- **階段 2（2026-11-03 開工）做法**：libpomelo 加 wl.c（Wayland 用戶端：xdg_toplevel＋伺服器端外框、wl_shm 雙緩衝、
  xkbcommon 鍵盤與重複鍵、cursor-shape 游標、text-input-v3 輸入法、xdg_popup 選單、inotify 監看 settings.conf），
  pm_ui_run／pm_ask／pm_ui_choice 的選單在原生模式改用它；X11 的程式碼路徑不動。**程式逐一開啟**：在 main() 設
  `pm_ui_native = 1` 的程式（確認 build() 裡沒有直接呼叫 X 的）才走 Wayland，其他照舊 X11；沒有 WAYLAND_DISPLAY、
  或 `POMELO_BACKEND=x11` 時也走 X11。pm_conf_set 照樣通知 X 程式（沒有 X 連線時臨時開一條）。
  第一批：pomelo-docker、pomelo-language、pomelo-monitor、pomelo-network、pomelo-sound、pomelo-storage、pomelo-theme
  （0 處 X 呼叫）；之後是檔案對話窗、pomelo-bluetooth、pomelo-software 等。彈出式（控制中心）要 layer-shell，留到階段 3。
- **階段 2 第一部分完成（2026-11-03，併入 0.40）**：wl.c（約 600 行）＋widgets.c 的原生迴圈 `ui_run_wl`；第一批七個程式
  已設 `pm_ui_native = 1`，在 headless compositor 逐一截圖確認是原生（程式只有一個 Wayland socket，X 那邊只有
  compositor 放的 1×1 替身）。測過：點擊、開關、滑桿拖曳、分段、下拉選單（滑鼠、↑↓ Enter、點外面關閉）、輸入框
  （大寫、Backspace 按住自動重複）、密碼欄不經過輸入法、注音（組字窗在游標下方、Enter 送出中文）、滾輪、改視窗大小、
  標題列關閉鈕、設定改了即時套用（深色模式）、沒有 WAYLAND_DISPLAY／`POMELO_BACKEND=x11` 退回 X11。
  資源：原生版固定兩張 wl_shm 緩衝（跟 compositor 共用，不複製），frame() 要再畫一遍時中間那遍不送出。
  **坑**：鍵盤全部拔掉時 compositor 拿掉 seat 的鍵盤能力，舊的 wl_keyboard 就廢了，要 release、能力回來時重拿
  （測試環境沒有實體鍵盤，每個 wtype 結束都會這樣）；fork 的子程序 `close(ConnectionNumber(dpy))` 要先判斷 dpy；
  build() 裡間接碰 X 的 libpomelo 函式（pm_under_compositor、pm_logout）改成沒有連線時臨時開一條。
  libpomelo 新增相依 libwayland-client0、libxkbcommon0（pomelo-input 本來就要）。
- **階段 2 第二部分（2026-11-03）**：第二批 pomelo-bluetooth、pomelo-software、pomelo-system（`--about` 小視窗另外走 X11）；
  新 API `pm_ui_menu(u, m, n)`（在剛點的位置開選單，X 與原生都能用，取代程式裡的 XQueryPointer＋pm_popup）；
  檔案對話窗原生版（fd_draw／fd_click／fd_key 兩種後端共用，fd_run_x／fd_run_wl 各自的事件迴圈；裡面的 pm_ask
  掛在對話窗底下）；compositor 每個原生視窗多一個 popup_tree（外框最上層），選單不會被下方圓角的細條蓋住；
  pmwl_menu 打開選單那一下的放開不算選取（在選單上按過、或按住超過 0.3 秒拖過去才算）。
- **階段 2 第三部分（2026-11-03）**：pomelo-vm、pomelo-remote、pomelo-keyring 的管理視窗開原生（畫面視窗 console.c、連線畫面
  session.c、密碼提示窗都在另外的程序或模式，照舊 X11）。剪貼簿：wl.c 接 wl_data_device（只收發文字；自己放的直接給、
  不等自己回應；別的程式最多等一秒），widgets.c 的 X11 版自己回應 SelectionRequest；輸入框 Ctrl+C／V／X、Shift+Insert、
  右鍵選單（剪下、複製、貼上、全部清除；密碼欄不能複製），檔案對話窗名稱可以貼上。測過原生↔原生、原生→X、X→原生、
  X11 版貼上與被貼上。
  **還沒做**：HiDPI（compositor 還沒有輸出縮放的設定，要跟 X 程式在高解析度下怎麼顯示一起決定，放到階段 3 顯示器設定）、
  pomelo-display、控制中心、截圖、小柚（要 layer-shell／output-management，階段 3）；拖放（pm_ui 沒有用到）；
  根視窗屬性換成 socket／inotify（設定改了的通知已經有 inotify，面板動作、單一實例、輸入法狀態跟著階段 3 的外殼一起換）。

- **階段 3（2026-11-03 開工）順序**：compositor 已有 layer-shell、foreign-toplevel、ext-idle-notify、screencopy、xdg-output；
  還沒有 ext-session-lock、output-management（做到鎖定畫面、顯示器設定時再加）。由小到大、先打基礎：
  1. libpomelo（wl.c）：layer-shell 視窗（pmwl_layer：哪一層、貼哪幾邊、邊距、保留空間、鍵盤）、xdg-output 取得每個螢幕的
     位置與大小（X 傳來的全域座標換成某個螢幕裡的邊距）、layer 上的 xdg_popup（選單）。
  2. 控制中心（pm_ui_popover）：overlay 層、貼右上角；點外面關閉用一張全螢幕透明的 overlay 墊在下面接點擊。
  3. 通知（pomelo-notify）：overlay、不拿鍵盤。4. 啟動器、搜尋、總覽：overlay、獨佔鍵盤。
  5. 鎖定畫面：compositor 加 ext-session-lock，pomelo-lock 改用它。6. 電源（ext-idle-notify）、截圖（screencopy）、
     顯示器（compositor 加 output-management）。7. 選單列與 Dock（top／bottom 層＋foreign-toplevel）、桌布（background 層）。
  8. 小柚（overlay＋輸入區域）。9. 登入畫面。每一步 X11 工作階段照舊；沒有 layer-shell 的環境退回 X11。
  **進度（2026-11-03）**：1. 完成——公開標頭 pomelo-wl.h（pmwl_*：一般視窗、layer 視窗、邊距／大小、輸入區域、
  pmwl_clear 用 viewporter 把 1×1 透明點放大、xdg-output 的螢幕清單、pmwl_watch_fd 讓 D-Bus 等外部 fd 一起等）。
  2. 控制中心完成（pm_ui_popover 在有 layer-shell 時走原生）。3. 通知完成（鎖定狀態暫時還讀 X 的 _POMELO_LOCKED，
  等第 5 步鎖定畫面原生後改）。4. 啟動器完成（Wayland 工作階段的「搜尋」就是啟動器；總覽是 compositor 自己畫的）：
  overlay 貼滿游標所在的螢幕（layer 不指定螢幕時 compositor 放在游標那個），模糊背景改用 libpomelo 新的
  pm_desktop_image（桌布或 Pomelo 漸層，選單列的桌布也改用它），切換用 XDG_RUNTIME_DIR 的 PID 檔。
  5. 鎖定畫面完成：compositor 的 lock.c（ext-session-lock；先蓋底色再回 locked；鎖定中鍵盤滑鼠直接送給鎖定畫面、
  別處給焦點一律擋、每格畫面前把鎖定那層拉回最上面；鎖定程式沒解鎖就不見時維持鎖住並重開，一分鐘五次後改成
  一分鐘再試或按鍵重開；_POMELO_LOCKED 由 compositor 寫；協定列為特權）；libpomelo 的 pmwl_lock／pmwl_lock_surface／
  pmwl_unlock＋PMW_OUTPUTS；pomelo-lock 原生時每個螢幕一個鎖定畫面。測試不輸入真的密碼（scratchpad/locktest 不經過
  PAM 測解鎖流程）。**坑**：沒有鍵盤時拿的 wl_keyboard 收不到 enter，換鍵盤時要強制重新 enter，不然密碼打不進去
  （一般路徑也一樣，seat.c 的 use_keyboard）。
  6. 截圖、錄影、電源、顯示器完成：libpomelo 的 pmwl_capture（wlr-screencopy，可以只抓一塊）；compositor 在根視窗寫
  _POMELO_WL_STACK（一般視窗由上往下的範圍，0.15 秒合併寫一次）給截圖點選視窗；錄影在 Wayland 工作階段每 1/30 秒
  拿一格從 ffmpeg 標準輸入餵（rawvideo＋use_wallclock_as_timestamps，停止＝關管線＋SIGINT）；compositor 的 outmgr.c
  加 wlr-output-power-management（關掉的螢幕排列不變，有輸入就自己亮）與 wlr-output-management（先 test 整組再 apply，
  重新打開的螢幕要重新接回 scene 的 layout）；pomelo-power 用 pmwl_screens_power 關螢幕（閒置偵測照舊用 X 的 IDLETIME，
  compositor 會幫原生輸入歸零）；pomelo-display 讀寫改用 pmwl_heads（Wayland 沒有主要顯示器：最左上的當主要；鏡像改成疊在
  同一位置），原生視窗；pomelo-session 在 Wayland 工作階段也跑 --apply；螢幕範圍變了時跑到外面的視窗搬回來。
  **還沒做**：插上新螢幕時套用記住的設定（X11 是 pomelo-wm 呼叫 --hotplug；compositor 目前用預設模式自動排在右邊）。
  **坑**：外部 fd 可讀時要先讀掉再收下一個事件，不然 pmwl_next 一直回傳 PMW_FD 空轉；
  測通知要用私有 D-Bus（使用者的工作階段已經有通知服務），abstract socket 名稱每次換一個（舊的匯流排還在會綁不上）。

- **階段 4 第 2 步完成（2026-11-20）：A 批、B 批程式已原生**（pomelo-calc、pomelo-edit、pomelo-viewer、pomelo-system --about、pomelo-paint、
  pomelo-writer、pomelo-sheet、pomelo-slides、pomelo-pdf）。做法：同一份程式碼雙後端——main() 用 `pmwl_connect()` 決定原生或 X（POMELO_BACKEND=x11 可強制 X），
  原生時設 `pm_ui_native = 1`、不開 X 連線；事件處理函式不動，原生事件用 libpomelo 新的 `pmwl_to_xevent` 轉成 XEvent 再交給同一個 switch，
  鍵盤用 `pmwl_xkey`（原生）或 XLookupString／XIC（X）。每個程式的詳細與測試見 CHANGELOG 0.42「階段 4 第 2 步」與 `docs/Wayland階段4缺口清單.md`。
  順便修好的共用問題：pmwl_begin 讀大小的時機（離開全螢幕被舊大小拉回）、popup 用 Esc 關掉後鍵盤焦點沒還（compositor xdg.c）、文字編輯器存檔後路徑變空。
  **還沒做**：D 批（截圖、polkit…）、E 批（影片、虛擬機器、遠端桌面）。（之後已完成，見第 4、5 步。）
- **階段 4 第 3 步完成（2026-11-20）：C 批——檔案管理員 pomelo-files、終端機 pomelo-terminal 已原生（雙後端）。**
  - 檔案管理員：同樣的 `pmwl_connect()`＋`pmwl_to_xevent`／`pmwl_xkey` 做法；X 後端的 XDND（自己的來源＋目標）一行沒動，原生走 `pmwl_drag_start_files`／`pmwl_dnd_target`。
    原生拖放的流程：拖進來 ENTER 就用 `pmwl_dnd_files` 讀檔案清單（拖的當中就能判斷「放進自己裡面」「本來就在這裡」），MOTION 用 `pmwl_dnd_accept`（能放才接受，偏好動作＝同磁碟移動、
    不同磁碟拷貝，最後動作以放下時協商的為準），DROP 後做事，`PMW_DND_END` 結束我們這邊的拖曳狀態（拖的當中來源視窗收不到滑鼠事件）。剪貼簿檔案清單用 `pmwl_clip_set_types`／`pmwl_clip_get_type`（自己放的也讀得回來）。
  - 終端機：**Xft → cairo 的差異**：X 後端仍用 Xft（`XftDrawGlyphFontSpec`）；原生後端用 `cairo_show_glyphs`＋`cairo_ft_font_face_create_for_pattern`（同一份 fontconfig 比對結果），畫在自己的
    影像緩衝上，每幀整張 `pmwl_end`。程式裡新增兩個繪圖原語 `P_rect`／`P_glyphs`（draw_row、方框字元、游標、底線都走它們），字型挑選 `font_for`（Xft）與 `font_for_native`（cairo）邏輯對稱、
    共用 `missing[]` 與備用字型上限。cairo 的字形編號用 `cairo_scaled_font_text_to_glyphs`（有快取）。格子大小：寬＝「M」的 advance 取整，高＝(ceil ascent＋ceil descent)×行距，跟 Xft 一致，
    但 Xft 用螢幕 dpi（Xvfb 實測 95.7）、原生固定 96 dpi；分頁列的文字用 libpomelo 的 `pm_font`／`pm_text`（Noto Sans CJK，跟其他程式的介面字一樣）而不是 Xft 的 tabfont。**不改 pango**
    （照決定）；CJK 組字沿用「每格一個字」的終端機模型（寬字元兩格），缺字回退沿用現有邏輯，所以沒有 pango 的斷字與連字，跟 X 版相同。
  - 新 libpomelo 能力：只有 `pmwl_next_fds` 的上限 16→48。
  - 測試方法與工具搬進 repo：`libpomelo/tests/wl-apps/`（`setup.sh` 編 vptr 虛擬滑鼠、clipget、dnd-test、xdnd-test；`lib.sh` 開 headless compositor 與 start／snap／click／key／ctrl／dragto／xstart 等函式；
    `restrict-audit.sh`、`restrict-container.sh`、`overlap-heur.py`、`alias-heur.py` 是重疊字串函式的稽核；`pics/` 測試圖）與各程式的 `tests/`（pomelo-files、pomelo-terminal 以及第 2 步的各程式）。
    用法：`POMELO_TEST_DIR=<短路徑> sh libpomelo/tests/wl-apps/setup.sh` 一次，然後 `POMELO_TEST_DIR=… sh pomelo-files/tests/files-test.sh`；截圖放 `$POMELO_TEST_DIR/shots`。
    注意 `POMELO_TEST_DIR` 要短（Unix socket 路徑上限 108 字元）；原生拖到 XWayland 的測試要用有 patch 的 wlroots（`startp`，設 `POMELO_WLROOTS_BUILD`）；中文輸入測試要先編好 pomelo-input（`POMELO_INPUT`）。
    坑：wtype 第一個鍵與 Ctrl 會掉（`ctrl()` 先送一個 Shift 等 0.3 秒）、輸入法剛啟用後要等 1～1.5 秒再送 Ctrl+Space；clipget 會開一個視窗搶走焦點，用完要等 1 秒再送鍵。
  - **還沒做／沒驗證**：見 `docs/Wayland階段4缺口清單.md`「第 C 批完成後」。
- **階段 4 第 4 步完成（2026-11-20）：D 批**——截圖／錄影、密碼認證視窗（pomelo-polkit）、電源管理（pomelo-power）、鑰匙圈密碼提示窗改成原生雙後端（X 後端不變）。
  - 截圖：覆蓋層＝每個螢幕一張 overlay 層全螢幕 layer 視窗（第一張獨佔鍵盤；凍結畫面來自 `pmwl_capture`），工具列、倒數、縮圖也是 layer 視窗；拷貝到剪貼簿用 wl_data_device（`pmwl_clip_owned` 判斷別人接手）；
    錄影狀態與視窗清單兩個根視窗屬性仍臨時連 XWayland（IPC）。
  - polkit／keyring 提示窗：overlay 層＋獨佔鍵盤、不啟用輸入法；polkit 加 `--demo`／`--demo-nonadmin` 假後端供測試（不連 polkit）。
  - power：ext-idle-notify（libpomelo 新增 `pmwl_idle_watch`／`pmwl_idle_clear`／`pmwl_can_idle`、`PMW_IDLE`），原生時不開 X（只有 `_POMELO_LOCKED` 臨時連線）。
  - 其餘混合者（session、panel、小柚、螢幕保護程式）的 X 連線都是 IPC 必要，理由逐一記在缺口清單；pomelo-lock、pomelo-display 已無 X 殘留。
  - 測試與沒驗證的部分：見缺口清單「第 D 批完成後」；測試腳本在 `pomelo-screenshot／pomelo-polkit／pomelo-power／pomelo-keyring/tests/`。
    **測試環境注意**：這台機器的 `/tmp` 現在是 noexec，`POMELO_TEST_DIR` 要放在 home 底下的短路徑（例如 `/home/pake/.p4`）。
- **階段 4 第 5 步完成（2026-11-20）：E 批——影音播放器、虛擬機器畫面視窗、遠端桌面連線視窗改成原生雙後端（X 後端不變）。這時還剩 pomelo-archive 的 gui-browse.c，第 6 步轉完。**
  - libpomelo 新增：`pmwl_cursor_image`（自訂游標圖，來賓游標）、`pmwl_frame_request`／`pmwl_frame_wait`／`PMW_FRAME`（frame callback，影片與整張重畫的視窗對時、不丟格）、`PMW_CLIP`（剪貼簿變了）、
    `pmwl_scancode`（evdev→PC 掃描碼）、`pmwl_fd`、`pmwl_pointer_hint`。HiDPI／fractional-scale 評估後不做（compositor 沒有輸出縮放設定）、xdg-activation 用不到，理由在缺口清單。
  - 播放器：影片本來就不是 X 視窗嵌入（自己的 ffmpeg 管線→cairo 影像），所以不需要 libmpv 或子畫面，整個視窗 `pmwl_begin`／`pmwl_end`；全螢幕、隱藏游標、防止閒置、拖放、精細捲動音量、frame 對時。
    單一視窗與媒體鍵指令改用抽象 socket；選單列的小圖示仍靠 X 根視窗屬性（有 XWayland 才寫，沒有也能播）。
  - 虛擬機器畫面與遠端桌面：既有的局部重畫程式碼不動——畫在一張記憶體影像上，主迴圈整張送出；全螢幕時 `pmwl_shortcuts_inhibit`（Alt+Tab、Super 交給來賓）；來賓游標；
    evdev→掃描碼；剪貼簿同步（遠端桌面 CF_UNICODETEXT；虛擬機器 VNC Latin-1，**只有原生後端有**）；選單與對話窗原生。
  - 稽核：整個樹 grep 之後，Wayland 工作階段仍連 X 的程式與理由整理在缺口清單「階段 4 完成後仍連 X 的程式與理由」：**IPC 必要**（session、panel、小柚、螢幕保護程式、power、截圖、notify、player 選單列圖示、control-center 讀螢幕寬度）、
    **第三方／X 程式需要**（pomelo-input 的 XIM、瀏覽器與其他第三方）、**刻意不改**（pomelo-wm、各程式的 X11 路徑）。（第 6 步補：pomelo-archive 的 gui-browse.c 已轉，見下一項。）
    把「IPC 必要」的 X 屬性換成 socket 或 compositor 協定是階段 5 的前提，不在階段 4。
  - **完成宣告要分清楚驗證到哪裡**：
    - **開發機 headless 測過**（pomelo-compositor headless＋虛擬滑鼠／虛擬鍵盤／grim 截圖，與 X11 後端〔Xvfb〕對照）：播放器（影片與音樂畫面、全螢幕、游標隱藏、進度列、音量精細捲動、拖放、單一視窗交接、frame callback 約每影片格一次、
      防止閒置〔ext-idle-notify 探針〕、選單列狀態屬性）；虛擬機器畫面視窗（對 `fake-qemu.py`：畫面、來賓游標、延伸按鍵掃描碼、滑鼠按鍵與滾輪、工具列選單、全螢幕縮放與浮動工具列、快捷鍵抑制、剪貼簿雙向）；
      遠端桌面（對 `fake-rdp-server`：憑證與登入對話窗、NLA 連線、畫面、游標、掃描碼、滑鼠、全螢幕與快捷鍵抑制、剪貼簿雙向）。X11 後端對同樣的假伺服器各截圖確認事件一致、版面沒變。
    - **實機未驗證**：真的顯示卡與 GPU compositor、真的聲音輸出（開發機的測試環境沒有 PulseAudio）、HiDPI／多螢幕、真的 QEMU（USB 穿透、快照、光碟、儲存狀態、VNC 聲音通道、解析度跟著視窗、帶透明度的游標）、
      真的 Windows／xrdp 伺服器（RemoteFX、GFX 管線、H.264、動態解析度、聲音、磁碟機／印表機對應）、FreeRDP 2（Ubuntu 22.04、Rocky 9）、使用者的真實鍵盤（測試用虛擬鍵盤送標準 evdev 碼）、
      Fedora／Rocky／Debian 的重新建置（這次只在 Ubuntu 24.04 的主機與建置容器編）、重新打包。
    - 觀察到但沒查原因：測試環境裡視窗剛開、游標第一次移進去時偶爾沒有收到 `wl_pointer.enter`（再動一下就有；只在虛擬機器視窗出現過一次）。可能是 compositor 在視窗剛被程式自己改大小時的行為，值得實機留意。
  - 測試與工具：`pomelo-player／pomelo-vm／pomelo-remote/tests/`；新測試工具 `libpomelo/tests/wl-apps/vkbd`（標準 evdev 碼的虛擬鍵盤）、`idleprobe`（ext-idle-notify 探針）；
    假伺服器 `pomelo-remote/tests/fake-rdp-server.c`（FreeRDP 3 server API）與 `pomelo-vm/tests/fake-qemu.py`。主機缺開發檔的程式用 pomelo-build:24.04 容器編（26.04 容器編出來的主機 glibc 跑不起來）。
- **階段 4 第 6 步完成（2026-11-20）：壓縮管理員 pomelo-archive 的瀏覽視窗改成原生雙後端、全面回歸驗證——宣告階段 4 完成。**
  - Pomelo 自有的圖形程式 Wayland 工作階段下都是原生視窗，`DISPLAY` 不設也能開——**（小柚的設定／確認／對話視窗原本是例外，2026-11-20 已改成原生）**。階段 2、3 的程式第 6 步用 `env -u DISPLAY` 在 headless compositor 重測過（結果表在缺口清單「第 6 步回歸」），A～E 批各程式當時就用 `env -u DISPLAY` 測過。
  - 壓縮管理員：瀏覽、選取、雙擊進資料夾、右鍵選單、解壓縮到…、檔案對話窗、密碼視窗、拷貝路徑、拖放進視窗（開啟／加入）、進度視窗；X 版沒有「拖出」，原生也沒有。順手修正加入檔案後檔名變空字串（X 版也有）。
  - **開發機 headless 測過**：壓縮管理員（開窗、導覽、選單、解壓、拖入加入、剪貼簿、資訊、開啟對話窗、密碼視窗；X11 後端開窗、導覽、選單）、看圖（新編版：看圖、縮放、旋轉、拷貝、全螢幕、拖放）、文字編輯器（原生版二進位確認）、
    階段 2、3 的程式在 `DISPLAY` 未設時開窗（截圖看過總覽）。
  - **實機未驗證**：真實 GPU compositor、多螢幕、HiDPI、真實鍵盤與輸入法、長時間使用；用 wlroots 補丁的原生→XWayland 放下需要重編 wlroots 才生效；Fedora／Rocky／Debian／SUSE 的重新建置與打包（只在 Ubuntu 24.04 主機與建置容器編）。
  - **仍連 X 的 IPC 程式列入階段 5 前提**（沒有 XWayland 時）：pomelo-session（會把「沒有 X 視窗管理員」當成登出，未實際跑、靜態檢查；風險評估見階段 5 前提）、
    pomelo-panel 的輸入法／播放器／錄影圖示與 `_POMELO_SPAWN`／使用時間、小柚的單一實例轉交（小柚的設定／確認／對話視窗已在 2026-11-20 改成原生，不再列入）；其餘（screensaver、power、notify、screenshot、player、control-center）沒有 X 照常運作。詳細見缺口清單。
  - 測試腳本：`pomelo-archive/test/test-wl.sh`、`test-x11.sh`；`DISPLAY` 未設回歸腳本在 `/home/pake/.p4/nodisp/`（開發機，未進原始碼樹）。
- **階段 4 進度（2026-11-20）：第 1 步完成（libpomelo 與共用基礎；各程式主體還沒動）。**
  - **拖放**：libpomelo 的 wl.c 接 wl_data_device 的拖放——目標（`pmwl_dnd_target`、事件 `PMW_DND_ENTER／MOTION／LEAVE／DROP`、
    `pmwl_dnd_fetch`／`pmwl_dnd_files`，同一程式內拖放不等自己）與來源（`pmwl_drag_start`、`pmwl_drag_start_files`、拖曳圖示、
    copy／move、`PMW_DND_END`）；`pm_ui_on_drop` 原生也能用、新 `pm_ui_drag_files`。compositor：seat.c 接 request_start_drag
    （指標抓取、拖曳圖示 scene、Esc 取消）；新檔 xdnd.c 補 **X 程式 → 原生 Wayland** 的橋（wlroots 0.17 只有 Wayland → X：
    X 來源拖東西時指標焦點留在它的 X 視窗＝implicit grab，游標底下是原生視窗時把蓋滿螢幕的 XdndAware proxy 拉到最上面，
    XdndEnter／Position／Drop 換成 data_device 的 enter／motion／drop，資料向來源 XConvertSelection）；
    wlroots 的 patch 多修一處（原生 → XWayland 放下後 wlroots 立刻送 XdndLeave、拒絕資料請求，見
    `packaging/docker/wlroots-0.17.1-client-list-no-override-redirect.patch`）。
  - **其他共用缺口**：剪貼簿任何格式、primary selection、15 種游標形狀、視窗屬性（標題、app id、最小／最大、全螢幕、最大化）、
    `PMW_KEYUP`＋evdev 代碼、側鍵、水平與精細捲動、輸入法預編輯與用途、相對滑鼠與快捷鍵抑制、防止閒置、子畫面。舊程式不呼叫
    `pmwl_want` 就收不到新事件，行為不變。
  - **缺口清單與各程式需要什麼、建議轉換順序**：見 `docs/Wayland階段4缺口清單.md`。第 1 步時沒補的共用缺口：自訂游標圖、frame callback、
    HiDPI、ext-idle-notify 用戶端、tablet、xdg-activation（之後 ext-idle-notify 在第 4 步、游標圖與 frame callback 在第 5 步補好；HiDPI 評估後不做、tablet 與 xdg-activation 沒人要求）。
  - **測試**：headless compositor＋虛擬滑鼠，`libpomelo/tests/dnd-test.c`（一個視窗當來源、另一個當目標）與 `xdnd-test.c`（Xlib 的 XDND
    來源／目標）、`misc-test.c`：原生→原生（uri-list 含空格與中文檔名、自訂格式＋move、同程式內、Esc 取消、放在桌面上）、
    原生→XWayland、XWayland→原生、XWayland→XWayland、拖進 pomelo-network 觸發匯入；剪貼簿多格式、primary、精細／水平捲動、
    鎖滑鼠相對移動、子畫面、全螢幕／最大化、標題；X11 後端（Xvfb、POMELO_BACKEND=x11）pomelo-network 截圖確認沒退步。
    **坑**：`wl_proxy_add_listener` 會把 proxy 的 user data 設成第三個參數——offer 的資訊要在 add_listener 之後才 set_user_data；
    wlr_seat_start_drag 會讓 wlroots 的 xwm 搶 XdndSelection，所以 X 來源的拖放不能呼叫它（xdnd.c 手動設 drag->started 與 seat->drag）；
    dnd_finish 之後 wlroots 自己會銷毀來源，不能在 finish 裡銷毀。

## 現況（2026-11-02 擬定時）

- **pomelo-compositor**（wlroots 0.17.1 私有編譯，約 2,300 行）：只管理 XWayland 視窗。xdg-shell 沒有接（所以 GTK4、Qt6、
  Firefox 都只能走 XWayland，工作階段也刻意不設 `WAYLAND_DISPLAY`）；layer-shell 只立了 global；有 screencopy。
- **約 30 個套件直接用 Xlib**，合計約 1,600 處 X 呼叫。設定類程式（外觀、音效、網路、系統設定、儲存空間、虛擬機器、
  容器…約 15 個）大多只透過 libpomelo 的 `pm_ui_run`；終端機、檔案、編輯器、PDF、文書三套、看圖、繪圖、播放器、
  遠端桌面、虛擬機器畫面、截圖、面板、小柚等有自己的 X 事件迴圈。
- **程式之間靠根視窗屬性溝通**：`_POMELO_SETTINGS`（設定改了）、`_POMELO_ACTION`（面板→compositor：登出、總覽、搜尋）、
  `_POMELO_INPUT_*`（輸入法狀態）、`_POMELO_EDIT`／`_POMELO_PDF`／`_POMELO_PLAYER`（單一實例）、`_POMELO_RECORDING`、
  `_POMELO_LOCKED` 等 24 種。Wayland 沒有根視窗，這些都要換掉。
- **輸入法** pomelo-input：XIM 伺服器（X 程式）＋模擬 IBus 協定（Qt、GTK4）。Wayland 原生程式的標準做法是
  compositor 的 input-method-v2 ↔ 程式的 text-input-v3，選字列是 compositor 擺在游標旁的 popup surface。

## 分階段

每個階段做完都可以發行：沒轉過去的程式繼續走 XWayland，不會有「轉一半不能用」的版本。

### 階段 1：compositor 支援原生 Wayland 程式（先做，馬上有感）

- **xdg-shell**：一般視窗與彈出選單，外框、圓角、陰影、焦點、堆疊、最大化、全螢幕、最小化、Dock 叫回、總覽，
  都跟現在的 X 視窗一樣；**xdg-decoration** 讓 GTK／Qt 用 Pomelo 的外框；**xdg-activation**（程式要求到前景）。
- **輸入**：pointer-constraints＋relative-pointer（遊戲、虛擬機器）、keyboard-shortcuts-inhibit（虛擬機器、遠端桌面全螢幕時
  Super 等鍵交給它們）、cursor-shape。
- **輸入法**：input-method-v2＋text-input-v3＋input popup surface（Firefox、Chromium、GTK4、Qt6 原生程式打中文）。
- **其他**：primary-selection、data-control（剪貼簿管理員）、viewporter、fractional-scale（HiDPI 不糊）、presentation-time、
  idle-inhibit（看影片不鎖螢幕）、foreign-toplevel（視窗清單給面板）、security-context。
- **工作階段**：設 `WAYLAND_DISPLAY`、`XDG_SESSION_TYPE=wayland`，GTK 用 `wayland,x11`、Qt 用 `wayland;xcb`、
  Firefox `MOZ_ENABLE_WAYLAND=1`、Electron／Chromium 用 `--ozone-platform-hint=auto`。
- **xdg-desktop-portal-wlr**：瀏覽器、OBS 的螢幕分享與截圖。檔案選擇的 portal：xdg-desktop-portal-gtk 會帶進
  gsettings 等 GNOME 的東西，之後考慮自己寫一個用 Pomelo 檔案對話窗的 portal 後端。
- 規模：compositor 約增加 3,000～4,000 行。

### 階段 2：libpomelo 加 Wayland 後端（同一套 API，執行時看環境選）

- 視窗用 xdg_toplevel；繪圖跟現在一樣先畫在 cairo 的記憶體影像，再交給 wl_shm（雙緩衝）；HiDPI 縮放。
- 鍵盤用 xkbcommon（配置、重複鍵）、滑鼠、滾輪、觸控板；游標主題；彈出選單用 xdg_popup；檔案對話窗、pm_ask。
- 剪貼簿、拖放（wl_data_device）；輸入法（text-input-v3）。
- **換掉根視窗屬性**：設定改了→ inotify 監看 settings.conf；面板動作→ compositor 的控制 socket（已經有 ipc.c）；
  單一實例→ `$XDG_RUNTIME_DIR` 裡各程式的 socket；輸入法狀態→ input-method 協定或 socket。X11 工作階段也改用同一套，
  不再兩種並存。
- 規模：libpomelo 約增加 3,000 行。完成後 `pm_ui_run` 類的設定程式（約 15 個）不用改就是原生 Wayland。

### 階段 3：桌面外殼改原生

- 面板與 Dock：layer-shell＋foreign-toplevel；桌布：layer-shell 的 background 層。
- 啟動器、總覽、搜尋、通知、控制中心、小柚（桌面上走動的機器人）：layer-shell 的 overlay 層＋輸入區域。
- 螢幕鎖定：ext-session-lock（compositor 保證鎖住，比現在搶鍵盤的方式安全）。
- 截圖與錄影：screencopy；錄影改從 compositor 取畫面（不再用 ffmpeg x11grab）。
- 輸入法 pomelo-input：input-method-v2 用戶端＋選字列 popup；XIM 伺服器留著給 XWayland 程式。
- 電源管理的閒置偵測：ext-idle-notify；顯示器設定：output-management；登入畫面、密碼視窗、鑰匙圈提示：layer-shell。

### 階段 4：應用程式改原生（2026-11-20 完成，見上面「階段 4 第 6 步完成」）

- 有自己事件迴圈的程式改用 libpomelo 新的視窗 API（X11 與 Wayland 共用一份程式碼），一次轉一個；
  沒轉的繼續走 XWayland。
- 比較麻煩的：終端機（鍵盤、輸入法、剪貼簿細節多）、遠端桌面與虛擬機器畫面（全螢幕時攔截系統快捷鍵、相對滑鼠）、
  截圖（選取範圍的覆蓋層）。

### 階段 5：X11 工作階段（pomelo-wm）的去留

- **保留**：libpomelo 雙後端一直維護，每個功能要在兩種工作階段各測一次。
- **停止**：階段 4 完成後 pomelo-wm 下架；compositor 在五個發行版都已經在用（wlroots 私有編譯，Rocky 9 也一樣）。
- 不管哪個選擇，XWayland 都保留給第三方的 X11 程式。

#### 階段 5 前提：沒有 XWayland 時 pomelo-session 的就緒判斷（2026-11-20 風險評估，**只讀程式碼，沒有實跑，也沒有改 pomelo-session**）

- **現況（pomelo-session.c）**：就緒判斷全靠「連 X、讀根視窗屬性」。`root_has()` 在 `XOpenDisplay` 失敗時回 0，有三個呼叫處受影響：
  1. `wm_already_present()`（`_NET_SUPPORTING_WM_CHECK`）：沒有 X → 0 → 走 `start_wm()` 啟動 X 視窗管理員 `pomelo`。它連不上 X，結束碼 2；`wait_wm_ready()` 自己也連不上 X、直接返回（不會卡住），
     主迴圈 `wait()` 馬上收到 wm 結束、碼 2 被當成「使用者登出，或 X 已關閉」→ 記 log「pomelo 結束，登出」→ `break` → `shutdown_all()`。**結果：登入後桌面一閃就回到登入畫面（無限登入迴圈），pomelo-compositor 才剛起來就被收掉。**這是最大的風險，也是唯一會讓桌面起不來的點。
  2. `root_has("_POMELO_COMPOSITOR")` 判斷「是不是 pomelo-compositor」：沒有 X → 0 → `_JAVA_AWT_WM_NONREPARENTING` 不設（Java 程式視窗一片灰；沒有 XWayland 時 Java／AWT 本來也只能走原生，影響小，但要記得）；
     `start_services(0)` 的條件是 `(!compositor || getenv("WAYLAND_DISPLAY"))`，有 `WAYLAND_DISPLAY` 時結果一樣（照跑 `pomelo-display --apply`），不受影響。
  3. `wait_panel_ready()`：連不上 X 就直接返回，不等面板第一次畫好 → 自動啟動的程式提早跟面板搶磁碟與 CPU（登入會慢一點，不是故障）。
- **面板結束碼 2**：`code == 2` 一樣被當成「X 已關閉」而 `break`。面板原生版沒有 X 時目前不會以 2 結束（IPC 只是少功能），但之後面板若改成「連不上 X 就 exit(2)」會直接觸發登出——**改面板時要同步改 session**。
- **另一個方向的競態（推測，沒量過）**：compositor 起來、XWayland 還沒就緒時，`wm_already_present()` 也可能暫時回 0，pomelo-dm 目前靠 compositor log 的「XWayland 就緒」才啟動 session 才避開；若之後 XWayland 改成「第一個 X 程式連上才啟動」（lazy），這個判斷會變成必然失敗，所以不能繼續靠 X 屬性當就緒訊號。
- **建議修法（階段 5 開頭做，順序要緊）**：
  1. **就緒與身分不靠 X**：pomelo-dm 啟動 compositor 時已經知道它的 PID 與 `WAYLAND_DISPLAY`，另外給 session 一個環境變數（例如 `POMELO_COMPOSITOR_PID`），或 session 連 Wayland 檢查有沒有 `pomelo_workspace_v1` 全域物件（只有 pomelo-compositor 有）。
     有這個訊號就**完全跳過** `start_wm()`／`wait_wm_ready()`／`wm_already_present()`；`root_has("_POMELO_COMPOSITOR")` 同樣改用它（Java 變數照設，或有 XWayland 才設）。沒有 `WAYLAND_DISPLAY` 才走舊的 X 判斷（X11 工作階段、Xephyr 的 `make test` 照舊）。
  2. **登出的依據改成 compositor 結束**：Wayland 工作階段裡 session 要監看 compositor（`pidfd`／`waitpid` 該 PID）——compositor 結束＝登出；面板、`pomelo` 的結束碼 2 只在 X 模式才當成登出，Wayland 模式只記 log 並照 `too_many()` 重啟。
  3. **`wait_panel_ready()` 換通道**：面板第一次畫好改用 `$XDG_RUNTIME_DIR/pomelo-panel-ready`（面板寫自己的 PID）或 inotify，不要靠 X 根視窗屬性（階段 5 要把 `_POMELO_PANEL_READY` 這類屬性換成 socket／協定的一部分）。
  4. **測試方式**：compositor 需要一個「不啟動 XWayland」的開關（目前沒有，例如 `POMELO_NO_XWAYLAND=1`，wlroots 建 `wlr_xwayland` 前判斷）；之後在 headless compositor 用假 PATH（只放 session 啟動的假程式）跑 session，檢查：不啟動 `pomelo`、面板有起來、
     殺面板後會重啟、殺 compositor 後 session 正常結束且不當掉；另外各跑一次「有 XWayland」「X11（Xephyr）」確認沒退步。**改 session 風險高（登入失敗＝進不了桌面），要在虛擬機器或 VT 上實機登入／登出各測多次再發行**；
     修之前先在 session 加一個 `POMELO_SESSION_NO_X=1` 之類只用於測試的路徑，不要直接改正式判斷。
  5. 同時處理的 IPC 前提（見缺口清單）：面板的輸入法／播放器／錄影圖示與 `_POMELO_SPAWN`／使用時間、小柚的單一實例轉交——這些換成 socket 之後，session 才真的能在沒有 XWayland 的環境完整運作。
- **小柚的設定／確認／對話視窗**（原本也列在這裡）已在 2026-11-20 修好：`main()` 在 `pmwl_connect()` 成功時設 `pm_ui_native = 1`，沒有 `DISPLAY` 也能開（見 CHANGELOG 0.42）。

## 測試

- 開發主機上用 headless 的 compositor＋grim 截圖＋虛擬鍵盤滑鼠（之前 0.35 的測試方式）自動測。
- 顯示卡、多螢幕、HiDPI、觸控板手勢、睡眠喚醒只能在實機確認：每個階段發行前請在實機與 VirtualBox 各跑一輪。
- 第三方程式清單：Firefox、Chromium、小柚瀏覽器、VS Code（Electron）、LibreOffice、GIMP（GTK3）、Qt6 程式、
  OBS（螢幕分享）、Wine、Steam（XWayland）。

## 需要決定的事

1. 轉移完成後要不要保留 X11 工作階段（pomelo-wm）？建議：轉移期間保留，階段 4 完成後下架。
2. 從階段 1 開始？建議：是，第三方程式立刻變原生，而且後面每個階段都需要它。
3. 每個階段一個版本（例如 0.38＝階段 1）？
