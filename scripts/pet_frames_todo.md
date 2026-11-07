# 白衣剑仙 xianjian — 剩余帧出图清单（让豆包一次性出完）

## 一、角色（所有帧统一，不跑形）
- 白发高髻（银白长发、束发髻、散落发丝）
- 白色汉服长袍（交领、宽袖、衣摆及踝）
- 深色腰带 + 棕色斜挎带（左肩斜跨右腰）
- 腰间酒葫芦（棕黄葫芦、黑字「酒」、红流苏）
- 右手持剑（剑尖向下）+ 背剑（左侧斜背）
- 深色长裤 + 深色靴子
- 表情：清冷、平静、淡雅

## 二、输出要求（每帧统一，尽量对齐最终规格）
- **纯白背景 #FFFFFF**，正方形画布（如 2048×2048）
- **全身入镜，水平居中**（中心偏差 ≤3px）
- **脚底对齐底部**（距底 8~10px），角色占高约 90%
- **同一姿态所有帧构图锁定**：大小/位置/角度不变，只变动作
- **相邻帧动作差异要拉开**（不要只差一两个像素，否则动画看不清）
- **循环动画首尾闭合**（最后一帧≈第一帧）
- 命名：`xianjian_<姿态>_<序号>.png`

## 三、已出（跳过，不用再出）
`xianjian_stand.png` + `xianjian_play_0.png` ~ `xianjian_play_7.png`

## 四、待出（共 53 帧）


### 待机 idle（8 帧，循环）

| 序号 | 命名 | 动作 |
|---|---|---|
| 0 | `xianjian_idle_0.png` | 平静站立 |
| 1 | `xianjian_idle_1.png` | 衣袂/发丝开始轻扬 |
| 2 | `xianjian_idle_2.png` | 衣袂扬起（微风拂过） |
| 3 | `xianjian_idle_3.png` | 发丝衣袂轻扬（飘逸） |
| 4 | `xianjian_idle_4.png` | 衣袂回落 |
| 5 | `xianjian_idle_5.png` | 发丝渐静 |
| 6 | `xianjian_idle_6.png` | 恢复平静 |
| 7 | `xianjian_idle_7.png` | 回到站立（≈帧0，循环） |

### 行走 walk（8 帧，循环）

| 序号 | 命名 | 动作 |
|---|---|---|
| 0 | `xianjian_walk_0.png` | 左腿迈出 |
| 1 | `xianjian_walk_1.png` | 左腿落地、衣袂前摆 |
| 2 | `xianjian_walk_2.png` | 身体重心前移 |
| 3 | `xianjian_walk_3.png` | 右腿迈出 |
| 4 | `xianjian_walk_4.png` | 右腿落地、衣袂前摆 |
| 5 | `xianjian_walk_5.png` | 身体重心前移 |
| 6 | `xianjian_walk_6.png` | 左腿再迈出 |
| 7 | `xianjian_walk_7.png` | 回到帧0（完整一步循环） |

### 等待 wait（8 帧，循环）

| 序号 | 命名 | 动作 |
|---|---|---|
| 0 | `xianjian_wait_0.png` | 平静站立，睁眼 |
| 1 | `xianjian_wait_1.png` | 眼帘微垂 |
| 2 | `xianjian_wait_2.png` | 闭目（养神） |
| 3 | `xianjian_wait_3.png` | 闭目，衣袂微扬 |
| 4 | `xianjian_wait_4.png` | 闭目，发丝轻扬 |
| 5 | `xianjian_wait_5.png` | 眼帘微动 |
| 6 | `xianjian_wait_6.png` | 缓缓睁眼 |
| 7 | `xianjian_wait_7.png` | 回到站立（≈帧0，循环） |

### 睡眠 sleep（8 帧，循环）

| 序号 | 命名 | 动作 |
|---|---|---|
| 0 | `xianjian_sleep_0.png` | 倚剑，平静 |
| 1 | `xianjian_sleep_1.png` | 眼帘渐垂 |
| 2 | `xianjian_sleep_2.png` | 闭目入睡 |
| 3 | `xianjian_sleep_3.png` | 呼吸轻起（身体微起） |
| 4 | `xianjian_sleep_4.png` | 呼吸轻伏 |
| 5 | `xianjian_sleep_5.png` | 呼吸轻起 |
| 6 | `xianjian_sleep_6.png` | 呼吸轻伏 |
| 7 | `xianjian_sleep_7.png` | 回到帧0（循环） |

### 摔倒 fall（3 帧，单次（停最后帧））

| 序号 | 命名 | 动作 |
|---|---|---|
| 0 | `xianjian_fall_0.png` | 身体微倾，开始失衡 |
| 1 | `xianjian_fall_1.png` | 踉跄后仰/侧倾 |
| 2 | `xianjian_fall_2.png` | 稳住身形（轻功点地稳落） |

### 被拎起 hold（6 帧，循环）

| 序号 | 命名 | 动作 |
|---|---|---|
| 0 | `xianjian_hold_0.png` | 被拎起，衣袂下垂 |
| 1 | `xianjian_hold_1.png` | 身体轻微左晃 |
| 2 | `xianjian_hold_2.png` | 回中 |
| 3 | `xianjian_hold_3.png` | 身体轻微右晃 |
| 4 | `xianjian_hold_4.png` | 回中，衣袂微扬 |
| 5 | `xianjian_hold_5.png` | 回到帧0（循环） |

### 丢回收站 trash（12 帧，单次（停最后帧））

| 序号 | 命名 | 动作 |
|---|---|---|
| 0 | `xianjian_trash_0.png` | 被抓住 |
| 1 | `xianjian_trash_1.png` | 脱手 |
| 2 | `xianjian_trash_2.png` | 身体后仰 |
| 3 | `xianjian_trash_3.png` | 衣袂展开 |
| 4 | `xianjian_trash_4.png` | 开始下坠，衣袂飘起 |
| 5 | `xianjian_trash_5.png` | 下坠中，发丝飞扬 |
| 6 | `xianjian_trash_6.png` | 下落，衣袂展开 |
| 7 | `xianjian_trash_7.png` | 继续下落 |
| 8 | `xianjian_trash_8.png` | 接近地面 |
| 9 | `xianjian_trash_9.png` | 轻功点地缓冲 |
| 10 | `xianjian_trash_10.png` | 稳住身形 |
| 11 | `xianjian_trash_11.png` | 从容落定（停此帧） |


## 五、出图建议
- 每个姿态先出一张关键帧（如 idle_0 / walk_0），确认构图一致后再补该姿态其余帧。
- 全部出完后打包给我（或告诉我目录），我跑 `process_pet_frames.py` 自动入库。
