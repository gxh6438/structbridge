# StructBridge 使用说明

## 安装

把 `endstone_structbridge.so`（Linux）或 `endstone_structbridge.dll`（Windows）放进服务器的 `plugins/` 目录，重启服务器。

首次启动时插件会：

1. 在 `plugins/structbridge/` 下生成配置文件 `config.json` 和结构库目录 `library/`；
2. 把结构库同步到 `behavior_packs/vanilla/structures/`。

## 命令

所有命令仅 OP 可用（可用配置 `require_op` 关闭）。统一入口为一个 `/struct` 命令。

结构名称支持中文等 UTF-8 字符（例如 `/struct export 中文屋 ...`），不允许空格、引号、路径分隔符和控制字符；命名空间冒号最多一个。

```
/struct export <名称> <x1> <y1> <z1> <x2> <y2> <z2> [entities] [blocks]
/struct import <文件或名称>
/struct place <名称> <x> <y> <z> [entities] [blocks]
/struct place <名称> original [entities] [blocks]
/struct dump <名称>
/struct list [all|library|world|pack]
/struct delete <名称>
/struct clear
/struct status | /struct api
```

### /struct export

把两个对角坐标之间的区域导出为结构文件：

- 坐标顺序随意，插件自动归一化；玩家执行时取玩家所在维度，控制台执行时为主世界
- `entities`、`blocks` 默认都为 `true`
- 单个结构模板的引擎上限是 **64×384×64**（X/Z/Y），高度超过 384 会被拒绝
- **水平方向超过 64×64 的区域自动分片为结构束**：落盘为一个 `<名称>.bundle` 目录，内含若干 64×64 的 `.mcstructure` 分片和一份 `manifest.json`（记录原始区域坐标、维度、分片网格和每个分片的引擎键名）。导入 / 放置 / 删除都把束当作一个整体

导出需要区域内的区块处于加载状态。插件会自动为区域添加临时 ticking area；不够用时（BDS 全局上限 10 个 area）会把**发起操作的玩家本人**传送到区域附近补齐加载，结束后移除 area 并把玩家传送回原位。不需要有其他玩家在线。

### /struct import

按名称导入时，插件在结构库里查找 `library/mystructure/<名称>.mcstructure`；也可以直接给一个文件路径，或一个 `.bundle` 目录路径（相对服务器根目录）。结构束导入后全部分片注册进引擎，立即可用；更常见的是直接用 `/struct place`，插件会自动逐片放置并保持分片接缝对齐。

在游戏里的结构块界面输入同名也可以直接使用（单结构；结构块不理解束）。

### /struct place

把结构或结构束放置到世界里：

- `/struct place <名称> <x> <y> <z>` — 放置到指定坐标。`<名称>` 可以是结构库里的结构名、结构束名，也可以是 `.bundle` 目录路径（覆盖导出到自定义目录的束）
- `/struct place <名称> original` — **按导出时的原坐标和原维度整体复原**，这是跨存档迁移建筑的标准用法：把 `.bundle` 目录拷到目标服务器（放在服务器目录下任意路径），一条命令原位复原
- 放置目标处的区块加载逻辑和导出相同（ticking area + 必要时传送发起玩家本人）
- 结构束的分片在放置前**按需导入**：引擎内存里没有的分片会先从束目录读入再放置，因此重启服务器、行为包无镜像的场景也能直接放置
- 单结构文件（`.mcstructure`）本身不携带世界坐标，`original` 只对结构束有效

### /struct dump

导出游戏内已经保存的结构。比如放一个结构块、选好位置、命名 `123` 并保存，之后随时可以 `/struct dump 123` 把它存成文件。

### /struct list

列出结构，默认只显示插件管理的（结构库 + 结构束），带参数过滤：

- `all` — 三来源合并去重：插件库、行为包（原版的要塞、村庄等）、世界存档（玩家用结构块或 `/structure save ... disk` 保存的，直接读世界数据库，包括从未经过本插件的结构和中文名称的）
- `library` / `world` / `pack` — 只看单一来源

每条记录标注来源、文件大小和是否已加载进引擎内存；结构束显示为一条记录（片数 + 总大小）。行为包自带的结构属于游戏数据，不参与删除。

### /struct delete / clear

`/struct delete <名称>` 删除该结构的全部副本：引擎内存、世界数据库、结构库文件、行为包镜像，重启后也不会再出现。名称对应结构束时删除整个束。

`/struct clear` 批量删除插件管理的全部结构（结构库、结构束及各自镜像），**需要再输一次 `/struct clear confirm` 确认**。玩家用结构块随手保存、从未经过本插件的世界存档结构不在管理范围内，不会被误删。

