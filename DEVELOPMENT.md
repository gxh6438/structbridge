# 开发说明

面向维护者的实现文档：构建流程与内部架构。

## 构建

### Linux

```bash
CXX=clang++-19 \
LIBCXX_A=/usr/lib/llvm-19/lib/libc++.a \
LIBCXXABI_A=/usr/lib/llvm-19/lib/libc++abi.a \
bash scripts/build_linux.sh
```

工具链要求 clang-19 + libc++ 19（clang-18 自带的 libc++ 18.1.3 无法编译 endstone 的 `platform/callstack.h`）。产物 `dist/endstone_structbridge.so`，glibc 2.17 兼容（脚本自带符号版本审计）。仅装了 clang-18 时，可用 `LIBCXX_INC` 指向 libc++ 19 的头文件目录。

依赖（Ubuntu 24.04）：`clang-19 lld-19 libc++-19-dev libc++abi-19-dev libboost-dev libglm-dev libmsgsl-dev python3-dev`。

### Windows

```bash
# 依赖：clang-cl、lld-link、libboost-dev / libglm-dev / libmsgsl-dev，
# 以及 xwin splat 出的 MSVC CRT + Windows SDK（默认 /opt/msvc）
bash scripts/build_windows.sh
```

xwin 用法：

```bash
curl -fsSL -o xwin.tar.gz \
  https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz
tar xzf xwin.tar.gz
./xwin-0.10.0-x86_64-unknown-linux-musl/xwin --accept-license --arch x86_64 splat --output /opt/msvc
```

### 端到端测试

需要一个 endstone 环境（Python 3.11+）：

```bash
pip install endstone==0.11.14
# 把 dist/*.so 放进 bedrock_server/plugins/，在服务器目录下启动：
{ sleep 75; echo "stop"; } | timeout 160 python -m endstone
```

`test/probe_plugin` 是一个 Python 测试插件，等待 API 可用后跑一遍全部接口（导入、导出、异步回调、删除、拒绝路径），装进同一个 venv 即可。

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

### 放置链（/struct place）

导出的逆操作，`PlaceJob` + `placeTick`（runTaskTimer）驱动：

```
名称解析（结构 | 库内束 | .bundle 路径 → resolveBundleRef）
  → manifest 展开（束：逐片；单结构：一片）
  → 目标区域 tickingarea 拉区块（预算不够时 TP job->tp_player_name——永远是指定/发起的那名玩家）
  → 分片不在引擎内存 → 从 src_dir 按需导入（引擎 load 链路）
  → 逐片派发 structure load <tile_key> <wx> <wy> <wz>（接缝由网格行列偏移对齐）
  → 完成：移除 area，回调 on_done
```

`original` 走 manifest（束）或 `meta.json`（单结构）里的原始区域/维度，目标坐标由分片 bounds 反推（束）或直接取 meta 记录（单结构），等价于把结构平移回导出时的位置。玩家离线（tp 不可用）时束任务允许 partial 完成并警告，单结构直接失败。

### 结构束（bundle）

引擎单模板上限 64×384×64，大区域由 `planBundle` 切成 64×64 网格（TILE=64，Y 不切，超 384 拒绝）。`bundleTick` 逐片走导出链，全部成功后写 `manifest.json`（原区域、维度、entities/blocks、每片的 file/bounds/key/done）；任何一片失败则标 `partial`，`tiles[].done` 定位缺口。

束的查找顺序：`findBundle`（结构库 `library/`）→ `resolveBundleRef`（带路径分隔符或 `.bundle` 后缀的引用，相对服务器根解析，`BundleManifest::src_dir` 记住来源目录供按需导入）。导出到自定义 out_dir 的束不写行为包镜像（`bundle_dir_override` 非空时跳过镜像，避免污染 vanilla 包）。

### 结构枚举（/struct list）

引擎没有结构枚举 API（StructureManager 只有 getOrCreate/getStructure），列表由插件自己合并来源、按引擎键去重（默认只列插件管理的，`all` 才合并全部三来源；束单独显示为一条记录）：

1. 插件结构库（`library/<ns>/<name>/<name>.mcstructure` 每结构一文件夹，`meta.json` 为插件自有 sidecar，`original` 复原的坐标/维度来源；仅此形状被识别为库条目）；
2. 各行为包的 `structures/` 目录（`structures/<ns>/<rest>.mcstructure` ⇔ 键 `<ns>:<rest>`）；行为包文件是只读游戏数据，只有插件自己写入 vanilla 包 `mystructure/` 的镜像可删；
3. 世界 LevelDB（`worlds/<level-name>/db`，level-name 从 server.properties 读取）——`structuretemplate_` 前缀的键。扫描器在 `src/leveldb_scan.{h,cpp}`，独立编译单元、只依赖标准库，磁盘格式说明记录在该头文件注释中。全部只读且 fail-soft：数据库归运行中的服务器所有，任何不一致只跳过对应文件或块。

删除（`/struct clear` 批量模式与 API 的 delete-all）只处理插件管理的条目（结构库 + 束 + 自有镜像），行为包游戏数据与世界独有存档不动；`clear` 需要二次输入 `confirm`。

### 名称与路径（UTF-8）

引擎对结构名没有字符限制，LevelDB 按原始 UTF-8 存键。插件的校验（`isValidStructureName`）接受一切合法 UTF-8 序列，拒绝控制字符、空格、引号、路径分隔符和第二个冒号；`splitStructureName` 原样保留多字节序列、只对小写 ASCII 折叠，`libraryRelativePath`（`<ns>/<name>/<name>.mcstructure`）与 `packRelativePath`（`<ns>/<name>.mcstructure`）基于它分别构造库与镜像路径。Windows 上 `fs::path` 的窄字符转换走 ANSI 代码页会损坏非 ASCII 文件名，所有路径构造和显示统一经过 `pathFromUtf8`/`pathToUtf8`（内部走 UTF-16 宽 API）。

### 删除

封装原版 `structure delete` 子命令（删引擎内存和 LevelDB 条目），再补删结构库和镜像两个文件。

### vtable shim

endstone 运行时不导出内部符号，插件必须自包含。StructureTemplate / StructureManager 的虚表前缀在源码里复刻（槽位即声明序，Itanium/MSVC ABI 下编译器自动算对），实际调用的只有 save / getStructure / getOrCreate / load 几个槽位。HashedString、BaseGameVersion 的构造函数在插件内本地定义。命令经插件描述注册。EndstoneLevel 的原生句柄通过直接访问 level_ 成员取得。

### 公开 API

`include/structbridge_api.h`：纯虚接口 + `extern "C"` 工厂函数，边界上只有 POD 类型（`const char*`、整数、函数指针），libstdc++ 消费方无需重编。Python 模块在 onLoad 时注入 sys.modules，回调挂在导出/放置任务上，在服务器线程触发；PyObject 的引用计数操作全部放在 GIL 区间内。

Windows 版本目前不编入 Python 模块：交叉编译环境没有 Python 头文件，而且链接特定的 python3xx.dll 会在进程里出现第二个解释器实例。C++ 接口不受影响。

### 命令注册

Endstone 命令的枚举语法 `(a|b|c)<action: Enum>` + 尾参 `[args: message]`：枚举值进 args[0]，message 整段进 args[1]，由 `splitSubArgs` 做引号感知的再拆分。usage 里的参数必须带类型（`<name: string>`）。
