# VPN 伺服器（pomelo-vpn）

用精靈建立並管理 **OpenVPN 伺服器**，也可以設定 **Tailscale**；需要的軟體在介面上按「下載並安裝」就會取回。不常駐、不自己連到外部網站，開啟時只讀取狀態。

## 兩種 VPN，怎麼選

| | OpenVPN 伺服器 | Tailscale |
|---|---|---|
| 做法 | 你自己架、自己管理憑證 | 登入帳號後，裝置自動互連 |
| 適合 | 讓外面的人（或你的手機）連回你的網路 | 自己的幾台裝置互連 |
| 需要 | 固定網址（網域或 DDNS）與路由器的連接埠轉發 | 不用開連接埠、不用公網 IP |
| 代價 | 要自己維護憑證、防火牆 | 依賴 Tailscale 公司的協調伺服器（第三方雲端服務；有免費方案，需登入帳號） |
| 授權 | OpenVPN 社群版 GPL-2.0（加 OpenSSL 例外）；easy-rsa GPL-2.0 | 用戶端 tailscale／tailscaled 是開源的 BSD-3-Clause |

想完全自己掌控 Tailscale 的協調服務，可以在 Tailscale 設定的進階選項填入自架的 **Headscale**（開源）的網址（`--login-server`）；pomelo-vpn 只提供這個輸入欄位，不會安裝 Headscale。

## 開始

從應用程式選單開「VPN 伺服器」（或 `pomelo-vpn`；網路設定的 VPN 區也有「建立 VPN 伺服器…」）。總覽頁的橫幅先給結論：

- 綠：OpenVPN 伺服器運作中，N 台已連線
- 橘：有注意事項（憑證將在 30 天內到期、防火牆沒有開放連接埠、這台電腦在路由器後面需要連接埠轉發…）
- 紅：服務啟動失敗、憑證已過期、CA 私鑰權限不安全
- 灰：尚未設定 VPN

下面是兩張卡片（OpenVPN、Tailscale），注意事項放最下方：標題、為什麼重要、建議做法。

## 下載與安裝

「下載與安裝」頁每個軟體一列：用途、是否已安裝（版本）、來源、授權、大小、按鈕。安裝走系統的套件管理員（apt／dnf，只裝必要套件，不裝建議套件）加管理員驗證；按下去前會顯示來源與將執行的指令。

| 軟體 | 用途 | 來源 |
|---|---|---|
| openvpn | OpenVPN 伺服器（必要） | 發行版軟體庫（Rocky：EPEL） |
| easy-rsa | 憑證工具（必要） | 發行版軟體庫（Rocky：EPEL） |
| qrencode | 手機掃描 QR（選用） | 發行版軟體庫（Rocky：EPEL） |
| tailscale | Tailscale 用戶端（選用） | Fedora：Fedora 軟體庫；其他：Tailscale 官方軟體庫（第三方） |

- **Rocky Linux**：openvpn、easy-rsa、qrencode 在 EPEL，要先按「啟用 EPEL 與 CRB」。
- **Tailscale 官方軟體庫**（Ubuntu、Debian、Rocky）：這是第三方軟體庫（pkgs.tailscale.com）。按「下載並安裝」會先顯示說明、簽章金鑰的指紋、將新增的檔案內容與指令，勾選「我了解這是第三方軟體庫」才會執行。金鑰一律用 HTTPS 下載、驗證指紋必須是 `2596 A99E AAB3 3821 893C 0A79 458C A832 957F 5868`（2026-11-18 從官方網站核對），不符就中止；不會用 `curl | sh`。「移除 Tailscale 軟體庫」可以還原。
- **沒有網路**：會明確說明無法連線，可重試。目前沒有圖形化的離線包匯入；可以把套件檔放到資料夾，用 `sudo apt install ./*.deb` 或 `sudo dnf install ./*.rpm` 安裝。

## OpenVPN 伺服器精靈（八步）