只能删除 `mystructure:` 命名空间下的结构，行为包自带的结构不受影响。

### /struct status、api

- `/struct status` — 查看版本、目录布局和进行中的任务数
- `/struct api` — 自检插件 API：模拟外部插件的完整调用路径（加载本插件库 → 解析导出符号 → 版本校验 → 接口调用），排查 API 接入问题时先用它（Linux 与 Windows 均可用）

## 结构束（.bundle）

大于 64×64 的水平区域导出时的自动分片格式：

```
<名称>.bundle/
  manifest.json          # 原区域坐标、维度、entities/blocks、分片网格
  tile_r0c0.mcstructure  # 64x64 分片，r/c 是网格行列
  tile_r0c1.mcstructure
  ...
```

- `manifest.json` 的 `partial` 标记表示导出中断（某片失败），`tiles[].done` 可以精确定位缺哪片；partial 的束放置时只放置已完成的分片并给出警告
- 束在 `/struct list` 里显示为一条记录；`/struct delete` 删除整个目录
- 用 API 导出到自定义目录的束不写行为包镜像（不污染 vanilla 包），放置时用 `.bundle` 路径引用，也可以直接把目录拷到别的服务器做跨服迁移

## 数据存放

导入一个结构时，数据写入四处：

| 位置 | 作用 |
|---|---|
| 引擎内存 | 当前会话立即可用 |
| 世界数据库 | 重启后保留 |
| `plugins/structbridge/library/` | 结构库，文件可以直接带走 |
| `behavior_packs/vanilla/structures/` | 镜像，引擎启动时扫描这个目录 |

镜像的意义：即使卸载本插件，已导入的结构对 `/structure load` 和结构块仍然可用。插件每次启动会把结构库同步到镜像目录。**结构束不写镜像**（分片按需导入即可），API 导出到自定义目录的单结构同样不写。

往结构库目录手动添加文件时注意：引擎只在启动时扫描行为包目录，运行中添加的文件要用 `/struct import` 导入，或者重启服务器。

内存占用方面，每个结构只有几 KB 到几十 KB，删除结构或重启服务器后释放；世界数据库中的结构条目在删除时一并清除，LevelDB 自身有压实机制，正常使用不会持续膨胀。

## 配置

`plugins/structbridge/config.json`：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `library_dir` | `library` | 结构库目录 |
| `include_entities` / `include_blocks` | `true` | 导出时的默认开关 |
| `require_op` | `true` | 命令仅 OP 可用 |
| `export_max_attempts` | `20` | 导出重试次数上限 |
| `export_retry_interval_ticks` | `20` | 重试间隔（20 ticks = 1 秒） |
| `export_area_chunk_limit` | `100` | 单个 ticking area 的区块上限 |
| `export_max_areas` | `10` | 同时存在的 ticking area 上限 |

## 阶段码

命令反馈和 API 回调使用的阶段码：

| 码 | 名称 | 含义 |
|---|---|---|
| 0 | ok | 成功 |
| 10 | level-handle | 拿不到 Level |
| 11 | structure-manager | 拿不到 StructureManager |
| 12 | get-structure | 结构模板不存在 |
| 13 | template-save | 模板序列化失败 |
| 14 | nbt-write | NBT 写出失败 |
| 15 | nbt-parse | NBT 解析失败 |
| 16 | file-write | 文件写入失败 |
| 20 | import-nbt-parse | 导入文件解析失败 |
| 21 | template-load | 引擎模板加载失败 |
| 30 | invalid-name-or-argument | API 调用参数错误 |
| 31 | region-too-large | 区域过大（Y 超过 384 等） |
| 32 | unsupported-dimension | 维度不支持 |
| 34 | not-found | 结构不存在 |
| 35 | export-retry-budget-exhausted | 导出重试超时 |
| 40 | place-chunk-load | 放置目标区块无法加载 |
| 41 | place-structure-load | 原版 structure load 未生效 |
| 42 | bundle-manifest | 结构束 manifest 缺失或损坏 |
| 43 | bundle-tile-export | 结构束某分片导出失败（束标记 partial） |
| 44 | tp-player-unavailable | 指定的 tp_player 不在线 |

## 插件 API

### Python

插件加载时会把 `structbridge` 模块注册进 `sys.modules`，其他 Python 插件直接导入即可：

