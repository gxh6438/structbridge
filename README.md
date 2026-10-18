# StructBridge

Endstone 插件，在 Bedrock 专用服务器上导出、导入 `.mcstructure` 结构文件。

导出走引擎自己的结构序列化器，和结构块保存用的是同一条代码路径，生成的文件可以直接放进行为包、用结构块加载，或者用 Amulet 等工具打开。导入同理，NBT 解析和模板加载都由引擎完成。

## 功能

- `/structexport` — 把世界中的一块区域导出为 `.mcstructure` 文件
- `/structimport` — 按名称或路径导入结构文件，重启服务器后依然可用
- `/structdump` — 导出游戏内已保存的结构
- `/structlist` / `/structdelete` — 查看和删除结构库
- 插件 API，供其他 Python / C++ 插件调用

## 环境要求

- Endstone 0.11.x（BDS 1.21+），在 BDS 1.26.52 上测试
- Linux：glibc 2.17 及以上
- Windows：可以构建，但没有进程沙箱，兼容性探测默认关闭，需要在配置中手动开启

## 安装

把对应平台的二进制放进 `bedrock_server/plugins/` 后启动服务器：

- Linux：`endstone_structbridge.so`
- Windows：`endstone_structbridge.dll`

首次启动会生成 `plugins/structbridge/` 目录（配置文件和结构库），然后运行一次兼容性探测。探测失败时插件停用自身功能，服务器正常运行。

详细用法见 [USAGE.md](USAGE.md)。

## 从源码构建

```bash
scripts/build_linux.sh     # 需要 clang-19，或 clang-18 + libc++ 19
scripts/build_windows.sh   # 用 clang-cl 交叉编译，依赖见 DEVELOPMENT.md
```

工具链细节和内部实现说明见 [DEVELOPMENT.md](DEVELOPMENT.md)。