1. **確認軟體**：openvpn、easy-rsa（選用 qrencode），缺的按一下安裝。
2. **網路設定**：對外位址（自己輸入網域或 IP；**不會自動連到外部網站查**，可按「偵測公網 IP」，會先說明連到哪個服務、傳出什麼，確認後才做）；UDP 1194（或 TCP 443，穿越嚴格網路但較慢）；VPN 子網路（預設 10.8.0.0/24，與本機網路衝突會警示並建議替代）；客戶端可存取「只有這台電腦／整個區網／所有流量走 VPN」；DNS 推送；是否允許客戶端互相通訊（預設否）。偵測到這台電腦在路由器後面會說明要做連接埠轉發。
3. **安全等級**：預設「現代」（見下）；可選「相容舊客戶端」（標示不安全）。可預覽將產生的設定檔，每一行有白話說明。可選替 CA 私鑰加密碼。
4. **建立憑證與設定**：在 `/etc/openvpn/pomelo-pki`（root、0700）建立 CA、伺服器憑證、tls-crypt 金鑰、撤銷清單，寫出 `/etc/openvpn/server/pomelo.conf`。
5. **防火牆與轉送**：一鍵放行連接埠、開啟 IP 轉送、設定 NAT；或只顯示指令自己做。改動前自動備份，可還原。
6. **啟動服務**：啟用並啟動 `openvpn-server@pomelo`，啟動後檢查服務、連接埠、tun 網卡；失敗時顯示日誌與可能原因。
7. **建立第一個客戶端**：名稱（只能用字母數字與 - _ .）；輸出內嵌式 .ovpn（存成檔案 0600、顯示 QR、匯入到這台電腦的 VPN 客戶端）。
8. **完成**：摘要與下一步（備份 CA、路由器連接埠轉發、定期撤銷不用的客戶端）。

### 安全預設與理由

| 設定 | 理由 |
|---|---|
| TLS 1.2 以上（支援 1.3）、`tls-version-min 1.2` | 不接受舊協定 |
| `tls-crypt` | 控制通道加密並驗證：沒有金鑰的人連握手都不行，擋掃描與阻斷服務 |
| 資料通道 AES-256-GCM（備援 ChaCha20-Poly1305） | AEAD，內建驗證 |
| EC 憑證（secp384r1，可選 prime256v1）、`dh none`、`ecdh-curve` | 橢圓曲線，不產生 DH 參數檔 |
| `remote-cert-tls client`、`verify-client-cert require` | 一定要有效的客戶端憑證 |
| `crl-verify`（撤銷清單，效期 10 年；每次撤銷自動更新） | 被撤銷的客戶端不能再連線 |
| `user nobody`／`group nogroup`（或 nobody）、`persist-key`、`persist-tun` | 啟動後降權，不以 root 執行 |
| `management … unix` 加 `management-client-user root` | 本機 UNIX 通訊端，不開網路埠，只有 root 能用來中斷連線 |
| `keepalive`、`explicit-exit-notify`（UDP）、`reneg-sec 3600` | 偵測斷線、每小時換金鑰 |

CA 私鑰預設不加密以利自動簽發；可選「CA 私鑰加密碼」：之後新增、撤銷客戶端都要輸入密碼（只經標準輸入傳給輔助程式，用環境變數交給 easy-rsa 的子行程，不上命令列、不寫檔、不進日誌）。**最安全的做法是把 CA 私鑰離線保存。**

「相容舊客戶端」額外允許 AES-256-CBC，只有舊客戶端連不上時才選。

## 管理

OpenVPN 頁：

