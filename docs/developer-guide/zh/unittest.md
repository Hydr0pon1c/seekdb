# 编写与运行单元测试

seekdb 使用 [Google Test](https://google.github.io/googletest/) 编写 C++ 单元测试。测试位于 `unittest/`，并按 CMake 模块组织为 `observer_tests`、`storage_tests`、`sql_tests` 等目标。

## 配置测试构建

CMake 单元测试目标仅在 Linux 上提供。编译模块测试前，先初始化依赖并配置 Release 构建：

```bash
./build.sh release --init
```

`--init` 通常只在首次构建或依赖定义变化后使用。后续需要重新配置时，执行不带 `--init` 的 `./build.sh release`。

## 选择并编译模块

使用以下命令列出可用的模块目标：

```bash
make -C build_release help | grep '_tests'
```

编译受修改影响的模块，例如：

```bash
make -C build_release observer_tests
make -C build_release storage_tests
```

每个测试二进制位于 `build_release/unittest/<module>/<module>_tests`。

## 运行模块测试

配置阶段生成的 `run_tests.sh` 会通过 CTest 设置测试所需的环境和工作目录。使用正则表达式运行一个模块的全部分片，例如：

```bash
./build_release/unittest/run_tests.sh \
  -R '^observer_tests_shard_[0-9]+$' \
  --output-on-failure
```

只运行一个 Google Test 用例时，在保留模块分片筛选的同时设置 `GTEST_FILTER`：

```bash
GTEST_FILTER='ComponentName.handles_invalid_input' \
  ./build_release/unittest/run_tests.sh \
  -R '^observer_tests_shard_[0-9]+$' \
  --output-on-failure
```

先运行覆盖修改范围的最小模块和用例，再根据需要扩大测试范围。

## 编写测试

测试文件使用 `test_*.cpp` 命名，放在对应的 `unittest/<module>/` 目录下，包含 `<gtest/gtest.h>`，并编写聚焦的 `TEST` 或 `TEST_F` 用例。CMake 会自动发现模块中的 C++ 测试源文件。沿用模块的共享 main 模式，不要单独添加 `main()`。

```cpp
TEST(ComponentName, handles_invalid_input)
{
  ASSERT_EQ(OB_INVALID_ARGUMENT, call_with_invalid_input());
}
```

当失败后无法继续执行测试时使用 `ASSERT_*`；后续检查仍有意义时使用 `EXPECT_*`。

## 编译并运行全部模块

`pretest` 目标会编译全部 CMake 单元测试模块。编译完成后，再单独运行已注册的 CTest 分片：

```bash
make -C build_release pretest
./build_release/unittest/run_tests.sh --output-on-failure
```

适合并行执行时，可以向 `run_tests.sh` 传入 `-j <jobs>`。Pull Request 的 CI 也会运行单元测试和 mysqltest。请在 Pull Request 描述中记录实际执行的目标和筛选条件。

## 比赛分支的 SQL 扩展回归

`sql_tests` 包含 AI Prompt/Provider 响应校验、CSV/JSONL 类型推断与扫描、
Parquet V1/V2 数据页及 Snappy、文件变化与路径检查、文件 SQL/COPY 解析测试。

```bash
make -C build_release -j8 sql_tests
./build_release/unittest/run_tests.sh -R '^sql_tests_shard_0$' --output-on-failure
```

端到端测试需要一个本地 seekdb 实例和 Python `pymysql`。在两个终端分别运行：

```bash
python3 unittest/sql/engine/expr/mock_ai_service.py --port 8000
```

```bash
python3 unittest/sql/test_competition_integration.py \
  --port 2881 --fixture-parent /path/allowed/by/secure_file_priv
```

集成测试使用独立的临时文件和带随机前缀的表/模型/Endpoint，并在结束时清理。
它验证 seekdb 原生聚合、过滤、排序、跨格式 JOIN、DESCRIBE、COPY 读回及失败清理，
以及 AI 请求配置、Ollama Provider 修改、Embedding 多输入请求与跨行映射、Rerank 分批稳定排序。
Mock 默认返回 768 维 Embedding，并反转 OpenAI Embedding 响应顺序；显式维度来自请求参数。
它只用于测试，不需要真实模型或 API Key。

文件 SQL 默认关闭；SQL 会话中需先设置 `SET enable_file_sql=ON`。
`secure_file_priv` 仍限制读取和 COPY 导出的目录，修改该变量须通过实例的 Unix Socket 连接。
当前文件扫描实现面向 Linux 单进程；Parquet 支持扁平基础类型、可空列、多个 Row Group、
PLAIN/字典/RLE 编码、未压缩或 Snappy 压缩，以及按查询引用列读取。
CSV/JSONL Schema 做独立的全量扫描，随后逐记录执行查询；`sample_size` 仅接受 `-1`。
单条记录/数据页上限为 16 MiB，列数上限为 1024，Parquet 页/字典保留内存上限为 128 MiB，
`glob` 最多发现 10000 个普通文件。其他编码或嵌套结构会明确报错。
