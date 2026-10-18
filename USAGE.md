# StructBridge 使用说明

## 安装

把 `endstone_structbridge.so`（Linux）或 `endstone_structbridge.dll`（Windows）放进服务器的 `plugins/` 目录，重启服务器。

首次启动时插件会：

1. 在 `plugins/structbridge/` 下生成配置文件和结构库目录；
2. 把结构库同步到 `behavior_packs/vanilla/structures/`；
3. 在子进程沙箱里跑一遍完整的导出导入链路，验证当前服务端兼容。

探测失败时所有命令拒绝执行，服务器本身不受影响。

## 命令

所有命令仅 OP 可用。

结构名称支持中文等 UTF-8 字符（引擎本身没有限制，例如 `/structexport 中文屋 ...`），不允许空格、引号、路径分隔符和控制字符；命名空间冒号最多一个。

### /structexport

```
/structexport <名称> <x1> <y1> <z1> <x2> <y2> <z2> [entities] [blocks]
```

把两个对角坐标之间的区域导出为 `.mcstructure` 文件，落在 `plugins/structbridge/exports/mystructure/<名称>.mcstructure`。

- 坐标顺序随意，插件自动归一化；玩家执行时取玩家所在维度，控制台执行时为主世界
- `entities`、`blocks` 默认都为 `true`
- 每条边最多 512 格，和结构块一致

导出需要区域内的区块处于加载状态。插件会自动为区域添加临时 ticking area，导出完成后立即移除，过程中不需要有任何玩家在线。

### /structimport

```
/structimport <名称或文件路径>
```

按名称导入时，插件在结构库里查找 `exports/mystructure/<名称>.mcstructure`；也可以直接给一个文件路径。导入后立即可用：

```
/structure load <名称> <x> <y> <z>
```

在游戏里的结构块界面输入同名也可以直接使用。

### /structdump

```
/structdump <名称>
```

导出游戏内已经保存的结构。比如放一个结构块、选好位置、命名 `123` 并保存，之后随时可以 `/structdump 123` 把它存成文件。

### /structdelete

```
/structdelete [名称]
```

带名称时删除该结构的全部副本：引擎内存、世界数据库、结构库文件、行为包镜像，重启后也不会再出现，即使结构没加载也会从数据库清除。不带参数时批量删除插件管理的全部结构（结构库及其镜像）；玩家用结构块随手保存、从未经过本插件的世界存档结构不在管理范围内，不会被误删，需要单独 `/structdelete <名称>` 清理。

只能删除 `mystructure:` 命名空间下的结构，行为包自带的结构不受影响。

### /structlist

列出服务器上能看到的所有结构，按三个来源合并去重：

- **插件库** — `plugins/structbridge/exports/` 里本插件导出、导入过的结构
- **行为包** — 各行为包自带的 `.mcstructure`（原版的要塞、村庄等）
- **世界存档** — 玩家用结构块或 `/structure save ... disk` 保存的结构，直接读世界数据库得出，包括从未经过本插件的和中文名的

每条记录标注来源（库/包/世界，可同时多处）、文件大小和是否已加载进引擎内存。行为包自带的结构属于游戏数据，不参与删除。

### /structprobe、/structstatus、/structapi

- `/structprobe` 重新运行兼容性探测
- `/structstatus` 查看探测结果和诊断信息
- `/structapi` 自检插件 API，模拟外部插件完整调用一遍，排查 API 接入问题时先用它（仅 Linux）

## 数据存放

导入一个结构时，数据写入四处：

| 位置 | 作用 |
|---|---|
| 引擎内存 | 当前会话立即可用 |
| 世界数据库 | 重启后保留 |
| `plugins/structbridge/exports/` | 结构库，文件可以直接带走 |
| `behavior_packs/vanilla/structures/` | 镜像，引擎启动时扫描这个目录 |

镜像的意义：即使卸载本插件，已导入的结构对 `/structure load` 和结构块仍然可用。插件每次启动会把结构库同步到镜像目录。

往结构库目录手动添加文件时注意：引擎只在启动时扫描行为包目录，运行中添加的文件要用 `/structimport` 导入，或者重启服务器。

