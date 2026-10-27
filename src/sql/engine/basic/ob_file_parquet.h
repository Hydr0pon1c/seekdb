/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include <map>
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase { namespace sql {
namespace file_parquet {
struct Node
{
  int64_t number = 0;
  std::string bytes;
  std::map<int, Node> fields;
  std::vector<Node> items;
  const Node &get(int id) const;
  bool has(int id) const { return fields.count(id) != 0; }
};
struct Column
{
  int physical = 0;
  int logical = -1;
  bool optional = false;
  int fixed_length = 0;
  int64_t timestamp_unit = 0;
};
struct Cursor
{
  int64_t offset = 0, end = 0;
  int codec = 0;
  std::vector<FileCell> dictionary, page;
  size_t position = 0;
};
}

class ParquetScan
{
public:
  int open(FILE *file, uint64_t size, std::vector<FileColumn> &columns);
  int next(std::vector<FileCell> &row);
  void set_projection(const std::vector<int64_t> &projection) { projection_ = projection; projected_ = true; }
private:
  int start_group();
  int read_page(size_t column);
  int read_at(int64_t offset, size_t length, std::string &data);
  int decode_plain(const std::string &bytes, size_t pos, size_t count,
                   const file_parquet::Column &column, std::vector<FileCell> &values);
  FILE *file_ = nullptr;
  int64_t footer_start_ = 0;
  file_parquet::Node footer_;
  std::vector<file_parquet::Column> columns_;
  std::vector<file_parquet::Cursor> cursors_;
  std::vector<int64_t> projection_;
  bool projected_ = false;
  size_t group_ = 0;
  int64_t row_ = 0, rows_ = 0;
};
} }
