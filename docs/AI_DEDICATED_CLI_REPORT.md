# SMBA 专用 AI CLI 交付报告

日期：2026-08-27

## 结论

'scripts/ai_cli.py' 已从通用 JSON 输入桥接器收敛为两个普通参数子命令：

~~~
ai_cli.py --target TARGET preview FUNCTION
ai_cli.py --target TARGET register-workflow FUNCTION
~~~

'FUNCTION' 只接受非负 '0x' 地址或精确函数名。地址与函数起点严格相等，
函数名严格字符串相等；找不到或不唯一均拒绝。顶层与两个子命令的 '-h'
完全在本地 argparse 层完成，不导入 Binary Ninja、不启动 'bn'，并明确说明
是否改分析、是否重分析、是否保存及示例。

最新版本闸门要求 Python 3.11 或更高版本。它只在 future-annotations 声明之后
导入 'sys'，并在 argparse、Binary Ninja 或任何可能依赖较新版本的导入之前
检查版本；系统 Python 3.9 的任何调用（含 '-h'）向 stderr 只输出
'ai_cli.py requires Python 3.11 or newer'，stdout 为空、无 traceback 且非零退出。

## 已实现的固定映射

| 公开子命令 | 唯一菜单命令 | 状态边界 |
| --- | --- | --- |
| 'preview' | 'SMBA CoBRA\Preview verified MBA simplifications' | 只读；不改所选函数分析，不保存。 |
| 'register-workflow' | 'SMBA CoBRA\Register or refresh current .mba workflow' | 仅注册/刷新；不选择 workflow、不重分析、不保存。 |

两个 in-Binary-Ninja 入口分别为 'run_preview' 和
'run_register_workflow'。二者均只迭代注册命令以找到自己的精确名字，然后
创建 function-only context、先调用 'is_valid'、再执行一个菜单命令；没有
公开或隐藏的通用 PluginCommand 列表/运行接口。

保留的 fail-closed 机制包括：

* 同步 'BNLogListener' 日志窗口与回调引用生命周期；
* 直接回调异常和日志中的 'Unhandled Python exception' 结构化失败；
* 原生 '[SMBA AI JSON]' 记录的固定 operation 过滤；
* 数值 'function_start' 与所选函数起点精确关联；跨函数记录、缺记录和非法
  记录均拒绝；
* 外部传输用 'Path(__file__).resolve()' 嵌入脚本绝对路径，因此不依赖当前
  工作目录。

已移除旧的 JSON/file 输入、输入版本字段、能力枚举和 workflow 激活/重分析
公开能力。JSON 仍仅作为输出格式；成功退出 '0'，拒绝或错误退出 '2'。
公开输出使用 'register-workflow'，原生记录继续使用既有
'register_workflow'，以免改变 C++ 的 '[SMBA AI JSON]' 通道。

## 测试与构建证据

Red 阶段先将测试改为新的公开接口，再以旧实现运行：

~~~text
uv run python -m unittest tests.test_ai_cli -v
结果：12 tests；8 AttributeError（run_preview、run_register_workflow、
invoke_external_preview 不存在）及 2 旧 JSON CLI 断言失败。
~~~

Green/回归阶段：

~~~text
uv run python -m unittest discover -s tests -p 'test_*.py' -q
结果：14 tests, OK

uvx --from cmake cmake --build build-core --parallel
uvx --from cmake ctest --test-dir build-core --output-on-failure
结果：2/2 passed

uvx --from cmake cmake --build build-plugin --parallel
uvx --from cmake ctest --test-dir build-plugin --output-on-failure
结果：2/2 passed

uv run python scripts/build_artifact.py
uv run python scripts/build_artifact.py --verify
结果：成功；manifest 校验通过。
~~~

版本闸门的 Red/Green：新增系统 Python 3.9 测试时，旧脚本的 '-h' 错误地
返回 0；加入闸门后同一测试通过，实际 '/usr/bin/python3'（3.9.6）确认仅
产生指定单行错误。当前 Python 回归测试总数为 14。

从 '/tmp' 使用脚本绝对路径已验证顶层、'preview' 和
'register-workflow' 帮助均退出 '0'。Python 测试同时覆盖 mocked external
transport：它检查传给 'bn py exec' 的程序包含来源脚本绝对路径和
'run_preview'，不含旧 JSON 输入对象；还验证旧 '--request-json' 被 argparse
拒绝且不启动 transport。

最终 artifact manifest 中 source/artifact 的 'ai_cli.py' 和 'README.md'
均以 'cmp -s' 验证一致。builder 现在在写入 manifest 前逐字节核对三个
payload 与对应来源；artifact 测试也覆盖 CLI 脚本的字节一致性。

## 文档和工件同步

更新了来源 README、专用 CLI/artifact 契约、构建测试说明和历史交付报告的
状态说明。重新运行 builder 后，以下工件已同步：

* 'artifact/plugin/smba_cobra_mba/ai_cli.py'
* 'artifact/plugin/smba_cobra_mba/README.md'
* 'artifact/plugin/smba_cobra_mba/manifest.json'
* 'artifact/plugin/smba_cobra_mba/smba-cobra-mba.dylib'

## 风险与偏差

未运行 live Binary Ninja 菜单命令、未保存 BNDB，也未改变默认 workflow、
SMBA 化简算法、DualBR 文件或机器码；菜单调用行为由 fake BN 对象和原生
headless JSON 测试覆盖。实际已安装 Binary Ninja 的版本兼容性仍依赖其
PluginCommand/BNLogListener API；本实现维持既有的
'PluginCommandContext(None)' 和不调用 'get_valid_list' 的规避策略。

'register-workflow' 的 'modified' 输出为 false，含义是所选函数分析未改变；
它并不否认菜单本身可能创建或刷新 workflow registry。这个语义在 help、
README 和专用契约中均已明确。
