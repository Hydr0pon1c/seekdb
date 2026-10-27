/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdio>
#include <string>
#include <vector>
#include "common/object/ob_object.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase { namespace sql {

class ParquetScan;

struct FileColumn
{
  std::string name;
  common::ObObjType type = common::ObNullType;
  bool nullable = false;
};
struct FileCell
{
  std::string text;
  bool is_null = true;
  common::ObObjType type = common::ObNullType;
};
struct FileScanConfig
{
  std::string path;
  std::string format;
  std::string null_text;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::vector<FileColumn> columns;
  uint64_t device = 0, inode = 0, size = 0;
  int64_t mtime_sec = 0, mtime_nsec = 0, ctime_sec = 0, ctime_nsec = 0;
  std::string encode() const;
  int decode(const common::ObString &encoded);
};

// A scan keeps a single record in memory. Schema discovery makes a separate
// full pass and retains only column names and widened types.
class FileScan
{
public:
  static constexpr size_t MAX_RECORD = 16 * 1024 * 1024;
  static constexpr size_t MAX_COLUMNS = 1024;
  FileScan() = default;
  ~FileScan() { close(); }
  FileScan(const FileScan &) = delete;
  FileScan &operator=(const FileScan &) = delete;
  static int infer(FileScanConfig &config);
  static int check_access(const std::string &path, const common::ObString &secure_dir,
                          std::string &canonical);
  static common::ObObjType infer_type(const std::string &value);
  static common::ObObjType widen(common::ObObjType left, common::ObObjType right);
  int open(const FileScanConfig &config);
  int next(std::vector<FileCell> &row);
  int rescan();
  void set_projection(const std::vector<int64_t> &columns);
  int check_changed() const;
  void close();
  const std::string &error() const { return error_; }
private:
  int read_csv(std::vector<FileCell> &row);
  int read_json(std::vector<FileCell> &row, std::vector<std::string> &names);
  int read_record(std::vector<FileCell> &row, std::vector<std::string> &names);
  int snapshot(FileScanConfig &config) const;
  int fail(const char *message);
  FILE *file_ = nullptr;
  ParquetScan *parquet_ = nullptr;
  FileScanConfig config_;
  int64_t line_ = 1;
  std::vector<std::string> matches_;
  size_t match_pos_ = 0;
  std::string error_;
  common::ObArenaAllocator json_allocator_;
};

} }
