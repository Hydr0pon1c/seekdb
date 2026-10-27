/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include <unistd.h>
#include "sql/engine/basic/ob_file_scan_utils.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
namespace {
struct TempFile
{
  std::string path;
  explicit TempFile(const std::string &data) {
    char pattern[] = "/tmp/seekdb-file-test-XXXXXX";
    const int fd = mkstemp(pattern); path = pattern;
    if (fd >= 0) { EXPECT_EQ(data.size(), write(fd, data.data(), data.size())); close(fd); }
  }
  ~TempFile() { unlink(path.c_str()); }
};
}
TEST(FileScanUtils, deterministic_types_and_widening)
{
  EXPECT_EQ(ObIntType, FileScan::infer_type("9223372036854775807"));
  EXPECT_EQ(ObIntType, FileScan::infer_type("-9223372036854775808"));
  EXPECT_EQ(ObVarcharType, FileScan::infer_type("9223372036854775808"));
  EXPECT_EQ(ObVarcharType, FileScan::infer_type("1e999"));
  EXPECT_EQ(ObVarcharType, FileScan::infer_type("nan"));
  EXPECT_EQ(ObVarcharType, FileScan::infer_type("2025-02-29"));
  EXPECT_EQ(ObDateType, FileScan::infer_type("2024-02-29"));
  EXPECT_EQ(ObVarcharType, FileScan::infer_type("2024-01-01 24:00:00"));
  EXPECT_EQ(ObDoubleType, FileScan::widen(ObIntType, ObDoubleType));
  EXPECT_EQ(ObVarcharType, FileScan::widen(ObTinyIntType, ObIntType));
  EXPECT_EQ(ObDateTimeType, FileScan::widen(ObDateType, ObDateTimeType));
}
TEST(FileScanUtils, multiline_csv_quotes_nulls_and_rescan)
{
  TempFile file("id,note,id\r\n1,\"hello,\nworld\",2\r\n3,\"say \"\"hi\"\"\",4\r\n5,,6\r\n");
  FileScanConfig config; config.path = file.path; config.format = "csv";
  ASSERT_EQ(OB_SUCCESS, FileScan::infer(config));
  ASSERT_EQ(3, config.columns.size());
  EXPECT_EQ("id__2", config.columns[2].name);
  FileScan scan; ASSERT_EQ(OB_SUCCESS, scan.open(config)); ASSERT_EQ(OB_SUCCESS, scan.rescan());
  std::vector<FileCell> row;
  ASSERT_EQ(OB_SUCCESS, scan.next(row)); EXPECT_EQ("hello,\nworld", row[1].text);
  ASSERT_EQ(OB_SUCCESS, scan.next(row)); EXPECT_EQ("say \"hi\"", row[1].text);
  ASSERT_EQ(OB_SUCCESS, scan.next(row)); EXPECT_TRUE(row[1].is_null);
  EXPECT_EQ(OB_ITER_END, scan.next(row));
  ASSERT_EQ(OB_SUCCESS, scan.rescan()); ASSERT_EQ(OB_SUCCESS, scan.next(row)); EXPECT_EQ("1", row[0].text);
}
TEST(FileScanUtils, json_union_missing_conflicts_and_nested_rejection)
{
  TempFile file("{\"id\":1,\"a\":null}\n{\"id\":2,\"b\":true}\n{\"id\":2.5,\"a\":\"text\"}\n");
  FileScanConfig config; config.path = file.path; config.format = "json";
  ASSERT_EQ(OB_SUCCESS, FileScan::infer(config)); ASSERT_EQ(3, config.columns.size());
  for (const auto &column : config.columns) { if (column.name == "id") { EXPECT_EQ(ObDoubleType, column.type); } }
  FileScan scan; ASSERT_EQ(OB_SUCCESS, scan.open(config));
  std::vector<FileCell> row; ASSERT_EQ(OB_SUCCESS, scan.next(row)); EXPECT_TRUE(row[2].is_null);
  TempFile nested("{\"a\":[1,2]}\n"); config.path = nested.path; config.inode = 0;
  EXPECT_NE(OB_SUCCESS, FileScan::infer(config));
}
TEST(FileScanUtils, detects_changes_and_denies_path_escape)
{
  TempFile file("id\n1\n");
  FileScanConfig config; config.path = file.path; config.format = "csv";
  ASSERT_EQ(OB_SUCCESS, FileScan::infer(config));
  FileScan scan; ASSERT_EQ(OB_SUCCESS, scan.open(config));
  FILE *append = fopen(file.path.c_str(), "a"); ASSERT_NE(nullptr, append); fputs("2\n", append); fclose(append);
  EXPECT_NE(OB_SUCCESS, scan.check_changed());
  std::string canonical;
  EXPECT_NE(OB_SUCCESS, FileScan::check_access("/etc/passwd", ObString("/tmp"), canonical));
  EXPECT_NE(OB_SUCCESS, FileScan::check_access(file.path, ObString("NULL"), canonical));
  EXPECT_EQ(OB_SUCCESS, FileScan::check_access(file.path, ObString("/tmp"), canonical));
}
TEST(FileScanUtils, malformed_csv_and_config_roundtrip)
{
  TempFile file("id,name\n1,\"unterminated");
  FileScanConfig config; config.path = file.path; config.format = "csv";
  EXPECT_NE(OB_SUCCESS, FileScan::infer(config));
  config.columns = {FileColumn{"hello", ObIntType}};
  const std::string encoded = config.encode(); FileScanConfig decoded;
  ASSERT_EQ(OB_SUCCESS, decoded.decode(ObString(encoded.size(), encoded.data())));
  EXPECT_EQ(config.path, decoded.path); EXPECT_EQ(config.columns[0].name, decoded.columns[0].name);
}
TEST(FileScanUtils, parquet_snappy_dictionary_nulls_and_row_groups)
{
  for (const char *name : {"nullable-v1.parquet", "nullable-v2.parquet"}) {
    FileScanConfig config; config.format = "parquet";
    config.path = std::string("unittest/sql/engine/basic/data/") + name;
    char absolute[4096]; ASSERT_NE(nullptr, realpath(config.path.c_str(), absolute)); config.path = absolute;
    ASSERT_EQ(OB_SUCCESS, FileScan::infer(config)) << name;
    ASSERT_EQ(5, config.columns.size()); EXPECT_EQ(ObDateType, config.columns[4].type);
    FileScan scan; ASSERT_EQ(OB_SUCCESS, scan.open(config));
    std::vector<FileCell> row;
    for (int i = 1; i <= 5; ++i) {
      ASSERT_EQ(OB_SUCCESS, scan.next(row)) << name << " row " << i;
      EXPECT_EQ(std::to_string(i), row[0].text);
      if (i == 3) { EXPECT_TRUE(row[1].is_null); }
      if (i == 1) { EXPECT_EQ("2026-01-01", row[4].text); }
    }
    EXPECT_EQ(OB_ITER_END, scan.next(row));
    ASSERT_EQ(OB_SUCCESS, scan.rescan()); ASSERT_EQ(OB_SUCCESS, scan.next(row)); EXPECT_EQ("1", row[0].text);
  }
}

TEST(FileScanUtils, utf8_bom_and_headerless_csv)
{
  TempFile file("\xef\xbb\xbf" "1;True\n2;False\n");
  FileScanConfig config; config.path = file.path; config.format = "csv"; config.header = false; config.delimiter = ';';
  ASSERT_EQ(OB_SUCCESS, FileScan::infer(config));
  EXPECT_EQ(ObIntType, config.columns[0].type); EXPECT_EQ(ObTinyIntType, config.columns[1].type);
  FileScan scan; ASSERT_EQ(OB_SUCCESS, scan.open(config)); ASSERT_EQ(OB_SUCCESS, scan.rescan());
  std::vector<FileCell> row; ASSERT_EQ(OB_SUCCESS, scan.next(row)); EXPECT_EQ("1", row[0].text);
}
