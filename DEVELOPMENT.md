# 开发说明

面向维护者的内部实现文档：构建、架构、BDS 实测结论、已知的坑。

## 构建

### Linux

```bash
# clang-18 + libc++ 19 头文件和静态库（clang-18 自带的 libc++ 18.1.3 有
# std::format 的 consteval bug，必须用 19）
CXX=clang++-18 \
LIBCXX_A=/usr/lib/llvm-19/lib/libc++.a \
LIBCXXABI_A=/usr/lib/llvm-19/lib/libc++abi.a \
LIBCXX_INC=/usr/lib/llvm-19/include/c++/v1 \
bash scripts/build_linux.sh
```

产物在 `dist/endstone_structbridge.so`，glibc 2.17 兼容（脚本自带符号版本审计）。

### Windows

```bash
# 依赖：clang-cl、lld-link、libboost-dev / libglm-dev / libgsl-dev
# （vendor/host-includes 是指向系统头文件的符号链接），
# 以及 xwin splat 出的 MSVC CRT + Windows SDK
bash scripts/build_windows.sh
```

xwin 用法：

```bash
curl -fsSL -o xwin.tar.gz \
  https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz
tar xzf xwin.tar.gz
./xwin-0.10.0-x86_64-unknown-linux-musl/xwin --accept-license --arch x86_64 splat --output /opt/msvc --disable-symlinks
```

### 端到端测试

需要一个 endstone 环境（Python 3.11+）：

```bash
pip install endstone==0.11.14
# 把 dist/*.so 放进 bedrock_server/plugins/，在服务器目录下启动：
{ sleep 75; echo "stop"; } | timeout 160 python -m endstone
```

`test/probe_plugin` 是一个 Python 测试插件，等待 API 可用后跑一遍全部接口（导入、导出、异步回调、删除、拒绝路径），装进同一个 venv 即可。

llvm apt 源在部分网络环境下极慢，`apt.llvm.org` 和清华/中科大镜像都可能卡死；南大镜像 `https://mirror.nju.edu.cn/llvm-apt/noble` 实测可用（2026-10）。

## 架构

### 导出链

```
玩家/控制台 origin
  → 为区域添加临时 ticking area（无人在线时服务器没有已加载区块）
  → 派发原版 structure save ... memory，引擎把区域捕获为内存模板
  → Level::getStructureManager()          [vtable]
  → StructureManager::getStructure()      [vtable shim]
  → StructureTemplate::save()             [vtable，引擎序列化]
  → Tag::write(MemoryDataOutput)          [vtable，IDataOutput 实现]
  → 前置根头（0x0A + 空名）后落盘
```

插件不读方块，模板由引擎捕获，文件字节和结构块导出同源。ticking area 自动分片（BDS 限制：单个 area 最多 100 区块、全局最多 10 个），导出完成或超时后移除。

### 导入链

```
文件字节 → MemoryDataInput → 引擎 Tag::load（引擎自己的 NBT 解析器）
  → 引擎 StructureTemplate::load(CompoundTag const&)
  → 注册进 StructureManager
```

裸名自动归入 `mystructure:` 命名空间。

### 已保存结构的导出

`/struct dump` 处理的结构可能只在磁盘上（引擎懒加载）。插件先派发一次 `structure load "<key>" 0 64 0 0_degrees none false true false 0.0`（integrity 0.0，不改动任何方块）把模板拉进内存，再走导出链的序列化部分。

### 放置链（/struct place，v0.1.0）

导出的逆操作，`PlaceJob` + `placeTick`（runTaskTimer）驱动：

```
名称解析（结构 | 库内束 | .bundle 路径 → resolveBundleRef）
  → manifest 展开（束：逐片；单结构：一片）
  → 目标区域 tickingarea 拉区块（预算不够时 TP job->tp_player_name——永远是指定/发起的那名玩家）
  → 分片不在引擎内存 → 从 src_dir 按需导入（引擎 load 链路）
  → 逐片派发 structure load <tile_key> <wx> <wy> <wz>（接缝由网格行列偏移对齐）
  → 完成：移除 area，回调 on_done
```

`original` 走 manifest 里的原始区域/维度，目标坐标由分片 bounds 反推，等价于把整个束平移回导出时的位置。单结构没有世界坐标信息，`original` 直接拒绝。玩家离线（tp 不可用）时束任务允许 partial 完成并警告，单结构直接失败。

