# RG40XXV netstream v1 儲存格式

這是實際磁碟格式的規格；`schema/netstream-v1.schema.json` 則描述同一份資料的邏輯模型，並不表示磁碟檔是 JSON。

## 共同規則

- 狀態目錄必須是絕對路徑、由目前 effective UID 擁有、mode 0700，且路徑不得含 `.`、`..` 或 symlink 元件。
- 固定檔名是 `wifi.v1`、`hosts.v1` 與鎖檔 `.lock`；三者均為 mode 0600 的 regular file，且不接受 symlink 或 hard link。
- 每個檔案以 LF 結尾。欄位以 TAB 分隔。欄位數、版本、數值語法或任何 invariant 不符時，整份檔案拒絕載入，不做部分復原。
- 可變字串先轉成 UTF-8 bytes，再做 percent encoding。`A-Z a-z 0-9 - . _ ~` 原樣保存，其餘 byte 一律為大寫 `%HH`；decoder 也接受合法的大寫 `%HH`，拒絕 NUL、裸空白、裸 `%` 及無效 hex。
- 目前只接受版本 `1`；未知版本 fail closed。
- 預設欄位必須是空字串或精確指向一筆現存、唯一的 record。

## `wifi.v1`

```text
RG40XXV_NETSTREAM_WIFI<TAB>1
D<TAB><encoded-default-ssid>
W<TAB><encoded-ssid><TAB><security><TAB><hidden><TAB><priority><TAB><encoded-secret>
```

- `security`：`open`、`wpa2-psk`、`wpa3-sae`。
- `hidden`：writer 使用 `0` 或 `1`。
- `priority`：十進位 `-999..999`。
- `open` 的 secret 必須空白。WPA2/WPA3 passphrase 為 8..63 bytes 的無控制字元 UTF-8；WPA2 另接受 64 個 hex 字元。
- 最多 64 筆；SSID 以 decoded byte 完全相等判斷唯一，長度 1..32 bytes。

## `hosts.v1`

```text
RG40XXV_NETSTREAM_HOSTS<TAB>1
D<TAB><encoded-default-name>
H<TAB><name><TAB><address><TAB><paired><TAB><last_used><TAB><width><TAB><height><TAB><custom><TAB><fps><TAB><bitrate_kbps><TAB><packet_size><TAB><codec><TAB><aspect>
```

- `name`、`address` 是 percent encoded；其他 enum 與十進位整數不做 encoding。
- `paired`、`custom`：writer 使用 `0` 或 `1`。
- `last_used`：Unix seconds，`0` 表示從未使用，最大值為 `4102444800`（2100-01-01 00:00:00 UTC）。
- `codec`：`H264`、`H265`、`AV1`；`aspect`：`fit`、`fill`、`stretch`。
- 最多 128 筆；decoded name 必須唯一。其餘範圍及預設值見 JSON schema。

## Commit protocol

每次修改都在 `.wifi.v1.tmp.<pid>.<serial>` 或 `.hosts.v1.tmp.<pid>.<serial>` 以 `O_CREAT|O_EXCL|O_NOFOLLOW` 建立 mode 0600 檔案，完整寫入後依序執行：

1. `fsync(temp_fd)`；
2. `renameat(temp, final)`；
3. `fsync(state_dir_fd)`。

過程由 `.lock` 的 `flock(LOCK_EX)` 序列化。任何 rename 之前的錯誤都只清除該次已明確命名的 temp file；程式沒有「刪除所有 profile」或 wildcard remove 路徑。
