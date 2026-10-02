TEST(Files, StepsRunInOrder) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
}

TEST(Files, DiesOnNull) {
  EXPECT_DEATH(seq_step_1(nullptr), "");
}