### 结构束（bundle，v0.1.0）

引擎单模板上限 64×384×64，大区域由 `planBundle` 切成 64×64 网格（TILE=64，Y 不切，超 384 拒绝）。`bundleTick` 逐片走导出链，全部成功后写 `manifest.json`（原区域、维度、entities/blocks、每片的 file/bounds/key/done）；任何一片失败则标 `partial`，`tiles[].done` 定位缺口。

束的查找顺序：`findBundle`（结构库 `library/`）→ `resolveBundleRef`（带路径分隔符或 `.bundle` 后缀的引用，相对服务器根解析，`BundleManifest::src_dir` 记住来源目录供按需导入）。导出到自定义 out_dir 的束不写行为包镜像（`bundle_dir_override` 非空时跳过镜像，避免污染 vanilla 包）。

### 结构枚举（/struct list）

引擎没有结构枚举 API（StructureManager 只有 getOrCreate/getStructure），列表由插件自己合并来源、按引擎键去重（v0.1.0 起默认只列插件管理的，`all` 才合并全部三来源；束单独显示为一条记录）：

1. 插件结构库（`library/` 目录，首个子目录即命名空间；pre-0.1.0 的 `exports/` 自动迁移）；
2. 各行为包的 `structures/` 目录（`structures/<ns>/<rest>.mcstructure` ⇔ 键 `<ns>:<rest>`）；`__brarchive/` 镜像目录与 `structures/` 平级，扫描天然不进入；行为包文件是只读游戏数据，只有插件自己写入 vanilla 包 `mystructure/` 的镜像可删；
3. 世界 LevelDB（`worlds/<level-name>/db`，level-name 从 server.properties 读取）——`structuretemplate_` 前缀的键。扫描器在 `src/leveldb_scan.{h,cpp}`，独立编译单元、只依赖标准库，格式结论都记录在头文件注释里（块压缩类型 4 是 raw DEFLATE 而非 snappy、WAL 的 delete 条目不带 value、MANIFEST VersionEdit tag 6=deleted/7=new/9=prevlog 等，均在真实数据库上实证）。全部只读且 fail-soft：数据库归运行中的服务器所有，任何不一致只跳过对应文件或块。

删除（`/struct clear` 批量模式与 API 的 delete-all）只处理插件管理的条目（结构库 + 束 + 自有镜像），行为包游戏数据与世界独有存档不动；`clear` 需要二次输入 `confirm`。

### 名称与路径（UTF-8）

引擎对结构名没有字符限制，LevelDB 按原始 UTF-8 存键（中文名实测可用）。插件的校验（`isValidStructureName`）接受一切合法 UTF-8 序列，拒绝控制字符、空格、引号、路径分隔符和第二个冒号；`sanitizeRelativePath` 原样保留多字节序列、只对小写 ASCII 折叠。Windows 上 `fs::path` 的窄字符转换走 ANSI 代码页会毁掉中文文件名，所有路径构造和显示统一经过 `pathFromUtf8`/`pathToUtf8`（内部走 UTF-16 宽 API）。

### 删除

封装原版 `structure delete` 子命令（删引擎内存和 LevelDB 条目），再补删结构库和镜像两个文件。

### 兼容性探测

启动后在 fork 子进程里跑完整往返：空模板 → 引擎 save → NBT 写出 → 引擎解析 → 引擎 load → 再序列化逐字节比对。子进程崩溃只影响探测本身，功能整体禁用，服务器不受影响。Windows 没有 fork，探测默认关闭，靠配置项开启。

### vtable shim

endstone 运行时不导出内部符号，插件必须自包含。StructureTemplate / StructureManager 的虚表前缀在源码里复刻（槽位即声明序，Itanium/MSVC ABI 下编译器自动算对），实际调用的只有 save / getStructure / getOrCreate / load 几个槽位，已对 LeviLamina 的 PDB 头交叉验证并在 BDS 26.52 上实测。HashedString、BaseGameVersion 的构造函数在插件内本地定义。命令经插件描述注册。EndstoneLevel 的原生句柄通过直接访问 level_ 成员取得。

### 公开 API

