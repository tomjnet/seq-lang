// JSON, SHA-256, configuration, and small utilities.

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "support/config.hpp"
#include "support/json.hpp"
#include "support/sha256.hpp"
#include "support/util.hpp"
#include "test_harness.hpp"

using seq::Json;

SEQ_TEST(Json_RoundTrip) {
  const std::string text =
      "{\"name\":\"x\",\"count\":3,\"ratio\":0.5,\"on\":true,\"off\":false,"
      "\"none\":null,\"list\":[1,\"two\",[]],\"nested\":{\"a\":{}}}";
  Json value;
  std::string error;
  CHECK(Json::Parse(text, &value, &error));
  CHECK_EQ(value.Dump(-1), text);
  CHECK_EQ(value.GetString("name"), std::string("x"));
  CHECK_EQ(value.GetInt("count"), std::int64_t{3});
  CHECK(value.Find("count")->is_integer());
  CHECK(!value.Find("ratio")->is_integer());
  CHECK(value.GetBool("on"));
  CHECK(value.Find("none")->is_null());
  CHECK(value.Find("missing") == nullptr);
}

SEQ_TEST(Json_KeepsInsertionOrder) {
  Json value = Json::MakeObject();
  value.Set("zebra", 1).Set("apple", 2).Set("mango", 3);
  value.Set("zebra", 9);
  CHECK_EQ(value.Dump(-1),
           std::string("{\"zebra\":9,\"apple\":2,\"mango\":3}"));
}

SEQ_TEST(Json_StringEscapes) {
  Json value;
  std::string error;
  CHECK(Json::Parse("\"a\\n\\t\\\"\\\\\\/\\u00e9\\ud83d\\ude00\"", &value,
                    &error));
  CHECK_EQ(value.AsString(), std::string("a\n\t\"\\/\xC3\xA9\xF0\x9F\x98\x80"));
  CHECK_EQ(Json(std::string("line\nbreak \"q\" \x01")).Dump(-1),
           std::string("\"line\\nbreak \\\"q\\\" \\u0001\""));
}

SEQ_TEST(Json_RejectsMalformedInput) {
  Json value;
  std::string error;
  for (const char* text :
       {"", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "\"unterminated", "01", "1.",
        "tru", "{} extra", "\"\\x\"", "\"\\ud800\"", "\"a\nb\"", "{'a':1}",
        "nan"}) {
    error.clear();
    CHECK(!Json::Parse(text, &value, &error));
    CHECK(!error.empty());
  }
  // Nesting is bounded.
  CHECK(!Json::Parse(std::string(200, '[') + std::string(200, ']'), &value,
                     &error));
}

SEQ_TEST(Sha256_KnownVectors) {
  CHECK_EQ(seq::Sha256Hex(""),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca49599"
                       "1b7852b855"));
  CHECK_EQ(seq::Sha256Hex("abc"),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff"
                       "61f20015ad"));
  CHECK_EQ(
      seq::Sha256Hex(
          "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419d"
                  "b06c1"));
  // One million 'a', fed in uneven pieces.
  seq::Sha256 hash;
  const std::string chunk(977, 'a');
  std::size_t remaining = 1000000;
  while (remaining > 0) {
    const std::size_t take = std::min(remaining, chunk.size());
    hash.Update(chunk.data(), take);
    remaining -= take;
  }
  CHECK_EQ(hash.HexDigest(),
           std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39"
                       "ccc7112cd0"));
}

SEQ_TEST(Config_ParsesTomlSubset) {
  std::vector<std::pair<std::string, std::string>> entries;
  std::string error;
  CHECK(
      seq::ParseConfigToml("# comment\n"
                           "top = 1\n"
                           "[model]\n"
                           "adapter = \"fake\"   # trailing\n"
                           "seed = 42\n"
                           "\n"
                           "[limits]\n"
                           "note = \"a \\\"quoted\\\" # not a comment\"\n"
                           "flag = true\n",
                           &entries, &error));
  CHECK_EQ(entries.size(), std::size_t{5});
  CHECK_EQ(entries[0].first, std::string("top"));
  CHECK_EQ(entries[1].first, std::string("model.adapter"));
  CHECK_EQ(entries[1].second, std::string("fake"));
  CHECK_EQ(entries[2].second, std::string("42"));
  CHECK_EQ(entries[3].second, std::string("a \"quoted\" # not a comment"));
  CHECK_EQ(entries[4].second, std::string("true"));
}

