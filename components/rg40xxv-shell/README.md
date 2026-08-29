# RG40XX V Shell

這是獨立於原廠 `dmenu.bin` 的 SDL2 主畫面原型。它只讀 ROM、封面、route
與狀態來源；本目錄的程式不修改 bootloader、啟動槽或實機 sysfs，也不會呼叫
`reboot`、`shutdown` 或全域 `sync`。

## 已實作

- 640×480、zh_TW 預設／English 即時切換的黑灰白直角線條 UI；
- 日期時間固定左上，Wi-Fi／電池／音量固定右上；未知硬體值顯示「無法取得」，不造假；
- 啟動時掃描 `--rom-root`（預設 `/mnt/mmc/Roms`），忽略 `Imgs`；封面對應
  `<system>/Imgs/<ROM basename>.png`；
- 五張直式海報、180 ms 選取動畫、選中海報向前放大，以及左下三欄
  platform／frontend／runtime 資訊；
- `SDL2_image` 背景解碼、selected ±2 預取、最多五張 GPU texture LRU；缺圖只畫
  灰階線條 placeholder；解碼前會檢查 PNG／JPEG／BMP header，拒絕超過
  4096×4096、4,194,304 像素、16 MiB 預估解碼量或 12 MiB 檔案，壞圖與超限圖
  直接落到 placeholder；
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
  冒充實測值；
- `--hardware-root`（別名 `--fixture-root`）可把所有唯讀硬體來源注入同一個
  fixture root；fixture 模式不會 fallback 到宿主的時間、kernel 或 ALSA；
- `--state-dir`（預設 `/var/lib/rg40xxv/netstream`）直接讀取 netstream v1
  安全 host store；串流頁可切換多台 Sunshine 主機並顯示配對、解析度、FPS、
  bitrate、codec 與比例，但完全不載入 Wi-Fi profile；
- `--stream-launcher` 預設 `/opt/rg40xxv/bin/rg40xxv-stream`；只有 runner 是可執行
  regular file、host 已配對且 netstream 驗證通過才會啟動。目前 runner gate 為
  H.264，固定 argv contract 是
  `stream HOST WIDTH HEIGHT FPS BITRATE h264 ASPECT`，不經 shell／eval；
- APPS 導覽是獨立 view，只列出 APPS 掃描項；它不覆寫既有系統／核心／收藏／
  最近／搜尋篩選，離開後會恢復原 view，A 也不可能誤送目前遊戲項；
- `--resident` 下 B 與 `SDL_QUIT` 不拆 SDL/KMS；一般 windowed/screenshot 可退出；
- power/lock 純狀態機：短按熄屏、可選三連按解鎖、3 秒長按倒數與 callback hooks；
  screen-off 主迴圈停止 render 並等待事件；
- 設定頁的背光 0–100、搖桿燈亮度 0–100、USB 偵錯、熄屏／喚醒與正常關機，
  透過單一序列 worker 呼叫 `/usr/sbin/ui-hardwarectl`；只傳固定 argv 與乾淨環境，
  不使用 shell／eval。helper 最長執行 5 秒，逾時會對獨立 process group 依序 TERM／
  KILL 並完整 reap；紀錄檔以 `O_NOFOLLOW` 開啟且只接受 regular file。背光 0 仍保留
  helper 的面板安全最低值，不等同熄屏；未有穩定 helper contract 的偵錯紀錄匯出／
  清除會明確顯示無法使用；
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
- `hardware.c`, `monitor.c`, `settings.c`, `settings_ui.c`, `stream.c`
- `persistence.c`, `metrics.c`, `power.c`, `power_ui.c`

## 建置與驗證

```sh
./build.sh
./preview.sh
sh tests/test-power.sh
sh tests/test-input-method.sh
sh tests/test-input-latch.sh
sh tests/test-input-navigation.sh
sh tests/test-texture-lifetime.sh
sh tests/test-stream-ui.sh
sh tests/test-apps-filter.sh
sh tests/test-apps-ui.sh
sh tests/test-cover-limits.sh
sh tests/test-cover-ui.sh
sh tests/test-system-hardware.sh
sh tests/test-settings-worker.sh
sh tests/test-settings-ui.sh
```

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

輸出包含 frame p50/p95/p99/max、超過 16.7 ms 的 frame 數與
input-to-present 最大值。首次 catalog 全掃只在啟動時發生。A 鍵已透過固定 argv 的
`posix_spawn` 接到 `--launcher`，遊戲使用獨立 process group；UI 會釋放 SDL/KMS、
等待子程序、在 TERM/INT 時清理整組程序，最後恢復畫面與輸入。按 A 後不使用固定
延遲：UI 先 present 一幀單色線條／波浪的「正在啟動」畫面，才在同一輪釋放
SDL/KMS 並 spawn；返回層以 tick 到期，持續處理輸入且不 sleep。非零退出、訊號與
spawn 錯誤會顯示在返回層並保留啟動紀錄。production 仍須在實機逐平台驗證存檔
round-trip，尚未通過 mainline7 route 的平台會 fail closed。串流 A 鍵沿用同一套
present／SDL-KMS handoff／process-group／恢復流程；未配對、無效 host、非 H.264 或
runner 不可執行會直接留在 UI 顯示繁中錯誤，不釋放 KMS。
