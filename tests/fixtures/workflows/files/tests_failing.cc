TEST(Files, StepsRunInOrder) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
  ASSERT_EQ(0, seq_step_2(&ctx));
}

// A wrong expectation: the program is right and this test is not.
TEST(Files, TotalIsSixteen) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
  ASSERT_EQ(0, seq_step_2(&ctx));
  std::ifstream total("total.txt");
  long value = 0;
  total >> value;
  EXPECT_EQ(16, value);
}
