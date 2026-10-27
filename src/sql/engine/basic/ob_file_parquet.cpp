/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/ob_file_parquet.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#if defined(__linux__)
#include <unistd.h>
#endif

namespace oceanbase { namespace sql {
using namespace common;
namespace file_parquet {
const Node &Node::get(int id) const
{
  static const Node empty;
  const auto found = fields.find(id); return found == fields.end() ? empty : found->second;
}
namespace {
bool varint(const std::string &s, size_t &p, uint64_t &n)
{
  n = 0;
  for (unsigned shift = 0; shift < 64 && p < s.size(); shift += 7) {
    const unsigned ch = static_cast<unsigned char>(s[p++]);
    if (shift == 63 && ch > 1) { return false; }
    n |= static_cast<uint64_t>(ch & 127) << shift;
    if (!(ch & 128)) { return true; }
  }
  return false;
}
uint64_t little(const char *p, size_t bytes)
{
  uint64_t value = 0;
  for (size_t i = 0; i < bytes; ++i) { value |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i); }
  return value;
}
// Read the bounded Compact Thrift metadata tree used by Parquet. No query
// evaluation or data materialization occurs in this decoder.
bool compact(const std::string &s, size_t &p, int type, Node &node, int depth, size_t &budget)
{
  if (depth > 32 || budget-- == 0) { return false; }
  uint64_t n = 0;
  if (type == 1 || type == 2) { node.number = type == 1; return true; }
  if (type == 3) { if (p >= s.size()) { return false; } node.number = static_cast<int8_t>(s[p++]); return true; }
  if (type >= 4 && type <= 6) {
    if (!varint(s, p, n)) { return false; }
    node.number = static_cast<int64_t>((n >> 1) ^ (0ULL - (n & 1))); return true;
  }
  if (type == 7) { if (s.size() - p < 8) { return false; } p += 8; return true; }
  if (type == 8) {
    if (!varint(s, p, n) || n > s.size() - p) { return false; }
    node.bytes.assign(s, p, n); p += n; return true;
  }
  if (type == 9 || type == 10) {
    if (p >= s.size()) { return false; }
    const unsigned h = static_cast<unsigned char>(s[p++]); n = h >> 4;
    if (n == 15 && !varint(s, p, n)) { return false; }
    if (n > budget || n > s.size()) { return false; }
    for (uint64_t i = 0; i < n; ++i) {
      Node child;
      if (!compact(s, p, h & 15, child, depth + 1, budget)) { return false; }
      node.items.push_back(std::move(child));
    }
    return true;
  }
  if (type == 11) {
    if (!varint(s, p, n) || n > budget || (n && p >= s.size())) { return false; }
    const unsigned types = n ? static_cast<unsigned char>(s[p++]) : 0;
    for (uint64_t i = 0; i < n; ++i) {
      Node key, value;
      if (!compact(s, p, types >> 4, key, depth + 1, budget)
          || !compact(s, p, types & 15, value, depth + 1, budget)) { return false; }
    }
    return true;
  }
  if (type == 12) {
    int field = 0;
    while (p < s.size()) {
      const unsigned h = static_cast<unsigned char>(s[p++]);
      if (!h) { return true; }
      if (h >> 4) { field += h >> 4; }
      else {
        if (!varint(s, p, n)) { return false; }
        field = static_cast<int>((n >> 1) ^ (0ULL - (n & 1)));
      }
      if (field <= 0 || node.has(field)) { return false; }
      if (!compact(s, p, h & 15, node.fields[field], depth + 1, budget)) { return false; }
    }
  }
  return false;
}
bool parse(const std::string &bytes, Node &node, size_t &position)
{
  size_t budget = 200000;
  return compact(bytes, position, 12, node, 0, budget);
}
// Raw Snappy block format, including overlapping copy operations.
bool unsnappy(const std::string &input, size_t expected, std::string &output)
{
  size_t pos = 0; uint64_t length = 0;
  if (!varint(input, pos, length) || length != expected || length > FileScan::MAX_RECORD) { return false; }
  output.clear(); output.reserve(length);
  while (pos < input.size() && output.size() < length) {
    const unsigned tag = static_cast<unsigned char>(input[pos++]);
    size_t count = 0, offset = 0;
    if ((tag & 3) == 0) {
      count = tag >> 2;
      if (count < 60) { ++count; }
      else {
        const size_t bytes = count - 59;
        if (bytes > 4 || input.size() - pos < bytes) { return false; }
        count = little(input.data() + pos, bytes) + 1; pos += bytes;
      }
      if (count > input.size() - pos || count > length - output.size()) { return false; }
      output.append(input, pos, count); pos += count;
    } else {
      const size_t bytes = (tag & 3) == 1 ? 1 : (tag & 3) == 2 ? 2 : 4;
      if (input.size() - pos < bytes) { return false; }
      if ((tag & 3) == 1) { count = 4 + ((tag >> 2) & 7); offset = ((tag & 224) << 3) | little(input.data() + pos, 1); }
      else { count = 1 + (tag >> 2); offset = little(input.data() + pos, bytes); }
      pos += bytes;
      if (!offset || offset > output.size() || count > length - output.size()) { return false; }
      for (size_t i = 0; i < count; ++i) { output += output[output.size() - offset]; }
    }
  }
  return output.size() == length && pos == input.size();
}
bool decompress(int codec, const std::string &input, size_t expected, std::string &output)
{
  if (codec == 0) { output = input; return output.size() == expected; }
  return codec == 1 && unsnappy(input, expected, output);
}
bool rle(const std::string &bytes, size_t &pos, size_t end, unsigned width, size_t count, std::vector<uint32_t> &out)
{
  if (width > 32 || end > bytes.size() || count > FileScan::MAX_RECORD) { return false; }
  out.clear(); out.reserve(count);
  while (out.size() < count && pos < end) {
    uint64_t run = 0;
    if (!varint(bytes, pos, run) || pos > end || run < 2) { return false; }
    if (run & 1) {
      const uint64_t groups = run >> 1;
      if (groups > (end - pos) / std::max(1U, width)) { return false; }
      const size_t values = groups * 8;
      for (size_t i = 0; i < values && out.size() < count; ++i) {
        uint32_t value = 0;
        for (unsigned bit = 0; bit < width; ++bit) {
          const size_t offset = i * width + bit;
          value |= ((static_cast<unsigned char>(bytes[pos + offset / 8]) >> (offset % 8)) & 1U) << bit;
        }
        out.push_back(value);
      }
      pos += groups * width;
    } else {
      const size_t length = run >> 1, nbytes = (width + 7) / 8;
      if (nbytes > end - pos || length > FileScan::MAX_RECORD) { return false; }
      const uint32_t value = little(bytes.data() + pos, nbytes); pos += nbytes;
      for (size_t i = 0; i < length && out.size() < count; ++i) { out.push_back(value); }
    }
  }
  return out.size() == count;
}
std::string decimal(double value)
{
  char buffer[64]; std::snprintf(buffer, sizeof(buffer), "%.17g", value); return buffer;
}
}
}
using namespace file_parquet;
int ParquetScan::read_at(int64_t offset, size_t length, std::string &data)
{
#if defined(__linux__)
  if (offset < 0 || length > FileScan::MAX_RECORD || offset > footer_start_ || length > footer_start_ - offset) { return OB_INVALID_DATA; }
  data.resize(length);
  size_t done = 0;
  while (done < length) {
    const ssize_t n = pread(fileno(file_), &data[done], length - done, offset + done);
    if (n <= 0) { return OB_IO_ERROR; }
    done += n;
  }
  return OB_SUCCESS;
#else
  return OB_NOT_SUPPORTED;
#endif
}
int ParquetScan::open(FILE *file, uint64_t size, std::vector<FileColumn> &columns)
{
  file_ = file; footer_start_ = size; group_ = 0; row_ = rows_ = 0; columns_.clear(); cursors_.clear();
  if (size < 12 || size > INT64_MAX) { return OB_INVALID_DATA; }
  std::string tail, magic;
  int ret = read_at(size - 8, 8, tail);
  if (OB_FAIL(ret) || tail.substr(4) != "PAR1" || OB_FAIL(ret = read_at(0, 4, magic)) || magic != "PAR1") { return OB_INVALID_DATA; }
  const uint64_t length = little(tail.data(), 4);
  if (length > FileScan::MAX_RECORD || length > size - 12) { return OB_INVALID_DATA; }
  std::string bytes;
  if (OB_FAIL(read_at(size - 8 - length, length, bytes))) { return ret; }
  footer_start_ = size - 8 - length;
  size_t position = 0; footer_ = Node();
  if (!parse(bytes, footer_, position) || position != bytes.size()) { return OB_INVALID_DATA; }
  const auto &schema = footer_.get(2).items;
  if (schema.size() < 2 || schema.size() > FileScan::MAX_COLUMNS + 1
      || schema[0].get(5).number != schema.size() - 1) { return OB_NOT_SUPPORTED; }
  columns.clear();
  for (size_t i = 1; i < schema.size(); ++i) {
    const Node &s = schema[i];
    if (!s.has(1) || s.has(5) || s.get(3).number > 1 || s.get(4).bytes.empty()) { return OB_NOT_SUPPORTED; }
    Column c; c.physical = s.get(1).number; c.optional = s.get(3).number == 1;
    c.fixed_length = s.get(2).number; c.logical = s.has(6) ? s.get(6).number : -1;
    FileColumn column; column.name = s.get(4).bytes; column.nullable = c.optional;
    if (c.physical == 0) { column.type = ObTinyIntType; }
    else if (c.physical == 1 || c.physical == 2) { column.type = ObIntType; }
    else if (c.physical == 4 || c.physical == 5) { column.type = ObDoubleType; }
    else if (c.physical == 6 || c.physical == 7) { column.type = ObVarcharType; }
    else if (c.physical == 3) { column.type = ObDateTimeType; }
    else { return OB_NOT_SUPPORTED; }
    const Node &logical = s.get(10);
    if (c.logical == 6 || logical.has(6)) { column.type = ObDateType; c.logical = 6; }
    if (c.logical == 9 || c.logical == 10 || logical.has(8)) {
      column.type = ObDateTimeType; c.timestamp_unit = c.logical == 9 ? 1000 : 1000000;
      const Node &unit = logical.get(8).get(2);
      if (unit.has(1)) { c.timestamp_unit = 1000; }
      if (unit.has(2)) { c.timestamp_unit = 1000000; }
      if (unit.has(3)) { c.timestamp_unit = 1000000000; }
    }
    if (c.logical == 5 || logical.has(2) || logical.has(3) || logical.has(5) || logical.has(7)) { return OB_NOT_SUPPORTED; }
    for (const auto &old : columns) { if (strcasecmp(old.name.c_str(), column.name.c_str()) == 0) { return OB_INVALID_DATA; } }
    if (column.name.size() > 256) { return OB_INVALID_DATA; }
    columns.push_back(column); columns_.push_back(c);
  }
  for (const auto &group : footer_.get(4).items) {
    if (group.get(1).items.size() != columns_.size() || group.get(3).number < 0) { return OB_INVALID_DATA; }
    for (const auto &chunk : group.get(1).items) {
      if (!chunk.get(1).bytes.empty() || !chunk.has(3)) { return OB_NOT_SUPPORTED; }
    }
  }
  return OB_SUCCESS;
}
int ParquetScan::start_group()
{
  const auto &groups = footer_.get(4).items;
  if (group_ >= groups.size()) { return OB_ITER_END; }
  const Node &group = groups[group_++]; rows_ = group.get(3).number; row_ = 0;
  cursors_.assign(columns_.size(), Cursor());
  for (size_t i = 0; i < columns_.size(); ++i) {
    const Node &metadata = group.get(1).items[i].get(3);
    Cursor &c = cursors_[i]; c.codec = metadata.get(4).number;
    c.offset = metadata.has(11) ? std::min(metadata.get(9).number, metadata.get(11).number) : metadata.get(9).number;
    const int64_t length = metadata.get(7).number;
    if (c.offset < 4 || length < 0 || c.offset > footer_start_ || length > footer_start_ - c.offset
        || metadata.get(5).number != rows_) { return OB_INVALID_DATA; }
    c.end = c.offset + length;
  }
  return OB_SUCCESS;
}
int ParquetScan::decode_plain(const std::string &bytes, size_t pos, size_t count,
                             const Column &column, std::vector<FileCell> &values)
{
  values.clear();
  if (count > FileScan::MAX_RECORD / sizeof(FileCell)) { return OB_SIZE_OVERFLOW; }
  const size_t boolean_start = pos;
  for (size_t i = 0; i < count; ++i) {
    FileCell cell; cell.is_null = false;
    const int type = column.physical;
    size_t length = type == 1 || type == 4 ? 4 : type == 2 || type == 5 ? 8 : type == 3 ? 12 : 0;
    if (type == 0) {
      if (boolean_start + i / 8 >= bytes.size()) { return OB_INVALID_DATA; }
      cell.text = (static_cast<unsigned char>(bytes[boolean_start + i / 8]) >> (i % 8)) & 1 ? "true" : "false";
    } else if (type == 6 || type == 7) {
      if (type == 6) {
        if (bytes.size() - pos < 4) { return OB_INVALID_DATA; }
        length = little(bytes.data() + pos, 4); pos += 4;
      } else { length = column.fixed_length; }
      if (length > bytes.size() - pos) { return OB_INVALID_DATA; }
      cell.text.assign(bytes, pos, length); pos += length;
    } else {
      if (length == 0 || length > bytes.size() - pos) { return OB_INVALID_DATA; }
      if (type == 4) {
        const uint32_t bits = little(bytes.data() + pos, 4); float value; std::memcpy(&value, &bits, 4);
        if (!std::isfinite(value)) { return OB_INVALID_DATA; } cell.text = decimal(value);
      } else if (type == 5) {
        const uint64_t bits = little(bytes.data() + pos, 8); double value; std::memcpy(&value, &bits, 8);
        if (!std::isfinite(value)) { return OB_INVALID_DATA; } cell.text = decimal(value);
      } else {
        int64_t value = type == 1 ? static_cast<int32_t>(little(bytes.data() + pos, 4)) : static_cast<int64_t>(little(bytes.data() + pos, 8));
        const bool date = column.logical == 6;
        if (date || column.timestamp_unit || type == 3) {
          const int64_t seconds = date ? value * 86400 : type == 3
              ? (static_cast<int64_t>(little(bytes.data() + pos + 8, 4)) - 2440588) * 86400 + value / 1000000000
              : value / column.timestamp_unit;
          const time_t epoch = seconds; struct tm time; char buffer[32];
          bool valid_time = false;
#ifdef _WIN32
          valid_time = gmtime_s(&time, &epoch) == 0;
#else
          valid_time = nullptr != gmtime_r(&epoch, &time);
#endif
          if (!valid_time || time.tm_year < -1899 || time.tm_year > 8099) { return OB_INVALID_DATA; }
          std::strftime(buffer, sizeof(buffer), date ? "%Y-%m-%d" : "%Y-%m-%d %H:%M:%S", &time); cell.text = buffer;
        } else { cell.text = std::to_string(value); }
      }
      pos += length;
    }
    values.push_back(std::move(cell));
  }
  return OB_SUCCESS;
}
int ParquetScan::read_page(size_t column_index)
{
  Cursor &c = cursors_[column_index];
  const Column &column = columns_[column_index];
  while (c.offset < c.end) {
    std::string header;
    int ret = read_at(c.offset, std::min<int64_t>(65536, c.end - c.offset), header);
    if (OB_FAIL(ret)) { return ret; }
    Node page; size_t hlength = 0;
    if (!parse(header, page, hlength) || !page.has(1) || !page.has(2) || !page.has(3)) { return OB_INVALID_DATA; }
    const int64_t compressed = page.get(3).number, uncompressed = page.get(2).number;
    if (compressed < 0 || uncompressed < 0 || compressed > FileScan::MAX_RECORD || uncompressed > FileScan::MAX_RECORD
        || hlength > c.end - c.offset || compressed > c.end - c.offset - hlength) { return OB_INVALID_DATA; }
    std::string packed, bytes;
    if (OB_FAIL(read_at(c.offset + hlength, compressed, packed))) { return ret; }
    c.offset += hlength + compressed;
    const int type = page.get(1).number;
    const Node &details = page.get(type == 0 ? 5 : type == 2 ? 7 : 8);
    const int64_t count = details.get(1).number;
    if (count < 0 || count > FileScan::MAX_RECORD / sizeof(FileCell)) { return OB_SIZE_OVERFLOW; }
    if (type == 2) {
      if (details.get(2).number != 0 || !c.dictionary.empty()) { return OB_NOT_SUPPORTED; }
      if (!decompress(c.codec, packed, uncompressed, bytes)) { return OB_INVALID_DATA; }
      if (OB_FAIL(decode_plain(bytes, 0, count, column, c.dictionary))) { return ret; }
      continue;
    }
    if (type != 0 && type != 3) { return OB_NOT_SUPPORTED; }
    size_t pos = 0;
    std::vector<uint32_t> definitions;
    int encoding = details.get(type == 0 ? 2 : 4).number;
    if (type == 0) {
      if (!decompress(c.codec, packed, uncompressed, bytes)) { return OB_INVALID_DATA; }
      if (column.optional) {
        if (details.get(3).number != 3 || bytes.size() < 4) { return OB_NOT_SUPPORTED; }
        const size_t length = little(bytes.data(), 4); pos = 4;
        if (length > bytes.size() - pos || !rle(bytes, pos, 4 + length, 1, count, definitions)) { return OB_INVALID_DATA; }
        pos = 4 + length;
      }
    } else {
      const int64_t def_length = details.get(5).number, rep_length = details.get(6).number;
      if (def_length < 0 || rep_length != 0 || def_length > packed.size() || def_length > uncompressed) { return OB_INVALID_DATA; }
      if (column.optional && !rle(packed, pos, def_length, 1, count, definitions)) { return OB_INVALID_DATA; }
      std::string values = packed.substr(def_length);
      const int codec = details.has(7) && !details.get(7).number ? 0 : c.codec;
      if (!decompress(codec, values, uncompressed - def_length, bytes)) { return OB_INVALID_DATA; }
      pos = 0;
    }
    if (!column.optional) { definitions.assign(count, 1); }
    const size_t nonnull = std::count(definitions.begin(), definitions.end(), 1U);
    if (std::any_of(definitions.begin(), definitions.end(), [](uint32_t d) { return d > 1; })) { return OB_INVALID_DATA; }
    std::vector<FileCell> values;
    if (encoding == 0) {
      if (OB_FAIL(decode_plain(bytes, pos, nonnull, column, values))) { return ret; }
    } else if (encoding == 3 && column.physical == 0) {
      if (pos > bytes.size() || bytes.size() - pos < 4) { return OB_INVALID_DATA; }
      const size_t length = little(bytes.data() + pos, 4); pos += 4;
      std::vector<uint32_t> booleans;
      if (length > bytes.size() - pos || !rle(bytes, pos, pos + length, 1, nonnull, booleans)) { return OB_INVALID_DATA; }
      for (uint32_t value : booleans) {
        if (value > 1) { return OB_INVALID_DATA; }
        values.push_back(FileCell{value ? "true" : "false", false, ObTinyIntType});
      }
    } else if (encoding == 2 || encoding == 8) {
      if (pos >= bytes.size() || c.dictionary.empty()) { if (nonnull == 0) { values.clear(); } else { return OB_INVALID_DATA; } }
      else {
        const unsigned width = static_cast<unsigned char>(bytes[pos++]); std::vector<uint32_t> indices;
        if (!rle(bytes, pos, bytes.size(), width, nonnull, indices)) { return OB_INVALID_DATA; }
        for (uint32_t index : indices) {
          if (index >= c.dictionary.size()) { return OB_INVALID_DATA; }
          values.push_back(c.dictionary[index]);
        }
      }
    } else { return OB_NOT_SUPPORTED; }
    c.page.clear(); c.position = 0; size_t index = 0;
    for (uint32_t defined : definitions) { c.page.push_back(defined ? std::move(values[index++]) : FileCell()); }
    if (!c.page.empty()) {
      size_t memory = 0;
      for (const auto &cursor : cursors_) {
        memory += (cursor.page.capacity() + cursor.dictionary.capacity()) * sizeof(FileCell);
        for (const auto &cell : cursor.page) { memory += cell.text.capacity(); }
        for (const auto &cell : cursor.dictionary) { memory += cell.text.capacity(); }
      }
      if (memory > 128 * 1024 * 1024) { return OB_SIZE_OVERFLOW; }
      return OB_SUCCESS;
    }
  }
  return OB_INVALID_DATA;
}
int ParquetScan::next(std::vector<FileCell> &row)
{
  int ret = OB_SUCCESS;
  while (row_ >= rows_) { if (OB_FAIL(start_group())) { return ret; } }
  row.assign(columns_.size(), FileCell());
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (projected_ && std::find(projection_.begin(), projection_.end(), i) == projection_.end()) { continue; }
    Cursor &cursor = cursors_[i];
    if (cursor.position >= cursor.page.size() && OB_FAIL(read_page(i))) { return ret; }
    row[i] = cursor.page[cursor.position++];
  }
  ++row_; return OB_SUCCESS;
}
} }
