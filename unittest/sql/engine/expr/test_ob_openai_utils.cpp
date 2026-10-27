/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
using namespace oceanbase::common;
namespace {
ObJsonObject *object(ObIAllocator &allocator, const char *text)
{
  ObJsonObject *result = nullptr;
  ObString input(text);
  EXPECT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, input, result));
  return result;
}
}
TEST(OpenAIEmbed, restores_indices_and_rejects_malformed_vectors)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator,
      object(allocator, R"({"data":[{"index":1,"embedding":[3,4]},{"index":0,"embedding":[1,2]}]})"), result));
  ObJsonArray *vectors = static_cast<ObJsonArray *>(result);
  EXPECT_EQ(1, static_cast<ObJsonArray *>(vectors->get_value(0))->get_value(0)->get_int());
  for (const char *invalid : {R"({"data":{}})", R"({"data":[]})",
       R"({"data":[{"index":0,"embedding":42}]})", R"({"data":[{"index":0,"embedding":[]}]})",
       R"({"data":[{"index":0,"embedding":[true]}]})", R"({"data":[{"index":-1,"embedding":[1]}]})",
       R"({"data":[{"index":0,"embedding":[1]},{"index":0,"embedding":[2]}]})",
       R"({"data":[{"index":0,"embedding":[1]},{"index":1,"embedding":[2,3]}]})"}) {
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, object(allocator, invalid), result)) << invalid;
  }
}
TEST(AIComplete, rejects_missing_null_and_empty_content)
{
  ObArenaAllocator allocator;
  ObOpenAIUtils::ObOpenAIComplete provider;
  ObIJsonBase *result = nullptr;
  for (const char *invalid : {R"({})", R"({"choices":[]})", R"({"choices":[{"message":{"content":null}}]})",
      R"({"choices":[{"message":{"content":""}}]})", R"({"choices":[{"message":{"content":12}}]})"}) {
    EXPECT_NE(OB_SUCCESS, provider.parse_output(allocator, object(allocator, invalid), result));
  }
  EXPECT_EQ(OB_SUCCESS, provider.parse_output(allocator, object(allocator,
      R"({"choices":[{"message":{"content":"hello"}}]})"), result));
}
TEST(Ollama, native_protocol_without_api_key)
{
  ObArenaAllocator allocator;
  ObAIFuncIComplete *resolved_complete = nullptr;
  ObAIFuncIEmbed *resolved_embed = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(allocator, ObString("oLlAmA"), resolved_complete));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(allocator, ObString("ollama"), resolved_embed));
  ObOllamaUtils::ObOllamaComplete provider;
  ObArray<ObString> headers;
  ObString empty, model("local-model"), system("instructions"), prompt("你好");
  ASSERT_EQ(OB_SUCCESS, provider.get_header(allocator, empty, headers));
  EXPECT_FALSE(headers.empty());
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, prompt, nullptr, body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ASSERT_NE(nullptr, body->get_value("system"));
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_SUCCESS, provider.parse_output(allocator, object(allocator, R"({"response":"ok"})"), result));
  ObOllamaUtils::ObOllamaEmbed embed;
  EXPECT_EQ(OB_SUCCESS, embed.parse_output(allocator, object(allocator, R"({"embeddings":[[1,2],[3,4]]})"), result));
  EXPECT_NE(OB_SUCCESS, embed.parse_output(allocator, object(allocator, R"({"embeddings":[[1],[2,3]]})"), result));
}
