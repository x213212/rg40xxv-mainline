# RG40XX V Shell

這是獨立於原廠 `dmenu.bin` 的 SDL2 主畫面原型。它只讀 ROM、封面、route
與狀態來源；本目錄的程式不修改 bootloader、啟動槽或實機 sysfs，也不會呼叫
`reboot`、`shutdown` 或全域 `sync`。

## 已實作

- 640×480、zh_TW 預設／English 即時切換的黑灰白直角線條 UI；
- 日期時間固定左上，Wi-Fi／電池／音量固定右上；未知硬體值顯示「無法取得」，不造假；
- 啟動優先載入 owner-only 的 catalog v2 snapshot；無 snapshot 時前景掃描上限
  300 ms／2,048 entries，首幀呈現後才啟動背景刷新。snapshot 保存各平台
  dev／inode／mtime／ctime／entry-count；未變平台只做根目錄 fingerprint、零 ROM
  掃描，變更平台才單獨重掃並 atomic merge，有限額結果保存下一平台 cursor；
- 遊戲庫固定先讀 TF1 `/mnt/mmc/Roms`；只有 `/mnt/sdcard` 出現在目前 mount
  namespace 且 `/mnt/sdcard/Roms` 是真實目錄時，才把 TF2 同平台內容追加到記憶體
  catalog。相同相對路徑（大小寫不拘）固定保留 TF1，scanner 不複製、移動或改寫
  ROM。持久 snapshot 只保存 TF1，確保 TF2 拔除後不會留下舊項目；session 可用
  `RG_TF2_ROM_ROOT` 覆寫傳給 UI 的 `--optional-rom-root`，但不負責掛載；
- `EASYRPG` 只列出頂層專案目錄內直接存在的實體 `RPG_RT.ldb`；缺檔、symlink
  或跨檔案系統 marker 不會冒充可玩的 RPG Maker 2000/2003 項目，完整來源樹仍由
  啟動器在建立唯讀 COW view 前再次 fail closed 驗證；
- 五張直式海報、180 ms 選取動畫、選中海報向前放大，以及左下三欄
  platform／frontend／runtime 資訊；
- `SDL2_image` 背景解碼、selected-first 的 ±2 預取、最多五張 pinned GPU texture
  LRU；generation 變更會取消舊工作，texture 只在主執行緒 upload，replacement
  成功前保留舊 texture；缺圖只畫
  灰階線條 placeholder；解碼前會檢查 PNG／JPEG／BMP header，拒絕超過
  4096×4096、4,194,304 像素、16 MiB 預估解碼量或 12 MiB 檔案，壞圖與超限圖
  直接落到 placeholder。160×232 上限的 ARGB 縮圖以 source path＋mtime＋size
  為 key 寫入 owner-only disk cache，先寫 temporary file 再 atomic rename；
- 搜尋、實體 `SDL_TEXTINPUT`、controller OSK、系統／收藏／最近／core 組合篩選；
- evdev 左右類比搖桿會依 `EVIOCGABS` 回報的範圍、中心與 flat 建立按下／放開
  hysteresis 並控制選單；HAT 與 D-pad 仍保留獨立輸入路徑；
- 啟動與遊戲返回後會先快照按鍵、D-pad、HAT 與左右搖桿；所有控制持續回到中立
  120 ms 前，不接受啟動、導覽或全域捷徑，避免殘留 A 鍵重複啟動；音量／電源服務
  與獨立的 MENU+START supervisor 不會被 grab 或改寫；
- canonical ROM path 收藏與原廠 `/mnt/data/misc/.favorite` 匯入；
- `compat/platform-routes.json` 動態 route；PORTS 只列頂層 `.sh`，不掃相鄰資料；
- system information 顯示台灣時間、kernel、RAM、Wi-Fi 連線／訊號、電池／
  充電狀態、背光與 ALSA 音量；`/proc`、`/sys` 與 ALSA 皆以 bounded、
  nonblocking、唯讀方式擷取，缺值明確顯示「無法取得」；CPU 電壓只接受真實
  regulator `microvolts` 或具 CPU label 的 hwmon `in*_input`，不把 OPP target
  冒充實測值；狀態列的「校時中／時間已校準／時間未校準」只描述 NTP 時鐘
  狀態，30 秒未完成即明確顯示未校準，不是遊戲存檔同步進度；
