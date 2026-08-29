# RG40XX V 裝置控制服務（候選套件）

這個目錄提供九套彼此獨立的裝置服務。它們目前只會產生候選套件，**不會安裝、啟用、連線或寫入實機**。

## 內容

### 1. USB 偵錯：`rg40xxv-usb-debug`

- 透過 Linux configfs 建立複合 USB gadget：
  - RNDIS 網路介面 `usb0`
  - CDC ACM 序列埠 `/dev/ttyGS0`
- 絕不建立 `mass_storage` function，也不會把 TF1、TF2、ROM 或存檔磁碟暴露給 USB host。
- `rg40xxv-usb-debug.service` 是可重複執行的 systemd oneshot；停止服務只解除 UDC 綁定。
- `rg40xxv-usb-debug-getty.service` 在 `ttyGS0` 啟動一般密碼登入提示，不會設定密碼或開啟自動登入。
- `99-rg40xxv-usb-debug.network` 可讓 systemd-networkd 將掌機設為 `10.55.0.1/24` 並向 USB host 發 DHCP 租約。
- `check-kernel-config.sh` 會檢查 configfs、libcomposite、ACM、RNDIS 與 gadget serial 所需核心選項；UDC 控制器只能在目標 DTS／核心確認，因此另列為警告。

預設 VID/PID 採 Linux Foundation 的開發用值，只適合內部測試；公開散布正式映像前必須申請或改用合法 VID/PID。此服務不會修改 SSH 設定；若系統原本已啟用 SSH，USB host 可經 `usb0` 連線。

### 2. 日誌控制：`debug-logctl`

介面只有：

```text
debug-logctl status [all|boot|kernel|ui|emulator|streaming|system]
debug-logctl export <分類>
debug-logctl clear <分類>
```

- 分類與來源路徑寫死在程式中，不接受自訂來源或輸出路徑。
- 匯出檔固定寫入 `/var/lib/rg40xxv/debug-exports/`，權限為 `0600`。
- `clear` 只處理該分類的固定日誌檔／目錄；不會碰 ROM、saves、NetworkManager 設定、Moonlight 憑證或 SSH keys。
- 清除 `boot` 前會先把 pstore ramoops 與 `last-failure.log` 原子封裝成 `/var/lib/rg40xxv/debug-retained/last-boot-failure.tar.gz`。
- pstore 本身永不由此工具清除。UI 的「長按確認」需在呼叫 `clear` 前完成；CLI 不假裝替 UI 判斷按鍵狀態。

### 3. 存檔防護：`save-guard`

```text
save-guard inventory
save-guard backup-before-core-switch --from <核心名稱> --to <核心名稱>
```

固定涵蓋：

- RetroArch save/state
- PPSSPP `SAVEDATA`、save state、截圖與作弊碼；包含原廠卡上路徑及
  mainline session 的 `/mnt/data/rg40xxv/state/home/.config/ppsspp`
- DraStic backup/save state
- `/mnt/mmc/Roms` 內相鄰的 `savedata`、`Saves`、`saves`、`Save`、`SAVEDATA`
- PORTS 遊戲目錄內上述存檔目錄

清單使用 SHA-256 與 Base64 路徑欄位，避免 Tab、換行或非 ASCII 檔名破壞格式。`inventory.tsv` 以及每次核心切換前的備份目錄都在同一檔案系統完成暫存後原子 rename。備份工具只有讀取來源與建立新備份，**沒有清除、搬移或覆蓋原存檔的路徑**。備份保存期限由日後 UI 明確操作，這個服務不會自動輪替或刪除。

### 4. 電源鍵鎖定後端：`power-lockctl`

UI 是 `KEY_POWER` 的唯一使用者空間擁有者，並把「按下／放開／計時器／一般按鍵按下」轉成：

```text
power-lockctl event power-down
power-lockctl event power-up
power-lockctl event button A
power-lockctl tick
power-lockctl status
power-lockctl config set lock-enabled true
power-lockctl config set lock-enabled false
power-lockctl hardware screen-off
power-lockctl hardware screen-on
power-lockctl hardware brightness 0..100
power-lockctl hardware joystick-rgb 0..100
```

