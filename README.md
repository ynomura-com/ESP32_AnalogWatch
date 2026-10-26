# ESP32 + GC9A01 円形ディスプレイで、アナログ時計

文字盤は次の2種類が選べます
![](images/ESP32_AnalogWatch1.png)

* 黒背景・水色の大きな数字・外周の分表示・▼マーク・ロゴ枠
* スイスの鉄道時計風のデザイン（白背景）
`PlatformIO` の環境`(env)`で切り替えられます。

## 文字盤の切り替え方
* `platformio.ini` に `[env:mhetesp32minikit]` と `[env:railway]` があります。
* `VS Code` 下部の環境選択(または `pio run -e railway -t upload`)で切り替えられます。
* * `mhetesp32minikit` を選ぶ（ディフォルト）と、黒背景の青い文字盤です。
* `railway` を選んで書き込むと、スイスの鉄道時計風の文字盤になります。
* 起動ログの `[BOOT] watch face:` に、どちらの文字盤か表示されます。

## 配線(GC9A01 → ESP32 MiniKit)
配線（ESP32のピン番号は PlatformIO.ini で設定できますので、やりやすい配線に合わせて書き換えて使用してください。

例えば
```cpp
    -DTFT_MOSI=17    // SDA
    -DTFT_SCLK=16    // SCL
    -DTFT_CS=22
    -DTFT_DC=21
    -DTFT_RST=27
    -DTFT_BL=32      // 配線なし
```
　VCC には `3.3V`、`GND`は`GND`に配線します。

 ## 調整ポイント

* 色が赤と青で逆になる場合: `platformio.ini` の `-DTFT_RGB_ORDER=TFT_BGR` のコメントを外します。
* 数字の縦位置がずれる場合: `NUM_DY` を調整します。
* 秒針の動き: `SMOOTH_SECOND` で、滑らかに動かすか、1 秒ごとにカチッと動かすかを切り替えます。
* 描画が重い場合: `SPI_FREQUENCY` を `80000000` に上げられます。表示が乱れたら `40000000` に戻してください。
* 向きが違う場合: `tft.setRotation()` を変えます。

## 文字盤を調整したい場合

* 時針・分針の太さは `drawClockStripBlue` 内の `drawHand(hDeg, 52, 10, 9.0f, TIP_W, …)` と `drawHand(mDeg, 86, 12, 7.0f, TIP_W, …)` の 4 番目の数値です。
* 秒針の太さは同じ関数内の `drawHand(sDeg, SEC_TIP_R, 26, 2.0f, 2.0f, …)` の `2.0f` です。
* 針が細すぎる、または円が大きすぎると感じたら、`fillSmoothCircle` の半径`(4)`を `3` にするのも手です。
