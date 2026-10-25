# StructBridge

Endstone 插件，在 Bedrock 专用服务器上导出、导入、放置 `.mcstructure` 结构文件，支持大区域自动分片（结构束）与跨存档原位迁移。

导出与导入均由引擎自身的结构序列化器 / NBT 解析器完成，与结构块保存走同一条代码路径：生成的文件可直接放进行为包、用结构块加载，或用 Amulet 等工具打开。

## 功能

统一命令 `/struct`：

- `/struct export` — 将世界中一块区域导出为 `.mcstructure`；超过 64×64 的区域自动分片为结构束 `.bundle`（目录 + manifest.json）
- `/struct import` — 按名称或路径导入结构文件或结构束，重启服务器后依然可用
- `/struct place` — 将结构或结构束放置到指定坐标；`original` 参数按导出时的原坐标与维度复原（跨存档建筑迁移）
- `/struct dump` — 将游戏内已保存的结构导出为文件
- `/struct list` — 列出服务器上的全部结构（插件库、行为包、世界存档三来源合并），支持按来源过滤
- `/struct delete` / `/struct clear` — 删除结构（引擎内存 + 世界数据库 + 插件文件）；`clear` 批量删除需二次确认
- `/struct status` / `/struct api` — 运行状态诊断 / API 自检（Linux 与 Windows 均可用）
- 插件 API（Python / C++），供其他插件调用：自定义输出目录、指定传送玩家、结构束导出与放置

结构名称支持中文等 UTF-8 字符。

尺寸限制（引擎上限）：单个结构模板最大 **64×384×64**（X/Z/Y）。更大的水平区域由插件自动切成 64×64 分片，作为一个结构束统一导出、一键导入。

传送规则：任何情况下插件**只传送发起操作的玩家本人**（或 API 调用方通过 `tp_player` 参数显式指定的玩家），绝不随机选择在线玩家。下界/末地维度必须有可传送的玩家才能导出/放置（引擎限制：非主世界维度需要实体 origin）。

## 环境要求

- Endstone 0.11.x（BDS 1.21+）
- Linux：glibc 2.17 及以上
- Windows：Windows 10 / Server 2016 及以上

## 安装

将对应平台的二进制放入 `bedrock_server/plugins/` 后启动服务器：

- Linux：`endstone_structbridge.so`
- Windows：`endstone_structbridge.dll`

首次启动会生成 `plugins/structbridge/` 目录（配置文件 `config.json` 与结构库 `library/`）。

详细用法见 [USAGE.md](USAGE.md)。

## 从源码构建

```bash
scripts/build_linux.sh     # clang-19 + libc++ 19，产物 dist/endstone_structbridge.so
scripts/build_windows.sh   # clang-cl 交叉编译，产物 dist/endstone_structbridge.dll
```

构建依赖与工具链说明见 [DEVELOPMENT.md](DEVELOPMENT.md)。

## 许可

[MIT](LICENSE)