- `--hardware-root`（別名 `--fixture-root`）可把所有唯讀硬體來源注入同一個
  fixture root；fixture 模式不會 fallback 到宿主的時間、kernel 或 ALSA；
- `--state-dir`（預設 `/var/lib/rg40xxv/netstream`）使用 netstream v1 安全 host
  store；串流 worker 只在首次開啟頁面時啟動，先發布持久 snapshot，再於背景以
  1.5 秒上限掃描 `_nvstream._tcp.local`。掃描不可用時明確標為 `PENDING`，既有
  host 仍可使用；UI thread 只複製 bounded snapshot，不等待 socket、pair 或磁碟；
- 串流頁可切換多台 Sunshine 主機，非阻塞調整並原子保存解析度、bitrate、codec；
  未配對時 A 會產生非 `0000` 的四位固定 PIN，以固定 argv 呼叫 pair runner，只有
  runner 回顯同一 PIN 且確認成功才保存 paired。Moonlight certificate/keydir 位於
  `$RG_STATE_ROOT/moonlight/keys`，不載入或顯示 Wi-Fi profile；
- `--stream-launcher` 預設 `/opt/rg40xxv/bin/rg40xxv-stream`；只有 runner 是可執行
  regular file、host 已配對且 netstream 驗證通過才會啟動。目前 runner gate 為
  H.264，固定 argv contract 是
  `stream HOST WIDTH HEIGHT FPS BITRATE h264 ASPECT [APP]`，不經 shell／eval；未指定
  APP 時先做最長 8 秒的 app list，依 `pc`、`Desktop`、第一個合法 app 選擇，避免
  假設 Sunshine 一定提供 Desktop；
- APPS 導覽是獨立 view，只列出 APPS 掃描項；它不覆寫既有系統／核心／收藏／
  最近／搜尋篩選，離開後會恢復原 view，A 也不可能誤送目前遊戲項；
- APPS 只有一個 YouTube Native texture tile，沒有 Web/Cog fallback。v3 admission
  固定 `evidence_scope=COMPONENT_GATE`，必須指向 release-owned、不可由 group/world
  改寫的 executable launcher，且 controller、
  resolver、range bridge、H.264/AAC、DRM/KMS、ALSA、input、session-return 九個核心 gate
  都是 `PASS` 才承認 route。唯一尚未完成的 memory-budget 可為 `UNVERIFIED`：tile 會以
  `VERIFY` 標示並允許使用者驅動的驗證；component admission 永不宣告 final device
  acceptance；`FAIL`、缺欄、格式錯誤或 launcher 不安全時仍
  顯示 `UNAVAILABLE` 且不可啟動，不把未驗證狀態冒充完整 device PASS；
- `--resident` 下 B 與 `SDL_QUIT` 不拆 SDL/KMS；但遊戲啟動會原子寫入固定格式的
  handoff request，完整停止 worker 並釋放 cover cache、font、renderer、SDL/KMS 後
  以保留狀態退出；session supervisor 驗證 request、前景等待遊戲，再啟動全新的 UI；
  一般 windowed/screenshot 可退出；
- 一次性 launch request 位於 systemd 管理的 `/run/rg40xxv-session` tmpfs，不讓
  `/mnt/data` 的 file/directory fsync 卡住遊戲 handoff；遊戲結束後只對當次變更的
  reviewed save roots 做 `fdatasync`／directory `fsync`，舊 APP/PORT 的裸 `sync`
  最多等候 300 ms，較慢的 durability pass 留在背景；