- 短按：保存背光與搖桿 RGB，立即關閉兩者；UI 進入 `paused`，停止動畫與繪圖；GPU runtime PM 改為 `auto`。不寫 CPU 頻率，也不做 full `sync`。
- `lock-enabled=true`（預設）：關屏狀態是 `screen_off` 且已鎖定；再短按還原背光／RGB並顯示鎖定畫面，不直接解鎖。
- `lock-enabled=false`：關屏狀態明確叫 `light_sleep`；再短按只還原硬體並直接回到 `awake`，不顯示鎖定頁、不要求三連按。省電與鎖定在 API/state 中不混用。
- 解鎖：在鎖定畫面用同一顆一般按鍵連按三次；換鍵或超過 1.5 秒會重算。RESET、音量上／下完全不計，也不破壞既有次數。
- 長按：2.5 秒顯示關機確認，持續到約 3 秒才回傳 `ACTION_REQUEST_ORDERLY_SHUTDOWN`；提前放開或按 B 取消。helper 本身不呼叫 `poweroff`，由 UI／root service 收到 action 後走正常 systemd 關機。
- 關屏時採事件驅動，不需要 frame loop。只使用 runtime idle，**不預設 suspend-to-RAM**。
- STR 要另通過 wake source、GPU、Wi-Fi、audio resume 連續 20 輪測試後才可能啟用；目前沒有 STR 程式碼。
- `60-rg40xxv-power-key.conf` 以 systemd 249 支援的 `HandlePowerKey=ignore` 讓 logind 忽略 power key，避免 UI 與 logind 雙重處理；長按也由同一 UI timer 判斷。正式映像必須確認沒有其他 evdev daemon 同時抓 `KEY_POWER`。
- 所有 sysfs 路徑由固定 class 與名稱規則發現；硬體快照也會再次驗證。任一必要節點缺失或寫入失敗時會還原已改值、維持原狀，拒絕假裝鎖定成功。
- `lock-enabled` 設定原子保存於 `/var/lib/rg40xxv/power-lock/config.v1`；執行狀態只放 `/run`。

### 5. UI 固定硬體控制入口：`ui-hardwarectl`

安裝路徑固定為 `/usr/sbin/ui-hardwarectl`，只接受以下 argv：

```text
ui-hardwarectl screen-off
ui-hardwarectl screen-on
ui-hardwarectl brightness 0..100
ui-hardwarectl joystick-rgb 0..100
ui-hardwarectl volume 0..100
ui-hardwarectl volume-up
ui-hardwarectl volume-down
ui-hardwarectl mute-toggle
ui-hardwarectl usb-debug on
ui-hardwarectl usb-debug off
ui-hardwarectl network-status
ui-hardwarectl network-wifi-recover
ui-hardwarectl network-wifi-scan
ui-hardwarectl network-wifi-connect BSSID   # 密碼由 stdin
ui-hardwarectl network-wifi-disconnect
ui-hardwarectl network-wifi-forget UUID
ui-hardwarectl network-hotspot on|off
ui-hardwarectl orderly-shutdown
```

