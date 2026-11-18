# Point1 修正 検証レポート

## 1. 概要
- **検証対象**: PX System Pattern-C, `OnnxInferenceNode`(px2)のPoint1関連修正
  - 修正①: accumulator/dead-zone不整合バグの解消
  - 修正②: dead zone幅 / max_step_degの再チューニング
  - 修正③: publishState()のACK待ちブロッキング解消（非同期化）
- **目的**: Point1「ターゲットを正確に追尾して逃さない」への対応が、実機・CI上でどう機能したかを時系列で記録する（開発ログ）
- **対象コード**: `SeamlessTrack/src/px2/include/inference.h`, `SeamlessTrack/src/px2/src/inference.cpp`, `SeamlessTrack/src/px2/include/control/axis_pd_controller.hpp`
- 修正箇所の詳細なBefore/Afterは `px_point1_code_fixes_before_after.md` を参照

---

## 2. 経緯（時系列）

1. **バグ調査**: `computeAxisDeg()`がdead zone内でも毎フレーム`pos_deg`を更新してしまう不整合を特定（①）。あわせてdead zone/max_step比率の見直し（②）、`publishState()`のACKブロッキング解消（③）を設計
2. **①②③をコードへ適用 → colcon buildでエラー2件**
   - `kp_yaw`未定義（パッチ適用時のコメント巻き込みミス）
   - `last_ack_time_`未定義（②のパッチ提供時にヘッダー側の宣言を出し漏れ）
3. **ビルドエラー修正後、sim-hil CIでFAIL（1回目）**
   - `control_params.yaml`の`dead_zone_deg`が1.5のまま(`inference.h`は2.0に変更済み) → yaml側を2.0に更新して解消
4. **実機検証（kp_yaw=0.4 / kd_yaw=0.5）**
   - ①のみ適用時: target静止時もservoが動き続ける(hunting継続)
   - ①②③すべて適用時: target静止でservoも静止 → 3点セットでの有効性を確認
   - ただし「追跡速度が遅い」という弱点(weakpoint)を確認
5. **速度低下の原因調査**: `launch_px.sh`が`max_step_deg:=3.0`を明示的にオーバーライドしており、②で変更したC++側デフォルト(1.2)は実運用パスでは反映されていなかったことが判明
6. **dead code cleanup（post process）**: ACK待ち機構一式（`wait_for_ack_ms`, `ack_mtx_`, `ack_cv_`, `ack_ready_`, `ack_group_`, `px3_ack_sub_`, `last_ack_time_`, `kAckTimeoutMs`）を削除。連鎖して`test_sim_hil_param_contract.py`の`INFERENCE_H_MAPPING`と`control_params.yaml`の`ack_timeout_ms`も同期
7. **cleanup適用漏れにより sim-hil CIが再度FAIL（2回目）**: `kAckTimeoutMs`が見つからないエラー。`test_sim_hil_param_contract.py`側の修正が未適用だったことが原因と判明、修正
8. **`max_step_deg`を1.2 / 3.0 / 5.0 / 8.0で振って実機検証**（本レポートの主結果、3節）

---

## 3. 検証結果

### 3.1 基本動作（①②③適用、kp_yaw=0.4 / kd_yaw=0.5）
targetが静止した際にservoも静止することを確認した。ただし①単独では未解消であり、①②③をすべて適用して初めて安定した点は記録として残す。

### 3.2 max_step_degの挙動特性

| max_step_deg | 静止後の収束 | 収束時間 | 追跡速度 |
|---|---|---|---|
| 1.2 | 即静止（hunting無し） | ― | 遅い |
| 3.0 | 即静止（hunting無し） | ― | 遅い（1.2と同等） |
| 5.0 | 静止直後は往復振動、その後収束 | 約5秒 | medium |
| 8.0 | 20秒経過しても収束せず | 収束せず | 高速 |

### 3.3 現時点のスタンス
max_step_degを上げるほど追跡速度は向上するが、5.0以上で静止後の収束性が悪化する傾向が見えた。3.0〜5.0の間、および5.0〜8.0の間のどこかに安定限界が存在すると推測されるが、**今回は事実の記録に留め、推奨値の結論は保留**する（要追加検証）。

---

## 4. CI / 契約テストの状態
- `test_sim_hil_param_contract.py`の3テスト（`test_effective_control_params_match_yaml`, `test_inference_h_constants_match_yaml`, `test_yaw_pitch_range_is_physically_valid`）は最終的に全てpass
- `dead_zone_deg`、`ack_timeout_ms`関連の同期ずれで2回CIが落ちた。以後、定数変更時は以下3点セットを同時に見直す運用にする:
  1. `inference.h`（定数本体）
  2. `simulation/data/params/control_params.yaml`（`future_params`配下）
  3. `ci_checks/tests/test_sim_hil_param_contract.py`の`INFERENCE_H_MAPPING`

---

## 5. Open items
- max_step_degの安定限界の特定（3.0〜8.0の間を追加検証）
- Point1④: 高頻度Trackerレイヤーの追加は未着手
- Point3: Jetson Nano/Orin換装は未着手
- `px3_servo_node.cpp`が`/px3_ack`へのpublishを継続中（購読者がなく実害はないが整理の余地あり）

---

## 6. 付録: 修正箇所まとめ

| # | 対象ファイル | 変更内容 | 詳細 |
|---|---|---|---|
| ① | `inference.h` / `inference.cpp` / `axis_pd_controller.hpp` | accumulator/dead-zone不整合修正（`errorDeg()`/`commit()`分離） | → `px_point1_code_fixes_before_after.md` 修正① |
| ② | `inference.h` / `inference.cpp` | dead zone 1.5→2.0deg、max_step_deg 3.0→1.2（デフォルト値。実運用は`launch_px.sh`依存、3.2節参照） | → 同ドキュメント 修正② |
| ③ | `inference.cpp` | `publishState()`のACK待ち非同期化、QoS `KeepLast(1)`化 | → 同ドキュメント 修正③ |
| ④ | `inference.h` / `inference.cpp` | ACK待ち機構一式(dead code)の削除 | → 同ドキュメント 修正④ |
| — | `test_sim_hil_param_contract.py` / `control_params.yaml` | CI契約テストのマッピング・yaml同期（dead_zone_deg, ack_timeout_ms） | → 同ドキュメント 修正④ 内 |
| — | `run_level3_tracking_tests.py` | docstring内の古い固定値記述を削除 | → 同ドキュメント 付随修正 |
