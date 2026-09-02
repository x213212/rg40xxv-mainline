# RG40XX V 藍牙 runtime candidate

這個元件把既有 UI 的固定 helper 介面接到 stock rootfs 的 BlueZ 5.66。
`rg40xxv-bluetooth-control` 直接使用 system D-Bus 讀取 adapter/device 狀態，
並實作掃描、開關、連線、斷線與移除；snapshot v2 同時列出 adapter 的真實
BD address，以及每個裝置的 paired／trusted／connected 狀態，因此可在同一頁
保留多個已配對裝置，不會用單一布林值假裝配對清單。配對使用固定絕對路徑的
`bluetoothctl --timeout 90 pair MAC` 取得 BlueZ agent 流程，成功後再透過
D-Bus 設定 `Trusted=true`。所有 MAC 都先正規化，沒有 shell 字串拼接。

systemd payload 會解除 Bluetooth rfkill、啟動 stock `bluetooth.service`，
並把 `/var/lib/bluetooth` bind 到 p7 的
`/mnt/data/rg40xxv/state/bluetooth`，保留配對資料。UI drop-in 只有非阻塞的
`Wants=`，刻意沒有 `After=`／network／udev ordering，因此藍牙缺席或初始化
較慢都不會延後第一幀；競態期間的 helper 呼叫由 BlueZ D-Bus alias 啟動服務。

兩個 RTL8821CS firmware 檔會放入 rootfs overlay。實機證據顯示 mainline
serdev 第一次 probe 發生在 p7 overlay 可見之前，所以第一次 firmware request
會失敗並留下零 BD address。`rg40xxv-bluetooth-hci-ready.service` 會在 overlay
可見後先核對兩個固定 SHA-256；只有 `hci0` 仍是零位址／零 ACL MTU 時，才對
`hci_uart_h5` 的精確 serdev child `serial0-0` 做一次 unbind/bind，接著要求
`hci0 up`，並在真實非零 BD address 與 ACL MTU 都可讀後才讓 bluetooth.service
繼續。健康的第二次執行不會重綁 UART。這條修正沿用目前核心已存在的
`hci_uart_h5`，不需要為韌體載入另外重編核心。
封裝同時從鎖定的 linux-firmware commit 取出並驗證
`LICENCE.rtlwifi_firmware.txt`，保留 Realtek firmware 的再散布聲明。

`admission.env` 的 `status=PASS` 只代表 helper/output contract 已通過主機端
測試並可供下一次真機驗收，不代表硬體 Device PASS。元件鎖檔明確保留
`device_validation=PENDING`；在實機完成 firmware reprobe、非零 BD address、
scan/pair/trust/connect/disconnect/forget
與暖重啟保存測試前，不得宣告 Bluetooth Device PASS。