内存占用方面，每个结构只有几 KB 到几十 KB，删除结构或重启服务器后释放；世界数据库中的结构条目在删除时一并清除，LevelDB 自身有压实机制，正常使用不会持续膨胀。

## 配置

`plugins/structbridge/config.json`：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `output_dir` | `exports` | 结构库目录 |
| `include_entities` / `include_blocks` | `true` | 导出时的默认开关 |
| `probe_enabled` | `true` | 是否运行启动探测 |
| `probe_timeout_seconds` | `15` | 探测超时 |
| `probe_template_name` | `structbridge:__probe__` | 探测用的临时结构名 |
| `require_op` | `true` | 命令仅 OP 可用 |
| `windows_allow_without_probe` | `false` | Windows 专用，见下 |
| `windows_expected_minecraft_version` | 空 | 版本前缀校验 |
| `diagnostic_logging` | `true` | 诊断日志 |
| `export_max_attempts` | `20` | 导出重试次数上限 |
| `export_retry_interval_ticks` | `20` | 重试间隔 |
| `export_area_chunk_limit` | `100` | 单个 ticking area 的区块上限 |
| `export_max_areas` | `10` | 同时存在的 ticking area 上限 |

Windows 没有 fork，无法做进程级沙箱，探测默认关闭。如果确认服务端版本兼容，可以把 `windows_allow_without_probe` 设为 `true` 开启功能，建议同时配置 `windows_expected_minecraft_version` 做版本校验。

## 阶段码

`/structstatus` 和 API 返回的阶段码：

| 码 | 含义 |
|---|---|
| 0 | 成功 |
| 10 | 拿不到 Level |
| 11 | 拿不到 StructureManager |
| 12 | 结构模板不存在 |
| 13 | 模板序列化失败 |
| 14 | NBT 写出失败 |
| 15 | NBT 解析失败 |
| 16 | 文件写入失败 |
| 18 | 回读校验失败 |
| 20 | 导入文件解析失败 |
| 21 | 引擎模板加载失败 |
| 22 | 导入往返比对不一致 |
| 30 | API 调用参数错误 |
| 31 | 区域过大 |
| 32 | 维度不支持 |
| 33 | 兼容性探测未通过 |
| 34 | 结构不存在 |
| 35 | 导出重试超时 |

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
    structbridge.export_region("house", 100, 60, 100, 110, 70, 110,
                               on_done=lambda stage, msg: print(stage, msg))
```

`is_available()` 在启动探测完成前为 `False`，调用方需要轮询等待。`export_region` 是异步的，回调在服务器线程触发；其余方法同步返回 `{"ok", "stage", "message"}` 字典。`list_structures` 的 `sources` 是来源列表，取值 `"library"`（插件库）、`"pack"`（行为包）、`"world"`（世界存档）。

### C++

C++ 插件通过 dlsym 拿到工厂函数：

```cpp
#include "structbridge_api.h"
#include <dlfcn.h>

// Endstone 从 plugins/.local/endstone_structbridge-*.so 加载插件，
// 要 dlopen 那个副本；直接 dlopen plugins/ 下的原文件会得到另一个实例
void *h = dlopen("plugins/.local/endstone_structbridge-XXXX.so", RTLD_NOW);
auto get = (structbridge_get_api_fn)dlsym(h, "structbridge_get_api");
auto *api = (structbridge::IApi *)get();

if (api->isAvailable()) {
    char msg[256];
    api->importStructure("myhouse", msg, sizeof(msg));   // 返回阶段码，0 为成功
    api->exportRegion("house", 100, 60, 100, 110, 70, 110, nullptr, true, true,
                      [](void *ctx, int stage, const char *msg) {}, nullptr);
}
```

接口只使用 `const char*`、整数和函数指针，没有 STL 类型，libstdc++ 编译的插件也可以直接调用。完整定义见 `include/structbridge_api.h`，文件头部有完整示例。

## 常见问题

**导出的文件能在其他地方用吗？**
能，和结构块导出的是同一种文件。

**导出为什么要等一两秒？**
区块需要先加载。没玩家在线时服务器没有任何已加载区块，插件用 ticking area 把区域拉起来后再执行导出。

**探测失败怎么办？**
说明当前 BDS 版本和插件的编译基准不一致，等更新即可，服务器不受影响。用 `/structstatus` 可以看到失败在哪个阶段。
