# RG40XXV 網路／串流持久設定 backend

這個目錄提供獨立、host 可測的 C library 與 `netstreamctl`。它只保存多筆 Wi-Fi profile 與 Sunshine／Moonlight host 設定，不會連網、不會呼叫 shell、`nmcli` 或 Moonlight，也不會寫入 NetworkManager、Windows 或 RG40XXV 實機設定。

## 建置與測試

```sh
services/netstream/build.sh
services/netstream/build.sh test
```

產物位於 `services/netstream/build/`：`libnetstream.a`、`netstreamctl` 與（執行 test 時）`test_netstream`。若要在 host 跑 ASan/UBSan，可用：

```sh
NETSTREAM_SANITIZE=1 services/netstream/build.sh test
```

交叉編譯可指定 `NETSTREAM_CC` 與 `NETSTREAM_AR`。backend 僅依賴 libc/POSIX API。

## 安全模型

- 預設狀態目錄是 `/var/lib/rg40xxv/netstream`。也可用 `--state-dir` 指定測試目錄；最終目錄必須由目前 effective UID 擁有且 mode 0700。
- `wifi.v1`、`hosts.v1`、`.lock` 都固定 mode 0600；load 時拒絕 symlink、hard link、非 regular file 與不同 owner。
- 寫入採同目錄 temp file、`fsync`、atomic `renameat`、目錄 `fsync`。程序間用 `flock` 序列化。
- 密碼只能由 stdin 或受限檔案讀入。沒有 `--password PASSWORD` 參數，因而不會把秘密放進 process argv。密碼檔須為絕對路徑、目前使用者所有、regular file、非 symlink/hard link，且不得有 group/other 權限（建議 0600；0400 亦接受）。
- `wifi list` 完全不輸出 secret；錯誤與匯入訊息也不包含 SSID/密碼內容。host address 直接進入型別化資料結構，絕不拼入 shell command。
- 這是檔案權限保護，不是加密。root、離線讀取儲存媒體或已取得同 UID 的程序仍可讀取明文 secret。

## Wi-Fi 操作

先建立狀態目錄（正式映像通常由安裝／啟動層建立）：

```sh
install -d -m 0700 /var/lib/rg40xxv/netstream
```

以 stdin 新增 profile 時，應由 UI／credential helper 透過 pipe 或匿名 FD 餵入下列命令；不要把密碼字面值寫進 shell history。為避免終端回顯洩漏，backend 會拒絕直接連到 TTY 的 password stdin：

```sh
services/netstream/build/netstreamctl wifi set \
  --ssid '家用網路' --security wpa2-psk \
  --password-stdin --make-default
```

由 UI 或 credential helper 事先建立 mode 0600 的檔案時，可改用：

```sh
services/netstream/build/netstreamctl wifi set \
  --ssid '家用網路' --security wpa3-sae \
  --password-file /run/rg40xxv/wifi-secret
```

其他 CRUD：

```sh
netstreamctl wifi list
netstreamctl wifi set --ssid Guest --security open --priority -10
netstreamctl wifi select --ssid Guest
netstreamctl wifi forget --ssid Guest
```

`set` 對同 SSID 是欄位更新；既有加密 profile 未提供新 password source 時保留原 secret。切換為 `open` 會清除該 profile 的 secret。`forget` 只刪指定 SSID；找不到時整個操作失敗，沒有 wildcard、全刪或 `rm system-connections/*` 行為。刪除目前預設 profile 只會清空 default 指標，不影響其他 profile。

SSID 必須是 1..32 bytes 的有效、無控制字元 UTF-8，可使用繁體中文。安全模式限 `open`、`wpa2-psk`、`wpa3-sae`；WPA passphrase 規則詳見 [FORMAT.md](FORMAT.md)。

## 匯入原廠 `/mnt/data/.wifi`

原廠 binary 使用一行一筆的 `S:<ssid><TAB>P:<password>` 明文格式。backend 可唯讀匯入一筆或多筆：

```sh
netstreamctl wifi import-vendor --file /mnt/data/.wifi --make-default
```

匯入是 all-or-nothing：任何 malformed record 都不更新資料庫；成功時只輸出筆數，不輸出 SSID 或 password，也不修改來源檔。

原廠常見的 `/mnt/data/.wifi` 是 **mode 0644 明文密碼**。這代表裝置上任何本機帳號／程序皆可讀取；匯入到 mode 0600 的 `wifi.v1` 並不會修復仍留在原路徑的副本。CLI 偵測 group/other readable 時會警告，但本 backend 不會自行 chmod 或刪除原檔，應由後續、明確授權的 migration／boot 整合層處理。

## Sunshine／Moonlight host 操作

最小新增命令只需名稱與 address；會套用 `640x480`、`60 fps`、`H264`、`fit`：

```sh
netstreamctl host set --name '客廳電腦' \
  --address sunshine-pc.local --make-default
```

完整更新範例：

```sh
netstreamctl host set --name '客廳電腦' \
  --paired true --last-used 1777777777 \
  --width 1024 --height 600 --custom true \
  --fps 120 --bitrate 42000 --packet-size 1200 \
  --codec H265 --aspect fill
netstreamctl host list
netstreamctl host select --name '客廳電腦'
netstreamctl host delete --name '客廳電腦'
```

Host name 是 1..96 bytes 的有效、無控制字元 UTF-8，可使用繁中；address 僅接受 IPv4、IPv6 或嚴格 ASCII DNS hostname，不接受 URL、port、空白或 shell metacharacter。

| 欄位 | 預設 | 驗證 |
|---|---:|---|
| `paired` | `false` | boolean |
| `last_used` | `0` | Unix seconds，0..4102444800 |
| `resolution` | `640x480`, `custom=false` | 160x120..7680x4320；非內建 preset 須 `custom=true` |
| `fps` | `60` | 1..240 |
| `bitrate_kbps` | `5000` | 500..200000 |
| `packet_size` | `1392` | 256..1500 |
| `codec` | `H264` | `H264`、`H265`、`AV1` |
| `aspect` | `fit` | `fit`、`fill`、`stretch` |

內建非 custom resolutions 是 640x480、1280x720、1920x1080、2560x1440、3840x2160。更新時省略的欄位保留原值；`--width` 與 `--height` 必須成對出現。刪除目前預設 host 會清空 default 指標。

## API、schema 與限制

- 公開 API：[include/netstream.h](include/netstream.h)
- 磁碟格式：[FORMAT.md](FORMAT.md)
- 邏輯 JSON schema：[schema/netstream-v1.schema.json](schema/netstream-v1.schema.json)
- unit tests：[tests/test_netstream.c](tests/test_netstream.c)
- CLI security tests：[tests/test_cli.sh](tests/test_cli.sh)

本輪刻意沒有 NetworkManager profile writer，因此沒有 `.nmconnection` 輸出；未來若新增，必須延續 0600、`openat(O_NOFOLLOW)`、固定／不可 traversal 檔名與 atomic commit 規則。本輪也沒有連線、配對、Moonlight 啟動、Windows 設定同步、實機部署或原廠檔案清理功能。