```python
import structbridge

if structbridge.is_available():
    structbridge.import_structure("myhouse")
    structbridge.dump_structure("123")
    structbridge.list_structures()          # [{"key": ..., "size": ..., "loaded": ..., "sources": [...]}, ...]
    structbridge.delete_structure("myhouse")

    # 导出到结构库（默认主世界）
    structbridge.export_region("house", 100, 60, 100, 110, 70, 110,
                               on_done=lambda stage, msg: print(stage, msg))

    # 导出到自定义目录 + 强制结构束 + 指定可传送的玩家
    structbridge.export_region(
        "castle", 0, 64, 0, 200, 120, 200,          # 名称 + 两角坐标
        dimension="overworld",                       # overworld / nether / the_end
        include_entities=True, include_blocks=True,
        out_dir="backups/october",                   # 相对服务器根目录；空 = 结构库
        tp_player="Steve",                           # 只传送这名玩家
        force_bundle=True,                           # 小区域也切成 .bundle
        on_done=lambda stage, msg: print(stage, msg))

    # 放置（单结构或结构束；名称 / 库内束名 / .bundle 路径均可）
    structbridge.place_structure(
        "backups/october/castle.bundle", 300, 64, 300,
        dimension="overworld", include_entities=True, include_blocks=True,
        tp_player="Steve",                           # 非主世界时必填
        on_done=lambda stage, msg: print(stage, msg))

    # 结构束按导出时的原坐标与维度复原（跨服迁移）
    structbridge.place_structure(
        "backups/october/castle.bundle", 0, 0, 0,
        original=True, on_done=lambda stage, msg: print(stage, msg))
```

`export_region`、`place_structure` 是异步的，回调在服务器线程触发且恰好触发一次；其余方法同步返回 `{"ok", "stage", "message"}` 字典。`list_structures` 的 `sources` 是来源列表，取值 `"library"`（插件库）、`"pack"`（行为包）、`"world"`（世界存档）。

`tp_player` 语义：区块加载需要补齐或维度为下界/末地时，插件**只传送这一名玩家**（不传则不传送任何人；下界/末地必须提供，否则直接拒绝，阶段码 44 表示指定的玩家不在线）。绝不随机挑选在线玩家。

### C++

C++ 插件通过动态加载器拿到工厂函数：

```cpp
#include "structbridge_api.h"
#include <dlfcn.h>   // Windows: LoadLibraryW / GetProcAddress

// Endstone 从 plugins/.local/endstone_structbridge-*.so 加载插件，
// 要打开那个副本；直接加载 plugins/ 下的原文件会得到另一个实例
void *h = dlopen("plugins/.local/endstone_structbridge-XXXX.so", RTLD_NOW);
auto get = (structbridge_get_api_fn)dlsym(h, "structbridge_get_api");
if (!get) { /* 符号缺失 */ }
auto *api = (structbridge::IApi *)get();

if (api->isAvailable()) {
    char msg[256];
    api->importStructure("myhouse", msg, sizeof(msg));   // 返回阶段码，0 为成功

    // 导出：自定义目录 + 束 + 指定传送玩家（只传送 Steve）
    api->exportRegion("castle", 0, 64, 0, 1023, 120, 1023, "overworld", true, true,
                      "backups/october", "Steve", /*force_bundle=*/true,
                      [](void *ctx, int stage, const char *msg) {}, nullptr);

    // 放置：束路径 + 原位复原（manifest 里的坐标与维度）
    api->placeStructure("backups/october/castle.bundle", 0, 64, 0, /*original=*/true,
                        nullptr, true, true, "Steve",
                        [](void *ctx, int stage, const char *msg) {}, nullptr);
}
```

接口只使用 `const char*`、整数和函数指针，没有 STL 类型，libstdc++ 编译的插件也可以直接调用。完整定义见 `include/structbridge_api.h`。

## 常见问题

**导出的文件能在其他地方用吗？**
能，和结构块导出的是同一种文件。结构束里的每个分片也是标准 `.mcstructure`，可以单独拿出来用。

**导出为什么要等一两秒？**
区块需要先加载。没玩家在线时服务器没有任何已加载区块，插件用 ticking area 把区域拉起来后再执行导出。

**我想搬一个 200×200 的建筑到另一个存档，怎么做？**
源服务器 `/struct export castle x1 y1 z1 x2 y2 z2`（自动切成 3×3=9 片的束）→ 把 `plugins/structbridge/library/castle.bundle` 整个目录拷到目标服务器任意路径 → 目标服务器 `/struct place castle.bundle original`（或用 API `place_structure(..., original=True)`），建筑按原坐标原维度复原。

**放置会不会随机传送我的玩家？**
不会。插件只可能传送发起命令的玩家本人，或 API 调用方通过 `tp_player` 指定的那名玩家。
