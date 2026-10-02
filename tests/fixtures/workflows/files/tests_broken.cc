TEST(Files, DoesNotCompile) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  EXPECT_EQ(0, seq_step_1(&ctx))
  EXPECT_EQ(0, function_that_was_never_declared(&ctx));
}