SEQ_TEST(Config_RejectsMalformedToml) {
  std::vector<std::pair<std::string, std::string>> entries;
  std::string error;
  for (const char* text :
       {"[model\n", "key\n", "key = \n", "key = \"open\n", "key = 1.5\n",
        "key = bare words\n", "= 3\n", "key = \"a\" extra\n"}) {
    error.clear();
    CHECK(!seq::ParseConfigToml(text, &entries, &error));
    CHECK(error.find("line 1") != std::string::npos);
  }
}

SEQ_TEST(Config_DefaultsAndOverrides) {
  const seq::Config defaults = seq::Config::Defaults();
  CHECK_EQ(defaults.Get("model.adapter"), std::string("llama-server"));
  CHECK_EQ(defaults.GetInt("build.repair_attempts"), std::int64_t{2});
  CHECK_EQ(defaults.GetInt("model.context_tokens"), std::int64_t{32768});

  seq::Config config;
  std::string error;
  CHECK(!seq::Config::Load({"no.such.key=1"}, &config, &error));
  CHECK(error.find("unknown setting") != std::string::npos);
  CHECK(!seq::Config::Load({"missing-equals"}, &config, &error));
  CHECK(!seq::Config::Load({"limits.cpu_seconds=soon"}, &config, &error));
  CHECK(error.find("nonnegative integer") != std::string::npos);
}

#if !defined(_WIN32)
SEQ_TEST(Config_PrecedenceIsFlagThenEnvironmentThenDefault) {
  // Point the configuration file somewhere empty so only env and flags count.
  setenv("XDG_CONFIG_HOME", "/nonexistent-seqc-config", 1);
  unsetenv("SEQC_LIMITS_CPU_SECONDS");

  seq::Config config;
  std::string error;
  CHECK(seq::Config::Load({}, &config, &error));
  CHECK_EQ(config.GetInt("limits.cpu_seconds"), std::int64_t{30});

  setenv("SEQC_LIMITS_CPU_SECONDS", "7", 1);
  CHECK(seq::Config::Load({}, &config, &error));
  CHECK_EQ(config.GetInt("limits.cpu_seconds"), std::int64_t{7});

  CHECK(seq::Config::Load({"limits.cpu_seconds=3"}, &config, &error));
  CHECK_EQ(config.GetInt("limits.cpu_seconds"), std::int64_t{3});
  unsetenv("SEQC_LIMITS_CPU_SECONDS");
}
#endif

SEQ_TEST(Util_Utf8) {
  CHECK(seq::IsValidUtf8("plain"));
  CHECK(seq::IsValidUtf8("\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80"));
  CHECK(!seq::IsValidUtf8("\xC3"));
  CHECK(!seq::IsValidUtf8("\xC0\xAF"));          // overlong
  CHECK(!seq::IsValidUtf8("\xED\xA0\x80"));      // surrogate
  CHECK(!seq::IsValidUtf8("\xF4\x90\x80\x80"));  // above U+10FFFF
  CHECK(!seq::IsValidUtf8("\x80"));
  CHECK_EQ(seq::Utf8Length("\xC3\xA9"
                           "a\xE6\x97\xA5"),
           std::size_t{3});
}

SEQ_TEST(Util_SafeRelativePath) {
  for (const char* path :
       {"a", "a/b.txt", "deep/er/file", "with space.txt", "..hidden", "a..b"}) {
    CHECK(seq::IsSafeRelativePath(path));
  }
  for (const char* path : {"", "/abs", "..", "a/../b", "./a", "a/./b", "a//b",
                           "a/", "a\\b", "a\nb", "../up"}) {
    CHECK(!seq::IsSafeRelativePath(path));
  }
  CHECK(!seq::IsSafeRelativePath(std::string(300, 'a')));
}

SEQ_TEST(Util_FormatBytes) {
  CHECK_EQ(seq::FormatBytes(0), std::string("0 bytes"));
  CHECK_EQ(seq::FormatBytes(1023), std::string("1023 bytes"));
  CHECK_EQ(seq::FormatBytes(1536), std::string("1.5 KiB"));
  CHECK_EQ(seq::FormatBytes(5 * 1024 * 1024), std::string("5.0 MiB"));
}