- 不接受任意路徑、systemd unit、程式、shell 字串或額外參數；百分比只接受十進位 `0..100`。
- `screen-off`、`screen-on`、背光與 RGB 都固定委派到 `power-lockctl hardware ...`，因此與 power-key state machine 共用 `/run/rg40xxv/power-lock/.lock`、硬體快照及 rollback，不會另外建立一套會互相覆寫的狀態。
- 背光與 RGB 百分比會依各節點的 `max_brightness` 做整數換算。背光 `0` 會 clamp 到可信、非 symlink 的 `min_brightness`；沒有該節點時安全 fallback 為 raw `1`，不冒充關屏。只有 `screen-off` 會把背光真正寫成 `0`。production DT 的 joystick RGB 對應精確 sysfs 名稱 `rgb:kbd_backlight`；allowlist 也保留既有 joystick/stick 名稱，但排除 status/power LED。`joystick-rgb 0` 仍會真正關燈；目前只控制 scalar `brightness`，不宣稱能設定 RGB 色彩。
- sysfs 列舉有固定上限，候選 class 名稱有 allowlist。Linux 正常建立的 `/sys/class/backlight/*`、`/sys/class/leds/*` 與 `/sys/class/udc/*` class-entry symlink 是唯一例外：必須經 `realpath -e` 落在同一 `/sys/devices/` 內；backlight/LED 的後續 attribute 仍必須是該 resolved device directory 內的 regular、non-symlink 檔案。任何 escape 都 fail closed。任一必要節點、快照或寫入失敗即還原；screen-off 失敗會嘗試還原背光、`bl_power`、RGB 與 GPU runtime PM。
- `screen-off` 的 CPU 行為只有讓 UI 停止 render，再關背光／RGB並把 GPU runtime PM 設為 `auto`；CPU 交給 `schedutil` 與 CPU idle 自然降頻，這個入口不寫 CPU frequency、OPP、regulator 或 voltage。
- `usb-debug on|off` 只以乾淨環境固定呼叫 `/usr/libexec/rg40xxv/usb-debugctl start|stop`。這是當次 bind/unbind，不會偷偷 enable unit；仍只有 RNDIS 與 ACM，絕不加入 mass-storage。
- `orderly-shutdown` 在 production 只會執行固定的 `/usr/bin/systemctl poweroff`。fixture mode 則要求固定 `/tmp/rg40xxv-device-control-test.*/mock-bin/systemctl`；測試不可能呼叫 host 的 systemctl。
- 這個 shell helper 不是 setuid 權限邊界。正式 UI 必須由受信任的 root service 以固定 argv 呼叫，且不可繼承測試環境變數。

### 6. 常駐系統音量：`rg40xxv-volume`

- daemon 不寫死 `event2`：只開啟 non-symlink `/dev/input/event*` character device，並以 `EVIOCGNAME` 精確驗證 `gpio-keys-volume` 及兩顆 volume key capability；不使用 `EVIOCGRAB`，不搶 UI 的其他輸入。
- `KEY_VOLUMEUP/DOWN` 的 press 與核心 autorepeat 都各調整 5，clamp 在 `0..100`。主迴圈永久阻塞於 `poll(2)`，沒有 frame/timer polling；fixture 會記錄 250 ms idle CPU ticks 與三筆按鍵事件至 marker 的 latency，gate 分別為 2 ticks／500 ms（這不是實機 latency 聲明）。
- 直接以 ALSA control ioctl 驗證 card 0 `Codec` 的精確 control/type/range。`Line Out Playback Volume` 是唯一可變增益；`DAC Playback Volume` 固定在 63（0 dB unity baseline），避免兩層音量疊加。0 關閉 Line Out 與 Speaker switch，非 0 才開啟，因此共用 Line Out 路徑的耳機與喇叭都受控。
- 每次異動先快照七個必要 control，任一 write/readback 或 marker 發布失敗就 reverse rollback，不回報假成功。
- 狀態以同目錄 temporary file、`fsync`、`rename` 原子發布至 `/run/rg40xxv-ui/alsa-volume`，格式固定為 `volume_percent=0..100` 與 `muted=0|1`。UI 若需要 OSD，可監看 parent directory 的 rename；backend 不製作假的 DRM overlay。
- 內部 CLI 只透過 mode `0600`、root peer-credential 驗證的 `SOCK_SEQPACKET` `/run/rg40xxv-volume/control.sock` 傳送 `get|set N|up|down|mute-toggle`，不執行 shell、任意路徑或外部 mixer 命令。
- `rg40xxv-volume.service` 由 `multi-user.target` 拉起且不隸屬 UI lifecycle，所以 UI 暫停與遊戲啟動期間仍持續處理實體按鍵。

### 7. CPU policy hook：`cpu-policyctl`

```text
cpu-policyctl validate
cpu-policyctl status
cpu-policyctl ui-default
cpu-policyctl lock-idle
cpu-policyctl run-performance -- <模擬器與參數...>
```

