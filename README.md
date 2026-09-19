# Sakura_CPP_ChainMine — DRG 连锁挖矿（C++ / UE4SS）

主机侧 mod：镐击**带资源数据的地形材质**（金/硝石/墨菱石/玉石等 17 种矿）时，自动
**探测出整条连通矿脉的体积**，再按最优挖点逐帧崩空整条脉（Dig 模式），或沿矿脉主轴
发一条雕刻样条一次刻空（Spline 模式，实验）。**只装主机即可，客机无需装**（所有地形 op
都是游戏自己的 NetMulticast，客机自动看到同样的崩落）。

## 功能特点
- **只挖矿**：双重门控——触发门控 `UTerrainMaterial.ResourceData != nullptr`（与游戏
  “挖掉出矿”同一判定）+ 探测门控（每个连锁点都经射线确认是同材质矿格）。普通岩石、
  苔藓/植物/水晶装饰、平台等 200+ 地形材质不触发。
- **Dig 模式（默认）**：整条脉按最优挖点崩空。覆盖判定用满尺寸轴对齐盒子
  （`half = DigRadius/2`，实测镐坑是体素化方块）；细长脉沿 PCA 主轴排“一串方块”，
  弯折/侧枝/块状脉退回贪心覆盖；挖点按离命中点由近到远逐帧崩，看起来像快速连点镐子。
- **Spline 模式（实验）**：沿矿脉主轴一条雕刻样条一次刻空（性能最优），carve 后自动
  清理矿面装饰；宝石/浮岛行为未完全验证。
- **多人共享**：谁挥镐都走主机钩子，全队享受效果；客机可选开启（仅 Dig）。
- **自然连敲不打断**：队列未崩完时再挥镐会合并追加 + 去重，不会留下边角矿。
- **配置热重载**：改 `config.txt` 约 1 秒生效，无需重启游戏。

## 单位说明（重要）
DRG 的地形坐标/半径是 **UE 默认厘米制**（1米=100）。实测镐洞尺寸 `DigSize=115.0`
（115cm ≈ 1.15m）。配置里 `VeinStep=45` 即 0.45m，`MaxDistance=1200` 即 12m。

## 工作原理
1. 在全局 `ProcessEvent` pre-callback 里按 UFunction 指针精确识别
   `APickaxeItem::Server_DigBlock`（不用 UFunction FuncPtr hook——发出的 Server RPC
   在本地不执行函数体，hook 拦不到客机自己的挥镐；全局回调在 RPC 路由前对主机执行和
   客机发送都触发）。钻头等同名函数是不同 UFunction，不会误触发。
2. 材质门控：`TerrainMaterial` 索引 → `ADeepCSGWorld.TerrainMaterials.Materials[]` →
   `UTerrainMaterial.ResourceData != nullptr`（金/硝石/墨菱石/玉石等资源材质）才触发。
3. 探测：沿 ±挥镐方向 15cm 细步找种子（命中点在矿面上），步长 `VeinStep` 洪泛；每格用
   `PointIsVein`：体内点（`IsPointInsideTerrain`）取六向出射射线材质（首个命中轴即定论，
   不看距离），空气点只认距矿面 `0.85×VeinStep` 内的同材质命中；每个接受格再采 6 个
   半步长点，薄脉/斜脉尖端落在网格缝里也覆盖；直到矿脉边界 / `MaxDistance` / `MaxNodes`。
4. Dig 模式：`PlanDigPoints` PCA 主轴管道 + 贪心覆盖生成最优挖点（覆盖判定用满尺寸
   `half=DigRadius/2` 的轴对齐盒子；判定/贪心用空间哈希桶）→ 队列由游戏线程
   ProcessEvent pre-callback 每 `FireIntervalMs` 发一笔嵌套 `Server_DigBlock`，游戏内部
   走完整复制：`TerrainOp_PickAxe` 多播同步客机 + `All_SimulateDigBlock` 粒子/音效；
   队列未崩完时再挥镐合并追加（去重），不打断连锁。
5. Spline 模式：`PlanSpline` 幂迭代拟合主轴 → 下一帧发一条 `TerrainOp_CarveSplineSegment`
   （Material=nullptr、ReplaceAll、TurnIntoGems，与游戏管道挖洞一致）→ 沿样条
   `RemoveDebrisInSphere` 清装饰。
6. 所有 UE 反射/配置热重载在游戏线程；hook 体 SEH 包裹；队列空时 tick 回调零开销。

