TEST(Files, StepsRunInOrder) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
}

// Crashes the whole test binary.
TEST(Files, Crashes) {
  volatile int *nowhere = nullptr;
  *nowhere = 1;
}