- `ui-default` 只把現有 cpufreq policy 切到核心已列出的 `schedutil`。
- `run-performance` 先快照每個 policy 的 governor，再暫時切到 `performance` 執行遊戲；子程式正常、失敗或收到 TERM/INT 時都會還原。它不更改遊戲的存檔路徑或參數。
- `lock-idle` 是明確的無寫入 hook：鎖屏依靠 CPU idle/runtime PM，不固定頻率。
- `validate/status` 驗證 `scaling_available_governors`、`scaling_available_frequencies`（公開 OPP）及 eFuse nvmem 是否可見，但不讀出 eFuse 內容。
- 程式中沒有寫入 `scaling_min_freq`、`scaling_max_freq`、`scaling_setspeed`、CPU `online`、regulator 或 voltage 的路徑，因此不會超頻、不會離線核心，也不會猜測未公開 OPP。
- production 核心目前最低公開 CPU OPP 是 **480 MHz**。UI 只顯示 sysfs 實際公開清單；本套件不寫入、不顯示也不宣稱 240 MHz 可用。

### 8. OpenVPN client backend：`vpn-profilectl` / `vpn-firewall`

```text
vpn-profilectl import NAME --ovpn client.ovpn [--ca ca.crt] [--cert client.crt] [--key client.key] [--auth auth.txt]
vpn-profilectl verify NAME
vpn-profilectl profile set NAME autoconnect true|false
vpn-profilectl profile set NAME endpoint <IPv4> <port> udp|tcp
vpn-profilectl profile set NAME kill-switch true|false
vpn-profilectl policy set moonlight bypass|vpn
vpn-profilectl policy set games bypass|vpn
vpn-firewall apply NAME
```

- profile 目錄固定在 `/var/lib/rg40xxv/openvpn/profiles/NAME`，目錄 `0700`、所有設定／CA／cert／key／auth 都是 `0600`。
- 匯入會拒絕 `up/down/plugin/management/config/include/log/status` 等可執行程式或指定任意路徑的 directive；外部 CA/cert/key/auth 會複製並改寫成固定相對檔名。inline PEM 仍留在權限 `0600` 的 `client.conf`。
- `openvpn@.service` 使用 `Restart=always`、`auth-nocache` 與固定 `verb 3`；不把 key、密碼或完整 config 印到 journal。`autoconnect=true` 是 UI/system image 執行 `systemctl enable --now openvpn@NAME.service` 的契約，本候選套件不自行 enable。
- kill switch 預設 `false`。設為 `true` 前必須明確設定 VPN endpoint IPv4/port/proto；nftables output policy 保留 loopback、VPN tunnel、DHCP、endpoint，以及整段 `192.168.0.0/24`，所以 LAN SSH 回程不會被擋。手動停 VPN 時 kill switch 保持 fail-closed，需先在 UI 明確關閉才移除。
- traffic policy 固定為 `moonlight` 與 `games` 兩個 key，值只接受 `bypass|vpn`。預設 Moonlight bypass（目前 Sunshine 在 LAN）、games 使用 VPN；launcher 讀 `policy get` 決定 route/bind。這是明確設定契約，不宣稱尚未整合的遠端 policy-routing 已完成。
- 目前只允許同時一個 active VPN profile，避免兩個 profile 爭用同一 nftables table。

### 9. NetworkManager／Wi-Fi 控制：`rg40xxv-network-control`

```text
rg40xxv-network-control prepare
rg40xxv-network-control recover
rg40xxv-network-control status
rg40xxv-network-control scan
rg40xxv-network-control connect BSSID   # WPA-PSK 只由 stdin 傳入
rg40xxv-network-control disconnect
rg40xxv-network-control forget UUID
rg40xxv-network-control hotspot on|off
```

