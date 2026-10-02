TEST(Hello, StepSucceeds) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  EXPECT_EQ(0, seq_step_1(&ctx));
}

TEST(Hello, StepLeavesNoErrorMessage) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
  EXPECT_STREQ("", ctx.message);
}