- power/lock 純狀態機：短按熄屏、可選三連按解鎖、3 秒長按倒數與 callback hooks；
  screen-off 主迴圈停止 render；KMSDRM 沒有 SDL 原生阻塞 wait 時直接以 `poll(2)`
  等待 gamepad／AXP 電源鍵 fd，不退化成 SDL 每 1 ms 忙輪詢；
- 設定頁的背光 0–100、搖桿燈亮度 0–100、USB 偵錯、熄屏／喚醒與正常關機，
  透過單一序列 worker 呼叫 `/usr/sbin/ui-hardwarectl`；只傳固定 argv 與乾淨環境，
  不使用 shell／eval。helper 最長執行 5 秒，逾時會對獨立 process group 依序 TERM／
  KILL 並完整 reap；紀錄檔以 `O_NOFOLLOW` 開啟且只接受 regular file。背光 0 仍保留
  helper 的面板安全最低值，不等同熄屏；未有穩定 helper contract 的偵錯紀錄匯出／
  清除會明確顯示無法使用；
- 網路頁已接到真實 NetworkManager 後端：A 進入 Wi-Fi 基地台清單並背景掃描，
  未保存 WPA 網路會開啟遮蔽密碼鍵盤，Y 可中斷／忘記，個人熱點可切換；status、
  scan、connect、disconnect、forget 全部使用同一序列 worker 與有界 timeout，
  nmcli 不會跑在渲染執行緒。藍牙頁顯示真實 adapter BD address，並逐項保留多個
  裝置的 paired／trusted／connected 狀態；A 配對／連線／斷線、X 掃描／忘記；
- 設定頁「一般重啟」固定呼叫 `ui-hardwarectl reboot-custom`，後端固定再呼叫
  `rg40xxv-reboot-target custom`；UI 不提供把一般重啟誤送到 stock、fastboot 或
  poweroff 的參數。持久設定會寫入並讀回 `boot_target=custom`；
- 收藏、篩選、語言與鎖定設定用 200 ms debounce worker 做 `fsync` + atomic rename；
- 硬體讀取在低優先 worker：時間／電池／CPU／RAM 每秒，Wi-Fi 每三秒；
- 啟動時預合成玻璃音效，輸入 frame 不做三角函數音訊合成。

新酷音採 backend contract：候選請求必須非阻塞，結果經 UI thread 回送。當前 rootfs
沒有 `libchewing`，所以 production UI 明確顯示「新酷音未安裝・僅英文後備」，不把
測試字典冒充可用中文輸入。密碼欄 contract 僅接受 ASCII、提供遮蔽輸出，且不保存
history。

## 原始碼

`src/main.c` 是 canonical entry；舊單檔版本保留為 `legacy-prototype.c.txt`，
`build.sh` 不會編譯它；功能依 catalog、render、硬體 worker 與啟動生命週期分模組。

主要模組：

- `catalog_scan.c`, `catalog.c`, `search.c`, `favorites.c`
- `platform_routes.c`, `cover_cache.c`, `text.c`
- `render.c`, `render_scene.c`, `render_lock.c`, `system_info.c`
- `input.c`, `keyboard.c`, `input_method.c`, `audio.c`
- `hardware.c`, `monitor.c`, `settings.c`, `settings_ui.c`, `stream.c`,
  `stream_backend.c`
- `persistence.c`, `metrics.c`, `power.c`, `power_ui.c`

## 建置與驗證

從 workspace 根目錄使用唯一入口；不要手動維護一份不完整的測試清單：

```sh
cd /root/kernel
tools/rg40xxv-p7-ui-pipeline.sh version  # 約 0.1 秒，只核對版本與 SHA
tools/rg40xxv-p7-ui-pipeline.sh build    # 約 12 秒，只編 UI
tools/rg40xxv-p7-ui-pipeline.sh quick    # 約 43 秒，編譯＋12 個關鍵 gate
tools/rg40xxv-p7-ui-pipeline.sh full     # 兩次重編＋全部 UI/payload host gate
tools/rg40xxv-p7-ui-pipeline.sh release  # full 通過後才封 next-v1 p7 release
```