- `default-wifi-powersave-on.conf` 在 release 最上層把 NetworkManager 的 `wifi.powersave` 固定為 `2`（停用 Wi-Fi 省電）；`prepare` 也只對已驗證為 Wi-Fi、非 AP mode 的 UUID 套用 powersave 2、autoconnect 與不限重試，不碰 VPN／乙太網路 profile。
- `rg40xxv-network-prepare.service` 在 dbus／NetworkManager 後執行，固定解除 NetworkManager mask、enable/start、解除 Wi-Fi rfkill，再發布 `/run/rg40xxv/network/snapshot.v1`。multi-user 會啟動它；time-sync 明確排在它之後。UI 只 `Wants=`、不加 `After=`，因此網路準備不會阻塞首幀。
- snapshot 是 root-owned `0600`、原子 rename 的有界格式，最多 32 個 AP；SSID/security 以 percent encoding 傳輸。UI 只接受 canonical BSSID／UUID，提供掃描、AP 選擇、WPA 密碼鍵盤、連線、中斷、忘記及 hotspot 切換／狀態。
- 密碼不放在 UI helper 或 nmcli argv。UI worker 以匿名 pipe 傳給 `ui-hardwarectl`，後者原樣交給 network helper；新 WPA-PSK 連線再用 `nmcli --ask` 從 stdin 讀取。測試同時檢查 capture 與 log 都沒有密碼。
- `connect` 目前支援開放網路與 WPA-PSK；WEP 與 enterprise/EAP 明確拒絕，不冒充成功。hotspot 使用固定 connection name／SSID，由 NetworkManager 產生並保存其 PSK；UI 本版只做切換與狀態，不把 PSK 寫入日誌。
- helper 不是通用 nmcli wrapper，不接受自訂 interface、connection name、命令或 shell 字串；介面固定為 `wlan0`，hotspot 固定為 `rg40xxv-hotspot`。

## 建置與驗證

```sh
cd services/device-control
./build.sh check
./build.sh test
./build.sh package
```

- `check`：執行 `bash -n`；若系統有 shellcheck 也會執行 shellcheck。
- `test`：只在 `/tmp/rg40xxv-device-control-test.*` 建立假 configfs、假 rootfs 與假存檔，不接觸 `/mnt`、`/var` 或實機。
- `package`：在 `build/` 產生 staging tree 與 `.tar.gz`，不執行 `systemctl enable`、不複製到根目錄。

## 正式整合前的必要條件

1. 在實機核心執行 `check-kernel-config.sh`，並確認 H700 的 USB OTG controller 以 peripheral/OTG 模式出現於 `/sys/class/udc`。
2. 確認 Type-C 連接埠的 DTS role-switch／extcon 設定；只有充電線時不會枚舉資料功能。
3. 測試 Windows、Linux host 的 RNDIS 與 ACM 枚舉，再決定是否啟用 networkd DHCP。
4. 系統映像另外設定 SSH policy。這個套件不寫入 root 密碼，也不更改 `sshd_config`。
5. 在切換任何模擬器核心前先執行 `save-guard backup-before-core-switch`，並以實際遊戲做存檔、退出、重新載入驗證。
6. 電源功能先確認 DRM runtime PM、背光 class 與 joystick LED class 名稱，再跑 20 輪短按鎖定／喚醒；不要把本 helper 誤稱為 suspend。
7. 實機確認 backlight、LED 與 UDC class entry 的 `realpath -e` 都落在同一 `/sys/devices/` 下，並跑 20 輪開關、亮度與 USB bind/unbind。helper 不接受指向其他 mount、`/tmp` 或任意 canonical 路徑的 class symlink。
8. CPU policy 先以 `validate` 確認 `schedutil`、`performance` 與最低 480 MHz OPP；遊戲 launcher 必須使用 `run-performance -- ...`，不可自行寫 governor 而漏掉退出還原。
9. VPN 正式整合要安裝 OpenVPN/nftables，確認 endpoint 與 `.ovpn` remote 一致，再以 LAN SSH 連線做 kill-switch 中斷／重連測試；切勿先開 kill switch 再補 endpoint。
10. 音量服務上機前確認 card 0 ID、七個 control 的 type/range 與 `gpio-keys-volume` name/capability，再跑喇叭及耳機各 20 輪 0/5/55/100、mute、長按 autorepeat；fixture 數據不是實機音壓或 latency 驗證。
11. 網路 hotfix 上機後確認 `NetworkManager` 未被 charge-mode launcher 再次 mask、`iw dev wlan0 get power_save` 為 off，並做 idle 10 分鐘、AP scan/connect/disconnect/forget/hotspot、SSH 及 mDNS multicast 20 輪；host fixture 不能證明 rtw88 真機 RF/firmware 穩定性。