- **客戶端**：名稱、狀態（已連線／離線／已撤銷）、虛擬 IP、來源位址、連線時間、上傳下載量；右鍵或按鈕：重新匯出設定檔、顯示 QR（含私鑰，先警告、勾選確認、60 秒後自動隱藏）、複製虛擬 IP、續期、撤銷（立即中斷連線）、刪除。連線資訊來自 OpenVPN 的狀態檔 `/run/pomelo-vpn/status.log`（status-version 3；選它而不是 management 介面，因為不用另外開通訊端、格式簡單、唯讀；降權後的 openvpn 可寫的專用資料夾，開機由 tmpfiles.d 建立；只有 root 讀得到，由唯讀的 `pomelo-vpn-status` 經 polkit 取得）。撤銷時立即中斷連線則用 management 的本機 UNIX 通訊端（只允許 root）。
- **憑證健檢**：CA、伺服器憑證、撤銷清單的到期日；30 天內橘色、過期紅色。
- **診斷**：「為什麼連不上」：服務、連接埠、防火牆、NAT 後面、IP 轉送、NAT 規則、憑證、系統時間、tun 裝置。
- **日誌**：`journalctl -r -n 500 -u openvpn-server@pomelo`，錯誤紅色。
- **伺服器**：啟動、停止、重新啟動、重新載入、開機自動啟動、編輯網路設定、備份 PKI（gpg 密碼加密的單一檔案，密碼不存放；或用「備份」程式）、解除安裝（保留 PKI／全部刪除，後者要輸入伺服器名稱確認）。

### 防火牆與 NAT 怎麼並存

| 偵測到 | 做法 |
|---|---|
| firewalld | `--permanent` 加入連接埠；NAT／轉送用 firewalld 的 policy `pomelo-vpn`（trusted → ANY、masquerade）並把 VPN 子網路加入 trusted 區 |
| ufw | `ufw allow` 連接埠、`ufw route allow from 子網路`；NAT 寫在 `/etc/ufw/before.rules` 的 `# BEGIN pomelo-vpn … # END pomelo-vpn` 標記區塊（可還原） |
| 其他／純 iptables | 建立 `pomelo-vpn-nat.service`：只在自己的 `POMELO_VPN_IN／FWD／NAT` 鏈加規則，停止時移除，不碰別人的規則；重複套用不會重複 |

防火牆管理員（pomelo-firewall）會把這些鏈顯示為「由 pomelo-vpn 管理」（唯讀）。IP 轉送寫在 `/etc/sysctl.d/99-pomelo-vpn.conf`。所有改動前先備份到 `/var/lib/pomelo-vpn/backup/`。不確定時只顯示可複製的指令。

### 各發行版（2026-11-18 在容器內實查）

| 發行版 | openvpn | easy-rsa | qrencode | Tailscale | easyrsa 位置 | `/etc/openvpn/server` | 降權帳號 |
|---|---|---|---|---|---|---|---|
| Ubuntu 24.04 | 2.6.19（main） | 3.1.7（universe） | 4.1.1（universe） | 官方軟體庫 noble（1.104.7） | /usr/share/easy-rsa/easyrsa | 755 root:root | nobody／nogroup |
| Ubuntu 26.04 | 2.7.0 | 3.2.5 | 4.1.1 | 官方軟體庫 resolute | 同上 | 755 root:root | nobody／nogroup |
| Debian 13 | 2.6.14 | 3.2.2 | 4.1.1 | 官方軟體庫 trixie | 同上 | 755 root:root | nobody／nogroup |
| Fedora 44／45 | 2.7.9 | 3.2.7 | 4.1.1 | **Fedora 自己的套件 1.102.5**（官方庫較新：1.104.7） | /usr/share/easy-rsa/3/easyrsa | 750 root:openvpn | openvpn／openvpn |
| Rocky 9.8 | 2.5.11（**EPEL**） | 3.2.1（**EPEL**） | 4.1.1（**EPEL**） | 官方軟體庫 centos/9 | /usr/share/easy-rsa/3/easyrsa | 750 root:openvpn | openvpn／openvpn |

- 套件名稱各發行版都叫 `openvpn`、`easy-rsa`、`qrencode`、`tailscale`；授權：openvpn GPL-2（加 OpenSSL 例外）、easy-rsa GPL-2.0、qrencode LGPL-2.1+、tailscale BSD-3-Clause。
- systemd 單元在所有發行版都是 `openvpn-server@.service`（實例 `openvpn-server@pomelo`），工作目錄 `/etc/openvpn/server`，設定檔 `/etc/openvpn/server/pomelo.conf`，狀態檔預設在 `/run/openvpn-server/`。
- 以「不裝建議套件」模擬安裝 openvpn、easy-rsa、qrencode，六個發行版都**沒有**帶進 GNOME／GTK 套件。
- Fedora／Rocky 的 SELinux：使用非標準連接埠時啟動前會自動 `semanage port -a -t openvpn_port_t`（輔助程式內處理；SELinux 本身沒有在容器裡實測）。

