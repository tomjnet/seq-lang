TEST(Files, StepsRunInOrder) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
  ASSERT_EQ(0, seq_step_2(&ctx));
}

TEST(Files, TotalIsFifteen) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
  ASSERT_EQ(0, seq_step_2(&ctx));
  std::ifstream total("total.txt");
  ASSERT_TRUE(total.is_open());
  long value = 0;
  total >> value;
  EXPECT_EQ(15, value);
}