每次執行都把開始／結束時間、逐步 wall time、timeout、exit code、log、完整編譯輸入
manifest、UI SHA-256、ELF Build ID 與 release contract 寫進
`reports/p7-ui-runs/<timestamp>/`，再產生 `EVIDENCE-SHA256SUMS`。腳本不連裝置、
不部署，也不寫 p8。

`quick` 固定涵蓋 layout、frame scheduler、input navigation、APPS handoff、Wi-Fi、
Bluetooth、settings、640×480 串流退出 contract、RPG、YouTube native candidate、硬體 fixture 與
resident handoff。`full` 會依檔名排序後，以 `sh` 執行 UI tests、以 `bash` 執行
payload tests；不可平行執行，避免固定 fixture／暫存路徑互撞。

目前 production identity 在 `PRODUCTION-IDENTITY.env`。Host/QEMU PASS 只證明建置、
ABI、狀態機與 fixture；不得把 `PENDING_DEVICE`、`UNVERIFIED` 或既有實機 FAIL 改寫
成 PASS。

若要讓 bundled subset font 涵蓋實際遊戲檔名與 route label：

```sh
RG40XXV_UI_ROM_ROOT=/mnt/u/GAME_BACKUP/Roms \
RG40XXV_UI_PLATFORM_ROUTES=/path/to/platform-routes.json \
./subset-font.sh
```

腳本會收錄介面、catalog 名稱（排除 `Imgs` 與 PORTS 相鄰資料樹），並保留完整
基本 CJK 字集。這讓後續加入的簡體 ROM／遊戲名稱仍有字形，不會因建置當下的
catalog 未出現該字而顯示 tofu。保留原生尺寸 hinting、去除垂直排版表後以
12 MiB 為資產上限；SDL_ttf 使用 normal hinting，所有文字以整數像素 1:1 raster。

QEMU dummy 的完整 fixture 動畫測試：

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy qemu-aarch64-static \
  -L ../../firmware/mnt/rootfs build/rg40xxv-shell \
  --windowed --font assets/RG40XXV-UI-Sans.otf \
  --rom-root /mnt/u/GAME_BACKUP/Roms \
  --platform-routes ../../lab/emulators/aarch64-staging/compat/platform-routes.json \
  --benchmark --demo-ms 10000
```

輸出包含 frame p50/p95/p99/max、超過 16.7 ms 的 frame 數、
input-to-present 最大值、catalog source/scan time，以及 cover stale/cancel/eviction、
decode peak、disk hit/miss/write、texture create/destroy/bytes 與 peak RSS。首次完整
catalog 建置在首幀後背景進行，後續未變平台不再掃 ROM。A 鍵已透過固定 argv 的
`posix_spawn` 接到 `--launcher`，遊戲使用獨立 process group。非 resident 測試模式會
釋放 SDL/KMS、等待子程序、在 TERM/INT 時清理整組程序，最後恢復畫面與輸入；
resident production 模式則不在同一個 UI process 內 spawn 遊戲，而由 session
supervisor 等待舊 UI 完整 teardown 並退出後啟動 launcher，遊戲結束才建立新的 UI
process。按 A 後不使用固定延遲：UI 先 present 一幀單色線條／波浪的「正在啟動」
畫面，再進入對應 handoff。非 resident 返回層以 tick 到期，持續處理輸入且不 sleep；
非零退出、訊號與 spawn 錯誤會顯示在返回層並保留啟動紀錄。production 仍須在實機
逐平台驗證存檔 round-trip，尚未通過 mainline7 route 的平台會 fail closed。已配對
串流的 A 鍵仍沿用同 process 的 present／SDL-KMS handoff／process-group／恢復流程；
未配對時只在 worker 中執行 fixed-PIN pairing，不釋放 KMS。無效 host、非 H.264 或
runner 不可執行也會留在 UI 顯示繁中錯誤。