## Tailscale

「Tailscale」頁：

1. **登入**：按「登入」產生登入網址（可開瀏覽器、複製、顯示 QR），等待登入完成（可取消，5 分鐘逾時）；進階：貼上 auth key（`tskey-…`，只經管道傳給 tailscale，不上命令列、不存檔）或自架登入伺服器 Headscale（https 網址）。第一次若提示權限不足，按「允許我操作 Tailscale（只需要一次）」（`tailscale set --operator`，需要管理員驗證）。
2. **選項**：接受其他節點的子網路由、Tailscale DNS、SSH、通告子網路由（自動偵測區網）、當出口節點（需要 IP 轉送，會一併寫入 `/etc/sysctl.d/99-pomelo-tailscale.conf`；路由與出口節點要到 Tailscale 管理後台核准）、本機主機名稱。每項附風險說明。
3. **完成**：Tailscale IP（v4、v6）、MagicDNS 名稱、版本、狀態。

管理：裝置清單（直連／中繼、出口節點、標籤、最後看到）、切換出口節點、連線／斷線、登出、重新驗證、診斷、網路檢查（`tailscale netcheck`）、日誌。只用本機命令列，**不使用 Tailscale 的雲端 API、不存任何 API 金鑰**。

## 命令列

```
pomelo-vpn [--status] [--json] [--page overview|openvpn|tailscale|install]
           [--clients] [--add-client 名稱 --out 檔] [--revoke 名稱] [--diagnose] [--help]
```

結束碼：0 正常、1 有注意事項、2 有問題、3 尚未設定、4 用法錯誤、5 操作失敗。`--add-client` 的設定檔含私鑰，一律寫入 `--out` 指定的檔案（0600，不覆蓋既有檔案，不跟隨符號連結），不會印在螢幕上。改變類指令走 pkexec 輔助程式。

## 疑難排解

- **客戶端連不上**：開「診斷」逐項看；最常見是路由器沒做連接埠轉發、防火牆沒放行、系統時間不對。
- **連上了但上不了網**：IP 轉送或 NAT 沒設定、沒有推送 DNS。
- **憑證到期**：總覽橘色提醒；伺服器憑證與撤銷清單在「憑證健檢」續期；客戶端憑證在客戶端清單續期（要重新發設定檔）；CA 過期要重建。
- **服務啟動失敗**：看日誌；常見原因是連接埠被占用、tun 不可用（虛擬機或容器要允許 /dev/net/tun）、SELinux。

## 解除安裝

OpenVPN 頁 →「伺服器」→「解除安裝」：保留憑證與客戶端紀錄，或全部刪除（需輸入伺服器名稱確認）。會停止服務並還原由 pomelo-vpn 做的防火牆、NAT、sysctl 設定。

## 安全模型

- 特權動作只走 `pomelo-vpn-admin`（pkexec／polkit）：白名單子命令、結構化參數、全部驗證；設定檔由它自己產生，不接受任意設定文字、不執行任意指令、不經 shell。
- 唯讀的 `pomelo-vpn-status` 只印 OpenVPN 狀態檔（誰在線上），本機登入中的使用者免密碼。
- 輸入驗證：客戶端名稱、位址、連接埠、子網路、DNS、auth key、網址一律白名單；單元測試含 2.4 萬筆模糊測試。
- 效能：不常駐；開啟約 10 ms 出第一個畫面；狀態在背景讀取；視窗開著時閒置每 10 秒讀一次，隱藏就停止。

## 尚未完成

- 離線包匯入（圖形介面）。
- 以真實 Tailscale 帳號或 Headscale 做端對端登入（只用假的 tailscale 命令列測試）。
- IPv6 的 VPN 子網路與轉送（目前 VPN 只做 IPv4）。
- OpenVPN 客戶端的 .ovpn 由伺服器保存私鑰以便重新匯出；尚未提供「客戶端自己產生私鑰、只送簽署請求」的模式。