`include/structbridge_api.h`：纯虚接口 + `extern "C"` 工厂函数，边界上只有 POD 类型。v2（0.1.0）在 vtable 末尾追加 `exportRegionTo`（自定义 out_dir / tp_player / force_bundle）和 `placeStructure`（束感知放置 + original 复原），vtable 前缀不动，v1 消费方二进制兼容。Python 模块在 onLoad 时注入 sys.modules，回调挂在导出/放置任务上，在服务器线程触发；PyObject 的引用计数操作全部放在 GIL 区间内。

Windows 版本目前不编入 Python 模块：交叉编译环境没有 Python 头文件，而且链接特定的 python3xx.dll 会在进程里出现第二个解释器实例。C++ 接口不受影响。

## BDS 26.52 实测结论

- **结构尺寸上限是 64×384×64（X/Y/Z）**，超过即被引擎拒绝（报错原文 "A structure's size cannot be larger than (64, 384, 64)"）。早期按结构块 UI 推测的 480/512 是错的，以实测为准——这就是束分片取 64×64 的原因。
- 控制台 origin 可用。之前流传的"控制台 origin 损坏"实际是无人在线时没有已加载区块，所有方块命令都会报 "Cannot access blocks outside of the world"；用 ticking area 拉起区块后一切正常。
- 实体 origin 派发命令是静默 no-op，且对任何命令都返回 True（包括加载不存在的结构）。不要用它验证任何事情。
- 裸结构名自动补 `mystructure:` 命名空间。
- `structure delete` 子命令可用，删内存模板和 LevelDB 条目，跨重启有效。
- `structure save ... memory` 只进内存不落盘——束分片导出用它，重启后引擎内存为空，放置时按需从束目录重新导入（已实测：无行为包镜像 + 重启 + 自定义目录，按路径放置成功）。
- tickingarea 语法是三维矩形形式：`tickingarea add <x1> <y> <z1> <x2> <y> <z2> [名字]`，y 被忽略。
- 空模板序列化 156 字节，format_version=2；方块 version 字段 18168865（1.21.60 起未变）。
- Endstone 命令枚举 `(a|b|c)<action: Enum>` + 尾参 `[args: message]`：枚举值进 args[0]，message 整段进 args[1]（需自行做引号感知的再拆分，见 `splitSubArgs`）。
- `Actor::teleport`/`Level::getDimension` 走 Endstone 公开 API 即可完成 TP 辅助；TP 目标永远是发起者或 `tp_player` 指定的玩家。

## 已知的坑

- libc++ 的 std::format 对 `char[N]` 按 N 个字符的区间格式化（含 NUL 和栈上垃圾），传参前包一层 `std::string`。
- Windows 上 `fs::path` 的窄字符构造/`.string()` 走 ANSI 代码页，中文结构名必须经 `pathFromUtf8`/`pathToUtf8`（UTF-16 宽 API）转换。
- 社区文档声称 Bedrock LevelDB 块压缩类型 4 是 snappy——实测是 raw DEFLATE（RFC 1951，无 zlib 头）。详见 `src/leveldb_scan.h` 注释。
- Python 引用的 DECREF 必须在 GIL 区间内，dealloc 可能执行 Python 代码。
- endstone 从 `plugins/.local/<名字>-<hash>.so` 加载插件副本，消费方 dlopen 原路径会得到第二个实例、API 指针为空。
- Python 里导入插件基类用 `from endstone.plugin import Plugin`，`endstone.Plugin` 会因为 lazy_loader 报 AttributeError。
- 命令 usage 的参数必须带类型（`<name: string>`），裸 `<name>` 会被当枚举解析。
- Endstone 0.11.14 的 Python Scheduler 只有 `run_task`，没有 `run_task_later`——延迟逻辑要么轮询要么在 C++ 侧用 runTaskTimer。
- Endstone 的 Python Logger 不支持 %-style 格式化（`logger.info("%s", x)` 会把参数当多余实参报 TypeError），必须用 f-string。
- `fs::file_size` 失败返回 `(uintmax_t)-1`，不检查 error_code 就累加会得到天文数字（束总大小的显示 bug 就是这么来的）。
- dl* 符号在 glibc 2.34+ 会绑到新版本，`glibc_compat.h` 里用 .symver 钉回 2.2.5，链接时显式带上 libdl.so.2。
