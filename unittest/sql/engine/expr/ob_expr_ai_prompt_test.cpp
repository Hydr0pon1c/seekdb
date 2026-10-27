/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
using namespace oceanbase::common;
TEST(AIPrompt, repeated_json_nested_unicode_and_bounds)
{
  ObArenaAllocator allocator;
  auto render = [&](const char *input, ObString &output) {
    ObJsonObject *object = nullptr; ObString text(input);
    int ret = ObAIFuncJsonUtils::get_json_object_form_str(allocator, text, object);
    return ret == OB_SUCCESS ? ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, object, output) : ret;
  };
  ObString result;
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{0}/{0}/{0}/{0}","args":["你好，世界"]})", result));
  EXPECT_EQ(ObString("你好，世界/你好，世界/你好，世界/你好，世界"), result);
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"before {0}","args":[{"template":"{0}+{0}","args":["value"]}]})", result));
  EXPECT_EQ(ObString("before value+value"), result);
  ASSERT_EQ(OB_SUCCESS, render(R"({"template":"{0}","args":[[1,true,null]]})", result));
  EXPECT_FALSE(result.empty());
  EXPECT_NE(OB_SUCCESS, render(R"({"template":"{1}","args":["x"]})", result));
  EXPECT_NE(OB_SUCCESS, render(R"({"template":"{184467440737095516160}","args":["x"]})", result));
  EXPECT_EQ(OB_SUCCESS, render(R"({"template":"{word} {} \\ \"","args":[]})", result));
}
