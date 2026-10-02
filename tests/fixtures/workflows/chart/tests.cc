TEST(Chart, AllStepsSucceed) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
  ASSERT_EQ(0, seq_step_2(&ctx));
  ASSERT_EQ(0, seq_step_3(&ctx));
}

TEST(Chart, WritesFiveTransactions) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
  std::ifstream in("company.txt");
  ASSERT_TRUE(in.is_open());
  int lines = 0;
  std::string line;
  while (std::getline(in, line)) {
    ++lines;
  }
  EXPECT_EQ(5, lines);
}

TEST(Chart, ProducesPng) {
  seq_ctx ctx;
  seq_ctx_init(&ctx);
  ASSERT_EQ(0, seq_step_1(&ctx));
  ASSERT_EQ(0, seq_step_2(&ctx));
  ASSERT_EQ(0, seq_step_3(&ctx));
  std::ifstream in("top3.png", std::ios::binary);
  ASSERT_TRUE(in.is_open());
  char signature[4] = {0, 0, 0, 0};
  in.read(signature, 4);
  EXPECT_EQ('P', signature[1]);
  EXPECT_EQ('N', signature[2]);
  EXPECT_EQ('G', signature[3]);
}