## 多人行为
- **别人来你房（你是主机/单机）**：连锁逻辑全在主机侧执行（谁挥镐都走 `Server_DigBlock`
  → 主机钩子），所以**全队都享受效果**，客机不用装任何东西；Dig 和 Spline 模式都生效。
- **你去别人房（你是客机）**：默认关闭（`EnableAsClient=false`），就是正常挖矿；想要也连锁
  就把 `EnableAsClient=true`——客机会本地探测并反复发送 `Server_DigBlock` RPC（Dig 模式），
  Spline 模式是 NetMulticast、只能在主机调用，客机自动退回 Dig。
- 注意：UE4SS 注入属于 modded 客户端，是否能进别人房还取决于对方房间的 mod 状态设置。

## 安装
1. 编译（见下）或直接取 `main.dll`。
2. 放到 `FSD\Binaries\Win64\ue4ss\Mods\Sakura_CPP_ChainMine\dlls\main.dll`。
3. 确认 `ue4ss\Mods\mods.txt` 有 `Sakura_CPP_ChainMine : 1`。
4. `config.txt` 放 mod 目录（缺省自动生成；本仓库的 `config.txt` 即最新默认）。
5. 从 Steam 启动游戏，进任意任务后生效。

## 配置（config.txt，改完约 1 秒生效）
| 键 | 默认 | 说明 |
|---|---|---|
| Enabled | true | 总开关 |
| Mode | Dig | `Dig`=逐帧挖空整条矿脉；`Spline`=一条样条一次刻空（实验） |
| VeinStep | 45 | 探测网格步长（厘米，45=0.45m），越小覆盖越全 |
| MaxNodes | 250 | 单次挥镐最多探测的矿脉格数（0=关闭连锁） |
| MaxDistance | 1200 | 距命中点最大连锁距离（厘米，1200=12m） |
| ProbeRayDist | 350 | 探测射线长度（厘米），需大于矿脉半径 |
| DigRadius | 115 | 镐洞尺寸（厘米），游戏默认 115 |
| TubeMargin | 60 | Spline 模式样条余量（厘米） |
| MaxSplineRadius | 220 | Spline 模式样条半径上限（厘米），防止挖出超大洞 |
| AlsoSpecial | true | 重击（Power Attack）是否也触发连锁 |
| EnableAsClient | false | 进别人房（客机）是否也连锁；仅 Dig 模式 |
| RaycastFilter | 3 | 0 Any / 1 Empty / 2 Filled / 3 Diggable / 4 NotDiggable |
| FireIntervalMs | 16 | 连锁挖掘间隔（毫秒）：卡顿调大，太慢调小 |

## 日志
`chainmine.log`（mod 目录，每次启动清空）。预期（Dig 模式）：
```
dig: Server_DigBlock ready (carvePos=0 carveDir=12 mat=24 special=28)
hook: Raycast ready (start=0 dir=12 dist=24 hitInfo=28 filter=56 ret=57 hitMat=24)
hook: IsPointInsideTerrain ready (pos=0 ret=12)
hook: RemoveDebrisInSphere ready (pos=0 radius=12 fragile=16 durable=17 type=18)
hook: spline carve ready (param=0 opNum=0 segs=8 mat=24 filter=32 precious=33)
chain: all hooks ready (raycast=1)
probe: mat=203 cells=250 visited=158 skipped=93 step=45.0 maxDist=1200.0
chain: enqueue ore=TM_Nitra idx=203 cells=250 digs=16 queue=16
chain: done fired=16
```
- 正常表现：`probe:` 的 cells 数量应接近矿脉体积/步长³（含半步长加密点）；`digs` 明显
  小于 cells；整脉在约 1 秒内逐帧崩完。
- 连续挥镐时 `chain: enqueue ... digs=N queue=M merged`：新挖点合并进进行中的队列
  （`digs` 是本次新增数、`queue` 是队列总数），连锁不会被清空打断。
- `probe: cells=0`：命中点探测失败（射线没找到矿脉边界）——看 `ProbeRayDist` 是否太小
  （< 矿脉半径），或 `RaycastFilter` 是否被改成非 Diggable。
- `chain: enqueue ... digs=0`：`DigRadius` 太小或矿脉超出 `MaxDistance`，调回默认。
- Spline 模式看 `spline: enqueue ... radius=... len=...` 与 `spline: fired ...`。
  若地形没变化：把日志发我。

## 编译
```bat
cd /d F:\stuff\DRG\ue4ss-research\ChainMineMod
build.bat
.\deploy.ps1   :: 复制 main.dll；config.txt 缺失才生成（改键可手动 Copy-Item -Force）
```
