# USBカメラ用 udev ルール作成ガイド(汎用版)

PX System の PIR カメラ(Night/Pattern-C)を例に、USBカメラへ**安定した固定シンボリックリンク**を割り当てる udev ルールの作り方をまとめる。`/dev/videoX` の番号は起動順・USBポート順・再接続で変動するため、ルールで固定名(例: `/dev/video/pir_camera`)を与えることで、launch やコード側のデバイスパスを変えずに運用できるようにするのが目的。

このガイドの手順は他の USB カメラ(RGBカメラ、Webカメラ等)にもそのまま応用できる。変えるのは **idVendor / idProduct / シンボリックリンク名** の3点だけ。

---

## 全体の流れ

1. デバイス情報を収集する(VID/PID、capture ノードの特定)
2. udev ルールファイルを書く
3. ルールを設置して反映する
4. シンボリックリンクが正しく張られたか検証する

---

## 1. デバイス情報の収集

### 1-1. Vendor ID / Product ID を確認

```bash
lsusb
```

対象カメラの行から `xxxx:yyyy` 形式の `VendorID:ProductID` を読み取る。
(PIRカメラの例: `HD USB Camera` → `32e4:9230`)

### 1-2. デバイスノードを確認

```bash
v4l2-ctl --list-devices
```

対象カメラに割り当てられた `/dev/videoX` を確認する。

> **重要:** 1台の UVC カメラは通常 **複数の `/dev/videoX` ノード** を生成する。
> 片方が映像キャプチャ用、もう片方が UVC のメタデータ専用ノードで、
> **メタデータノードは開けない(フォーマット0件)**。
> ルールでは capture ノードだけを掴む必要がある。

### 1-3. capture ノードを特定する

各ノードの対応フォーマットを確認:

```bash
v4l2-ctl --device=/dev/videoX --list-formats-ext
```

- MJPG / YUYV などが**列挙されるノード** → capture ノード(これを使う)
- フォーマットが**何も出ないノード** → メタデータノード(使わない)

属性レベルでの判定は `ATTR{index}` を使う:

```bash
udevadm info --attribute-walk --name=/dev/videoX | grep 'ATTR{index}'
```

- `ATTR{index}=="0"` → capture ノード
- `ATTR{index}=="1"` → メタデータノード

### 1-4. ルール記述に使う属性を一括確認

```bash
udevadm info --attribute-walk --name=/dev/videoX
```

ここから `SUBSYSTEM`, `ATTRS{idVendor}`, `ATTRS{idProduct}`, `ATTR{index}`,
必要なら `ATTRS{serial}` を控える。

---

## 2. udev ルールファイルを書く

`/etc/udev/rules.d/` 配下に `99-<name>.rules` を作成する。
先頭の数値(`99-`)は適用順。大きいほど後で適用され、他ルールの上書きに使える。

### PIRカメラの例(`99-pir_camera.rules`)

```udev
SUBSYSTEM=="video4linux", ATTRS{idVendor}=="32e4", ATTRS{idProduct}=="9230", ATTR{index}=="0", SYMLINK+="video/pir_camera", MODE="0666"
```

各条件の意味:

| キー | 意味 |
|---|---|
| `SUBSYSTEM=="video4linux"` | V4L2 デバイスに限定 |
| `ATTRS{idVendor}` / `ATTRS{idProduct}` | カメラ個体(機種)を特定 |
| `ATTR{index}=="0"` | **capture ノードだけに限定**(メタデータノードを除外) |
| `SYMLINK+="video/pir_camera"` | `/dev/video/pir_camera` を作成 |
| `MODE="0666"` | 全ユーザーが read/write 可能に |

> **`ATTR{index}` が必須な理由:**
> 同一カメラの2ノードは idVendor / idProduct / serial がすべて同一で区別できない。
> `ATTR{index}` だけが capture と metadata を識別できる唯一の属性。
> これを省くと両ノードに symlink が張られ、メタデータノード側を掴んでオープン失敗する場合がある。

### 同一機種を複数台つなぐ場合

VID/PID では個体を区別できないため、`ATTRS{serial}` を追加する:

```udev
SUBSYSTEM=="video4linux", ATTRS{idVendor}=="32e4", ATTRS{idProduct}=="9230", ATTRS{serial}=="<個体のserial>", ATTR{index}=="0", SYMLINK+="video/pir_camera", MODE="0666"
```

(ただし安価な UVC カメラは serial が機種共通の固定文字列のことが多く、その場合は
USBポート位置 `KERNELS=="1-4"` 等で区別する。)

---

## 3. ルールの設置と反映

```bash
sudo cp 99-pir_camera.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=video4linux
```

`setup_udev.sh` を使えば上記 + グループ追加 + 検証まで自動で行える。

---

## 4. 検証

```bash
ls -l /dev/video/pir_camera
```

`/dev/video/pir_camera -> ../videoX` のように capture ノードへのリンクが
出ていれば成功。

capture ノードであることの再確認:

```bash
v4l2-ctl -d "$(readlink -f /dev/video/pir_camera)" --get-fmt-video
```

---

## 5. アプリ側でのオープン時の注意(重要)

`/dev/video/<name>` のような **サブディレクトリ配下 + 非数字** のパスは、
OpenCV の `VideoCapture` がバックエンドを自動判定できず、
GStreamer や CAP_IMAGES へフォールバックして**オープンに失敗する**ことがある。

明示的に V4L2 バックエンドを指定すると確実:

```cpp
cap_.open(device_path, cv::CAP_V4L2);   // C++
```
```python
cap = cv2.VideoCapture('/dev/video/pir_camera', cv2.CAP_V4L2)   # Python
```

(`/dev/videoX` の素のパスなら自動判定が効くため未指定でも開けることがあるが、
固定 symlink 運用では明示指定を推奨。)

---

## 6. Docker で使う場合の落とし穴

ホストの symlink を解決した**実ノード**をコンテナへ渡し、
コンテナ内の名前を **launch / コードが期待する名前に一致させる**こと。

```bash
DEV=$(readlink -f /dev/video/pir_camera)        # 実ノード(例: /dev/video2)を解決
docker run ... --device="${DEV}:/dev/video/pir_camera" ...
```

> コンテナ内デバイス名(`:` の右側)と、launch の `camera_path` は**必ず一致**させる。
> ここがズレるとコンテナ内に該当パスが存在せず、`Failed to open camera` になる。

---

## トラブルシュート早見表

| 症状 | 原因 | 対処 |
|---|---|---|
| symlink ができない | VID/PID 誤り、ルール未reload | `udevadm info` で属性再確認 → reload + trigger |
| symlink はあるが開けない | metadata ノードを掴んでいる | `ATTR{index}=="0"` を追加 |
| `CAP_IMAGES: can't find starting number` | OpenCV のバックエンド自動判定失敗 | `cv::CAP_V4L2` を明示指定 |
| Docker 内で `Failed to open camera` | コンテナ内デバイス名と camera_path 不一致 | `--device` の右側を camera_path に合わせる |
| Permission denied | ユーザーが video グループ未所属 | `usermod -aG video $USER` + 再ログイン、または `MODE="0666"` |
| デバイスが busy | 別プロセスが掴んでいる | `kill_camera_users.sh` で解放 |
