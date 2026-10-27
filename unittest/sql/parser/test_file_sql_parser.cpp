/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;
TEST(FileSQLParser, read_functions_aliases_options_and_native_joins)
{
  for (const char *query : {"select * from read_csv('/tmp/a.csv')",
      "select * from read_csv('/tmp/a.csv', header = true, sample_size = -1) as f",
      "select * from read_ndjson('/tmp/a.jsonl') j",
      "select * from glob('/tmp/*')", "copy (select 1 as id) to '/tmp/a.csv' (format csv, header, nullstr 'NULL')", "copy t to '/tmp/b.csv'", "describe select * from read_parquet('/tmp/a.parquet')",
      "select a.id, b.name from read_csv('/tmp/a.csv') a left join read_json('/tmp/b.jsonl') b on a.id=b.id"}) {
    ObArenaAllocator allocator; ObParser parser(allocator, 0); ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(query), result)) << query;
  }
}
