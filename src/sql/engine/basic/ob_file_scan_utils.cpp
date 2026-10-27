/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/ob_file_parquet.h"
#include <new>
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/charset/ob_charset.h"
#include "share/ob_errno.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#if defined(__linux__)
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <limits.h>

namespace oceanbase { namespace sql {
using namespace common;
namespace {
void put(std::string &out, const std::string &value)
{
  out += std::to_string(value.size()); out += ':'; out += value;
}
bool take(const std::string &in, size_t &pos, std::string &value)
{
  size_t length = 0, start = pos;
  while (pos < in.size() && in[pos] >= '0' && in[pos] <= '9') {
    if (length > FileScan::MAX_RECORD / 10) { return false; }
    length = length * 10 + in[pos++] - '0';
  }
  if (pos == start || pos >= in.size() || in[pos++] != ':' || length > in.size() - pos) { return false; }
  value.assign(in, pos, length); pos += length; return true;
}
bool date_valid(const std::string &s, bool timestamp)
{
  if (s.size() != (timestamp ? 19U : 10U) || s[4] != '-' || s[7] != '-') { return false; }
  for (size_t i = 0; i < s.size(); ++i) {
    if (i == 4 || i == 7 || (timestamp && (i == 10 || i == 13 || i == 16))) { continue; }
    if (s[i] < '0' || s[i] > '9') { return false; }
  }
  const int year = std::atoi(s.substr(0, 4).c_str());
  const int month = std::atoi(s.substr(5, 2).c_str());
  const int day = std::atoi(s.substr(8, 2).c_str());
  const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  if (year < 1 || month < 1 || month > 12 || day < 1 || day > days[month] + (month == 2 && leap)) { return false; }
  return !timestamp || ((s[10] == ' ' || s[10] == 'T') && s[13] == ':' && s[16] == ':'
      && std::atoi(s.substr(11, 2).c_str()) < 24 && std::atoi(s.substr(14, 2).c_str()) < 60
      && std::atoi(s.substr(17, 2).c_str()) < 60);
}
void skip_bom(FILE *file)
{
  unsigned char bytes[3];
  const size_t size = fread(bytes, 1, 3, file);
  if (size != 3 || bytes[0] != 0xef || bytes[1] != 0xbb || bytes[2] != 0xbf) { std::fseek(file, 0, SEEK_SET); }
}
bool same_name(const std::string &a, const std::string &b)
{
  return ObCharset::case_insensitive_equal(ObString(a.size(), a.data()), ObString(b.size(), b.data()));
}
}

std::string FileScanConfig::encode() const
{
  std::string out;
  for (const auto &s : {path, format, null_text, std::string(1, delimiter), std::string(1, quote), std::string(1, escape)}) { put(out, s); }
  for (const auto &s : {std::to_string(header), std::to_string(device), std::to_string(inode), std::to_string(size),
      std::to_string(mtime_sec), std::to_string(mtime_nsec), std::to_string(ctime_sec), std::to_string(ctime_nsec), std::to_string(columns.size())}) { put(out, s); }
  for (const auto &column : columns) { put(out, column.name); put(out, std::to_string(column.type)); put(out, std::to_string(column.nullable)); }
  return out;
}
int FileScanConfig::decode(const ObString &encoded)
{
  if (encoded.empty() || encoded.length() > FileScan::MAX_RECORD) { return OB_INVALID_DATA; }
  std::string in(encoded.ptr(), encoded.length()), value;
  size_t pos = 0;
  if (!take(in, pos, path) || !take(in, pos, format) || !take(in, pos, null_text)) { return OB_INVALID_DATA; }
  char *characters[] = {&delimiter, &quote, &escape};
  for (auto character : characters) {
    if (!take(in, pos, value) || value.size() != 1) { return OB_INVALID_DATA; }
    *character = value[0];
  }
  int64_t numbers[9];
  for (auto &number : numbers) {
    if (!take(in, pos, value) || value.empty()) { return OB_INVALID_DATA; }
    char *end = nullptr; errno = 0; number = std::strtoll(value.c_str(), &end, 10);
    if (errno || *end != '\0') { return OB_INVALID_DATA; }
  }
  header = numbers[0]; device = numbers[1]; inode = numbers[2]; size = numbers[3];
  mtime_sec = numbers[4]; mtime_nsec = numbers[5]; ctime_sec = numbers[6]; ctime_nsec = numbers[7];
  if (numbers[8] < 0 || numbers[8] > FileScan::MAX_COLUMNS) { return OB_INVALID_DATA; }
  columns.clear();
  for (int64_t i = 0; i < numbers[8]; ++i) {
    FileColumn column;
    if (!take(in, pos, column.name) || !take(in, pos, value)) { return OB_INVALID_DATA; }
    column.type = static_cast<ObObjType>(std::atoi(value.c_str()));
    if (!take(in, pos, value)) { return OB_INVALID_DATA; }
    column.nullable = value == "1"; columns.push_back(column);
  }
  return pos == in.size() ? OB_SUCCESS : OB_INVALID_DATA;
}

int FileScan::check_access(const std::string &path, const ObString &secure_dir, std::string &canonical)
{
#if defined(__linux__)
  if (path.empty() || path.size() >= PATH_MAX || path.find('\0') != std::string::npos
      || secure_dir.empty() || secure_dir.case_compare("NULL") == 0) { return OB_ERR_NO_PRIVILEGE; }
  const size_t wildcard = path.find_first_of("*?[");
  if (wildcard != std::string::npos) {
    const size_t slash = path.rfind('/', wildcard);
    const std::string directory = slash == std::string::npos ? "." : path.substr(0, slash + 1);
    if (path.find('/', wildcard) != std::string::npos) { return OB_INVALID_ARGUMENT; }
    std::string resolved;
    const int ret = check_access(directory, secure_dir, resolved);
    if (ret != OB_SUCCESS) { return ret; }
    canonical = resolved + (resolved.back() == '/' ? "" : "/") + path.substr(slash == std::string::npos ? 0 : slash + 1);
    return OB_SUCCESS;
  }
  const std::string root(secure_dir.ptr(), secure_dir.length());
  if (root.find('\0') != std::string::npos) { return OB_ERR_NO_PRIVILEGE; }
  char resolved[PATH_MAX], allowed[PATH_MAX];
  if (nullptr == realpath(root.c_str(), allowed)) { return OB_ERR_NO_PRIVILEGE; }
  struct stat root_stat;
  if (stat(allowed, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) { return OB_ERR_NO_PRIVILEGE; }
  if (nullptr == realpath(path.c_str(), resolved)) { return OB_FILE_NOT_EXIST; }
  const size_t length = std::strlen(allowed);
  if (std::strncmp(resolved, allowed, length) != 0
      || (length != 1 && resolved[length] != '/' && resolved[length] != '\0')) { return OB_ERR_NO_PRIVILEGE; }
  canonical = resolved; return OB_SUCCESS;
#else
  return OB_NOT_SUPPORTED;
#endif
}
ObObjType FileScan::infer_type(const std::string &value)
{
  if ((value.size() == 4 && strcasecmp(value.c_str(), "true") == 0)
      || (value.size() == 5 && strcasecmp(value.c_str(), "false") == 0)) { return ObTinyIntType; }
  if (date_valid(value, false)) { return ObDateType; }
  if (date_valid(value, true)) { return ObDateTimeType; }
  if (value.empty()) { return ObVarcharType; }
  size_t pos = (value[0] == '-' || value[0] == '+') ? 1 : 0;
  bool integer = pos < value.size();
  for (; pos < value.size(); ++pos) { integer &= value[pos] >= '0' && value[pos] <= '9'; }
  char *end = nullptr;
  errno = 0;
  if (integer) {
    std::strtoll(value.c_str(), &end, 10);
    return errno == 0 && end == value.c_str() + value.size() ? ObIntType : ObVarcharType;
  }
  // Only decimal SQL number syntax; reject whitespace, hex, infinity and NaN.
  pos = (value[0] == '-' || value[0] == '+') ? 1 : 0;
  bool digits = false;
  while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') { ++pos; digits = true; }
  if (pos < value.size() && value[pos] == '.') {
    ++pos;
    while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') { ++pos; digits = true; }
  }
  if (pos < value.size() && (value[pos] == 'e' || value[pos] == 'E')) {
    ++pos;
    if (pos < value.size() && (value[pos] == '+' || value[pos] == '-')) { ++pos; }
    const size_t start = pos;
    while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') { ++pos; }
    digits &= pos > start;
  }
  if (!digits || pos != value.size()) { return ObVarcharType; }
  const double number = std::strtod(value.c_str(), &end);
  return errno == 0 && std::isfinite(number) && end == value.c_str() + value.size() ? ObDoubleType : ObVarcharType;
}
ObObjType FileScan::widen(ObObjType left, ObObjType right)
{
  if (left == ObNullType) { return right; }
  if (right == ObNullType || left == right) { return left; }
  if ((left == ObIntType && right == ObDoubleType) || (left == ObDoubleType && right == ObIntType)) { return ObDoubleType; }
  if ((left == ObDateType && right == ObDateTimeType) || (left == ObDateTimeType && right == ObDateType)) { return ObDateTimeType; }
  return ObVarcharType;
}
int FileScan::fail(const char *message)
{
  error_ = config_.path + ": record near line " + std::to_string(line_) + ": " + message;
  FORWARD_USER_ERROR(OB_INVALID_ARGUMENT, error_.c_str());
  return OB_INVALID_ARGUMENT;
}
int FileScan::snapshot(FileScanConfig &config) const
{
#if defined(__linux__)
  struct stat st;
  if (nullptr == file_ || fstat(fileno(file_), &st) != 0 || !S_ISREG(st.st_mode)) { return OB_IO_ERROR; }
  config.device = st.st_dev; config.inode = st.st_ino; config.size = st.st_size;
  config.mtime_sec = st.st_mtim.tv_sec; config.mtime_nsec = st.st_mtim.tv_nsec;
  config.ctime_sec = st.st_ctim.tv_sec; config.ctime_nsec = st.st_ctim.tv_nsec;
  return OB_SUCCESS;
#else
  return OB_NOT_SUPPORTED;
#endif
}
int FileScan::check_changed() const
{
#if defined(__linux__)
  if (config_.format == "glob") { return OB_SUCCESS; }
  FileScanConfig current;
  int ret = snapshot(current);
  struct stat named;
  if (OB_SUCC(ret) && (stat(config_.path.c_str(), &named) != 0 || named.st_dev != config_.device
      || named.st_ino != config_.inode || current.device != config_.device || current.inode != config_.inode
      || current.size != config_.size || current.mtime_sec != config_.mtime_sec || current.mtime_nsec != config_.mtime_nsec
      || current.ctime_sec != config_.ctime_sec || current.ctime_nsec != config_.ctime_nsec)) { ret = OB_STATE_NOT_MATCH; }
  return ret;
#else
  return OB_NOT_SUPPORTED;
#endif
}
void FileScan::close()
{
  delete parquet_; parquet_ = nullptr;
  if (nullptr != file_) { std::fclose(file_); file_ = nullptr; }
}
int FileScan::open(const FileScanConfig &config)
{
#if defined(__linux__)
  close(); config_ = config; line_ = 1;
  if (config.format == "glob") {
    const size_t slash = config.path.rfind('/');
    const std::string directory = config.path.substr(0, slash + 1);
    const std::string pattern = config.path.substr(slash + 1);
    DIR *dir = opendir(directory.c_str());
    if (nullptr == dir) { return OB_FILE_NOT_EXIST; }
    matches_.clear(); match_pos_ = 0;
    int ret = OB_SUCCESS;
    struct dirent *entry = nullptr;
    errno = 0;
    while (OB_SUCC(ret) && nullptr != (entry = readdir(dir))) {
      if (fnmatch(pattern.c_str(), entry->d_name, FNM_PERIOD) != 0) { continue; }
      const std::string path = directory + entry->d_name;
      struct stat st;
      if (lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        char resolved[PATH_MAX];
        if (nullptr == realpath(path.c_str(), resolved) || std::string(resolved) != path) { ret = OB_ERR_NO_PRIVILEGE; }
        else if (matches_.size() >= 10000) { ret = OB_SIZE_OVERFLOW; }
        else { matches_.push_back(path); }
      }
      errno = 0;
    }
    if (OB_SUCC(ret) && errno != 0) { ret = OB_IO_ERROR; }
    closedir(dir);
    std::sort(matches_.begin(), matches_.end());
    return ret;
  }
  int descriptor = ::open(config.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) { return OB_FILE_NOT_EXIST; }
  // Check the opened object, so renaming a path component cannot redirect the
  // scan outside the path whose authorization was checked by the SQL layer.
  char fd_path[64], actual[PATH_MAX];
  std::snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", descriptor);
  const ssize_t length = readlink(fd_path, actual, sizeof(actual) - 1);
  if (length < 0 || std::string(actual, length) != config.path) { ::close(descriptor); return OB_ERR_NO_PRIVILEGE; }
  file_ = fdopen(descriptor, "rb");
  if (nullptr == file_) { ::close(descriptor); return OB_IO_ERROR; }
  int ret = config.inode == 0 ? snapshot(config_) : check_changed();
  if (OB_SUCC(ret) && config_.format == "parquet") {
    parquet_ = new (std::nothrow) ParquetScan();
    if (nullptr == parquet_) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else { ret = parquet_->open(file_, config_.size, config_.columns); }
  }
  if (OB_SUCC(ret) && (config_.format == "csv" || config_.format == "json")) { skip_bom(file_); }
  if (OB_FAIL(ret)) { close(); }
  return ret;
#else
  return OB_NOT_SUPPORTED;
#endif
}
void FileScan::set_projection(const std::vector<int64_t> &columns)
{
  if (nullptr != parquet_) { parquet_->set_projection(columns); }
}
int FileScan::rescan()
{
#if defined(__linux__)
  if (config_.format == "glob") { match_pos_ = 0; return OB_SUCCESS; }
  int ret = check_changed();
  if (OB_SUCC(ret) && nullptr != parquet_) {
    return parquet_->open(file_, config_.size, config_.columns);
  }
  if (OB_SUCC(ret) && fseeko(file_, 0, SEEK_SET) != 0) { ret = OB_IO_ERROR; }
  line_ = 1;
  if (OB_SUCC(ret) && (config_.format == "csv" || config_.format == "json")) { skip_bom(file_); }
  if (OB_SUCC(ret) && config_.format == "csv" && config_.header) {
    std::vector<FileCell> header; ret = read_csv(header);
  }
  return ret;
#else
  return OB_NOT_SUPPORTED;
#endif
}
int FileScan::read_csv(std::vector<FileCell> &row)
{
  row.clear();
  FileCell cell;
  bool quoted = false, in_quote = false, after_quote = false, any = false;
  size_t bytes = 0;
  while (true) {
    int ch = std::fgetc(file_);
    if (ch == EOF && ferror(file_)) { return OB_IO_ERROR; }
    if (ch == EOF && !any && row.empty() && cell.text.empty()) { return OB_ITER_END; }
    any |= ch != EOF;
    if (++bytes > MAX_RECORD) { return fail("record exceeds 16 MiB limit"); }
    if (ch == '\n') { ++line_; }
    if (in_quote) {
      if (ch == EOF) { return fail("unterminated quoted field"); }
      if (ch == config_.escape && config_.escape != config_.quote) {
        ch = std::fgetc(file_);
        if (ch == EOF) { return fail("unterminated escape"); }
        if (ch == '\n') { ++line_; }
        cell.text += static_cast<char>(ch);
      } else if (ch == config_.quote) {
        const int peek = config_.escape == config_.quote ? std::fgetc(file_) : EOF;
        if (peek == config_.quote) { cell.text += config_.quote; }
        else {
          if (peek != EOF) { std::ungetc(peek, file_); }
          in_quote = false; after_quote = true;
        }
      } else { cell.text += static_cast<char>(ch); }
    } else if (ch == config_.delimiter || ch == '\n' || ch == '\r' || ch == EOF) {
      int64_t valid_bytes = 0;
      if (ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN, cell.text.data(), cell.text.size(), valid_bytes) != OB_SUCCESS
          || valid_bytes != cell.text.size()) { return fail("invalid UTF-8 field"); }
      cell.is_null = !quoted && cell.text == config_.null_text;
      cell.type = cell.is_null ? ObNullType : infer_type(cell.text);
      row.push_back(cell);
      if (row.size() > MAX_COLUMNS) { return fail("too many columns"); }
      cell = FileCell(); quoted = false; after_quote = false;
      if (ch != config_.delimiter) {
        if (ch == '\r') {
          const int next = std::fgetc(file_);
          if (next != '\n' && next != EOF) { std::ungetc(next, file_); }
          ++line_;
        }
        return OB_SUCCESS;
      }
    } else if (after_quote) { return fail("unexpected character after closing quote"); }
    else if (ch == config_.quote) {
      if (!cell.text.empty()) { return fail("quote inside unquoted field"); }
      quoted = in_quote = true;
    } else { cell.text += static_cast<char>(ch); }
  }
}
int FileScan::read_json(std::vector<FileCell> &row, std::vector<std::string> &names)
{
  row.clear(); names.clear();
  std::string text;
  while (true) {
    int ch = std::fgetc(file_);
    if (ch == EOF && ferror(file_)) { return OB_IO_ERROR; }
    if (ch == EOF && text.empty()) { return OB_ITER_END; }
    if (ch == '\n' || ch == EOF) {
      ++line_;
      if (text.find_first_not_of(" \r\t") == std::string::npos) { text.clear(); if (ch == EOF) { return OB_ITER_END; } continue; }
      break;
    }
    text += static_cast<char>(ch);
    if (text.size() > MAX_RECORD) { return fail("record exceeds 16 MiB limit"); }
  }
  json_allocator_.reuse();
  ObIJsonBase *base = nullptr;
  int ret = ObJsonBaseFactory::get_json_base(&json_allocator_, ObString(text.size(), text.data()),
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base);
  if (OB_FAIL(ret) || nullptr == base || base->json_type() != ObJsonNodeType::J_OBJECT) { return fail("expected a flat JSON object"); }
  ObJsonObject *object = static_cast<ObJsonObject *>(base);
  if (object->element_count() > MAX_COLUMNS) { return fail("too many columns"); }
  for (int64_t i = 0; OB_SUCC(ret) && i < object->element_count(); ++i) {
    ObString name;
    ObJsonNode *value = object->get_value(i);
    if (OB_FAIL(object->get_key(i, name))) { break; }
    names.emplace_back(name.ptr(), name.length());
    FileCell cell;
    cell.is_null = value->json_type() == ObJsonNodeType::J_NULL;
    if (value->json_type() == ObJsonNodeType::J_ARRAY || value->json_type() == ObJsonNodeType::J_OBJECT) { return fail("nested JSON values are not supported"); }
    if (!cell.is_null) {
      if (value->json_type() == ObJsonNodeType::J_STRING) {
        cell.text.assign(value->get_data(), value->get_data_length());
        cell.type = infer_type(cell.text);
        if (cell.type != ObDateType && cell.type != ObDateTimeType) { cell.type = ObVarcharType; }
      } else {
        ObJsonBuffer buffer(&json_allocator_);
        if (OB_FAIL(value->print(buffer, false))) { break; }
        cell.text.assign(buffer.ptr(), buffer.length()); cell.type = infer_type(cell.text);
      }
    }
    row.push_back(cell);
  }
  return ret;
}
int FileScan::read_record(std::vector<FileCell> &row, std::vector<std::string> &names)
{
  return config_.format == "csv" ? read_csv(row) : read_json(row, names);
}
int FileScan::infer(FileScanConfig &config)
{
  if (config.format == "glob") { config.columns = {FileColumn{"file", ObVarcharType}}; return OB_SUCCESS; }
  if (config.format != "csv" && config.format != "json" && config.format != "parquet") { return OB_NOT_SUPPORTED; }
  FileScan scan;
  int ret = scan.open(config);
  if (OB_FAIL(ret)) { return ret; }
  config = scan.config_;
  if (config.format == "parquet") { return scan.check_changed(); }
  config.columns.clear();
  std::vector<FileCell> row;
  std::vector<std::string> names;
  bool first = true;
  while (OB_SUCC(ret) && OB_SUCC(ret = scan.read_record(row, names))) {
    if (config.format == "csv") {
      if (first) {
        for (size_t i = 0; i < row.size(); ++i) {
          FileColumn column;
          const std::string base = config.header && !row[i].text.empty() ? row[i].text : "column" + std::to_string(i);
          column.name = base;
          if (i == 0 && column.name.compare(0, 3, "\xef\xbb\xbf") == 0) { column.name.erase(0, 3); }
          int suffix = 2;
          while (std::any_of(config.columns.begin(), config.columns.end(), [&](const FileColumn &old) { return same_name(old.name, column.name); })) {
            column.name = base + "__" + std::to_string(suffix++);
          }
          if (column.name.size() > 256 || column.name.find('\0') != std::string::npos) { return scan.fail("column name exceeds 256 bytes"); }
          config.columns.push_back(column);
        }
        first = false;
        if (config.header) { continue; }
      }
      if (row.size() != config.columns.size()) { return scan.fail("CSV column count differs from header"); }
      for (size_t i = 0; i < row.size(); ++i) { config.columns[i].type = widen(config.columns[i].type, row[i].type); config.columns[i].nullable |= row[i].is_null; }
    } else {
      for (auto &column : config.columns) {
        if (std::none_of(names.begin(), names.end(), [&](const std::string &name) { return same_name(column.name, name); })) { column.nullable = true; }
      }
      for (size_t i = 0; i < row.size(); ++i) {
        auto found = std::find_if(config.columns.begin(), config.columns.end(), [&](const FileColumn &column) { return same_name(column.name, names[i]); });
        if (found == config.columns.end()) {
          if (config.columns.size() >= MAX_COLUMNS || names[i].empty() || names[i].size() > 256 || names[i].find('\0') != std::string::npos) { return scan.fail("invalid JSON column name or column limit"); }
          config.columns.push_back(FileColumn{names[i], row[i].type, !first || row[i].is_null});
        } else { found->type = widen(found->type, row[i].type); found->nullable |= row[i].is_null; }
      }
      first = false;
    }
  }
  if (ret == OB_ITER_END) { ret = scan.check_changed(); }
  if (OB_SUCC(ret) && config.columns.empty()) { ret = scan.fail("file has no columns"); }
  for (auto &column : config.columns) { if (column.type == ObNullType) { column.type = ObVarcharType; } }
  return ret;
}
int FileScan::next(std::vector<FileCell> &row)
{
  if (config_.format == "glob") {
    if (match_pos_ >= matches_.size()) { return OB_ITER_END; }
    row = {FileCell{matches_[match_pos_++], false, ObVarcharType}}; return OB_SUCCESS;
  }
  int ret = check_changed();
  std::vector<FileCell> values;
  std::vector<std::string> names;
  if (OB_SUCC(ret) && nullptr != parquet_) { return parquet_->next(row); }
  if (OB_SUCC(ret)) { ret = read_record(values, names); }
  if (OB_SUCC(ret)) {
    if (config_.format == "csv") {
      if (values.size() != config_.columns.size()) { return fail("CSV column count differs from schema"); }
      row.swap(values);
    } else {
      row.assign(config_.columns.size(), FileCell());
      for (size_t i = 0; i < names.size(); ++i) {
        auto found = std::find_if(config_.columns.begin(), config_.columns.end(), [&](const FileColumn &column) { return same_name(column.name, names[i]); });
        if (found == config_.columns.end()) { return OB_STATE_NOT_MATCH; }
        row[found - config_.columns.begin()] = values[i];
      }
    }
  }
  return ret;
}
} }
