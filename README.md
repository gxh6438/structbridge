# StructBridge

Endstone 插件，在 Bedrock 专用服务器上导出、导入、放置 `.mcstructure` 结构文件，支持大区域自动分片（结构束）和跨存档原位迁移。

导出走引擎自己的结构序列化器，和结构块保存用的是同一条代码路径，生成的文件可以直接放进行为包、用结构块加载，或者用 Amulet 等工具打开。导入同理，NBT 解析和模板加载都由引擎完成。

## 功能

统一命令 `/struct`（旧的 `/structxxx` 命令仍可用但会提示弃用）：

- `/struct export` — 把世界中的一块区域导出为 `.mcstructure`；超过 64×64 的区域自动分片为结构束 `.bundle`（目录 + manifest.json）
- `/struct import` — 按名称或路径导入结构文件或结构束，重启服务器后依然可用
- `/struct place` — 把结构或结构束放置到指定坐标；`original` 参数按导出时的原坐标与维度复原（跨存档建筑迁移）
- `/struct dump` — 导出游戏内已保存的结构
- `/struct list` — 列出服务器上的全部结构（插件库、行为包、世界存档三来源合并），支持按来源过滤
- `/struct delete` / `/struct clear` — 删除结构（引擎内存 + 世界数据库 + 插件文件）；`clear` 批量删除需二次确认
- `/struct status` / `/struct probe` / `/struct api` — 诊断、重跑兼容性探测、API 自检
- 插件 API v2（Python / C++），供其他插件调用：自定义输出目录、指定传送玩家、结构束导出与放置

结构名称支持中文等 UTF-8 字符。

尺寸限制（引擎本身的上限）：单个结构模板最大 **64×384×64**（X/Z/Y）。更大的水平区域由插件自动切成 64×64 的分片，作为一个结构束统一导出、一键导入。

传送规则：任何情况下插件**只传送发起操作的玩家本人**（或 API 调用方通过 `tp_player` 参数显式指定的玩家），绝不随机选择在线玩家。下界/末地维度必须有可传送的玩家才能导出/放置（引擎限制，非主世界维度需要实体 origin）。

## 环境要求

- Endstone 0.11.x（BDS 1.21+），在 BDS 1.26.52 上测试
- Linux：glibc 2.17 及以上
- Windows：可以构建，但没有进程沙箱，兼容性探测默认关闭，需要在配置中手动开启

## 安装

把对应平台的二进制放进 `bedrock_server/plugins/` 后启动服务器：

- Linux：`endstone_structbridge.so`
- Windows：`endstone_structbridge.dll`

首次启动会生成 `plugins/structbridge/` 目录（配置文件和结构库），然后运行一次兼容性探测。探测失败时插件停用自身功能，服务器正常运行。

从 0.1.0 之前的版本升级时，旧的 `exports/` 结构库会自动迁移到新的 `library/` 目录，`config.json` 里的 `output_dir` 也会自动改写为 `library_dir`。

详细用法见 [USAGE.md](USAGE.md)。

## 从源码构建

```bash
scripts/build_linux.sh     # 需要 clang-19，或 clang-18 + libc++ 19（见 DEVELOPMENT.md）
scripts/build_windows.sh   # 用 clang-cl 交叉编译，依赖见 DEVELOPMENT.md
```

工具链细节和内部实现说明见 [DEVELOPMENT.md](DEVELOPMENT.md)。
