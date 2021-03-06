// Copyright (C) 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <memory>
#include <string_view>

#include <android-base/file.h>
#include <gtest/gtest.h>
#include <libsnapshot/cow_reader.h>
#include <libsnapshot/cow_writer.h>

namespace android {
namespace snapshot {

class CowTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cow_ = std::make_unique<TemporaryFile>();
        ASSERT_GE(cow_->fd, 0) << strerror(errno);
    }

    void TearDown() override { cow_ = nullptr; }

    std::unique_ptr<TemporaryFile> cow_;
};

// Sink that always appends to the end of a string.
class StringSink : public IByteSink {
  public:
    void* GetBuffer(size_t requested, size_t* actual) override {
        size_t old_size = stream_.size();
        stream_.resize(old_size + requested, '\0');
        *actual = requested;
        return stream_.data() + old_size;
    }
    bool ReturnData(void*, size_t) override { return true; }
    void Reset() { stream_.clear(); }

    std::string& stream() { return stream_; }

  private:
    std::string stream_;
};

TEST_F(CowTest, ReadWrite) {
    CowOptions options;
    CowWriter writer(options);

    ASSERT_TRUE(writer.Initialize(cow_->fd));

    std::string data = "This is some data, believe it";
    data.resize(options.block_size, '\0');

    ASSERT_TRUE(writer.AddCopy(10, 20));
    ASSERT_TRUE(writer.AddRawBlocks(50, data.data(), data.size()));
    ASSERT_TRUE(writer.AddZeroBlocks(51, 2));
    ASSERT_TRUE(writer.Finalize());

    ASSERT_EQ(lseek(cow_->fd, 0, SEEK_SET), 0);

    CowReader reader;
    CowHeader header;
    ASSERT_TRUE(reader.Parse(cow_->fd));
    ASSERT_TRUE(reader.GetHeader(&header));
    ASSERT_EQ(header.magic, kCowMagicNumber);
    ASSERT_EQ(header.major_version, kCowVersionMajor);
    ASSERT_EQ(header.minor_version, kCowVersionMinor);
    ASSERT_EQ(header.block_size, options.block_size);
    ASSERT_EQ(header.num_ops, 4);

    auto iter = reader.GetOpIter();
    ASSERT_NE(iter, nullptr);
    ASSERT_FALSE(iter->Done());
    auto op = &iter->Get();

    ASSERT_EQ(op->type, kCowCopyOp);
    ASSERT_EQ(op->compression, kCowCompressNone);
    ASSERT_EQ(op->data_length, 0);
    ASSERT_EQ(op->new_block, 10);
    ASSERT_EQ(op->source, 20);

    StringSink sink;

    iter->Next();
    ASSERT_FALSE(iter->Done());
    op = &iter->Get();

    ASSERT_EQ(op->type, kCowReplaceOp);
    ASSERT_EQ(op->compression, kCowCompressNone);
    ASSERT_EQ(op->data_length, 4096);
    ASSERT_EQ(op->new_block, 50);
    ASSERT_EQ(op->source, 104);
    ASSERT_TRUE(reader.ReadData(*op, &sink));
    ASSERT_EQ(sink.stream(), data);

    iter->Next();
    ASSERT_FALSE(iter->Done());
    op = &iter->Get();

    // Note: the zero operation gets split into two blocks.
    ASSERT_EQ(op->type, kCowZeroOp);
    ASSERT_EQ(op->compression, kCowCompressNone);
    ASSERT_EQ(op->data_length, 0);
    ASSERT_EQ(op->new_block, 51);
    ASSERT_EQ(op->source, 0);

    iter->Next();
    ASSERT_FALSE(iter->Done());
    op = &iter->Get();

    ASSERT_EQ(op->type, kCowZeroOp);
    ASSERT_EQ(op->compression, kCowCompressNone);
    ASSERT_EQ(op->data_length, 0);
    ASSERT_EQ(op->new_block, 52);
    ASSERT_EQ(op->source, 0);

    iter->Next();
    ASSERT_TRUE(iter->Done());
}

TEST_F(CowTest, CompressGz) {
    CowOptions options;
    options.compression = "gz";
    CowWriter writer(options);

    ASSERT_TRUE(writer.Initialize(cow_->fd));

    std::string data = "This is some data, believe it";
    data.resize(options.block_size, '\0');

    ASSERT_TRUE(writer.AddRawBlocks(50, data.data(), data.size()));
    ASSERT_TRUE(writer.Finalize());

    ASSERT_EQ(lseek(cow_->fd, 0, SEEK_SET), 0);

    CowReader reader;
    ASSERT_TRUE(reader.Parse(cow_->fd));

    auto iter = reader.GetOpIter();
    ASSERT_NE(iter, nullptr);
    ASSERT_FALSE(iter->Done());
    auto op = &iter->Get();

    StringSink sink;

    ASSERT_EQ(op->type, kCowReplaceOp);
    ASSERT_EQ(op->compression, kCowCompressGz);
    ASSERT_EQ(op->data_length, 56);  // compressed!
    ASSERT_EQ(op->new_block, 50);
    ASSERT_EQ(op->source, 104);
    ASSERT_TRUE(reader.ReadData(*op, &sink));
    ASSERT_EQ(sink.stream(), data);

    iter->Next();
    ASSERT_TRUE(iter->Done());
}

TEST_F(CowTest, CompressTwoBlocks) {
    CowOptions options;
    options.compression = "gz";
    CowWriter writer(options);

    ASSERT_TRUE(writer.Initialize(cow_->fd));

    std::string data = "This is some data, believe it";
    data.resize(options.block_size * 2, '\0');

    ASSERT_TRUE(writer.AddRawBlocks(50, data.data(), data.size()));
    ASSERT_TRUE(writer.Finalize());

    ASSERT_EQ(lseek(cow_->fd, 0, SEEK_SET), 0);

    CowReader reader;
    ASSERT_TRUE(reader.Parse(cow_->fd));

    auto iter = reader.GetOpIter();
    ASSERT_NE(iter, nullptr);
    ASSERT_FALSE(iter->Done());
    iter->Next();
    ASSERT_FALSE(iter->Done());

    StringSink sink;

    auto op = &iter->Get();
    ASSERT_EQ(op->type, kCowReplaceOp);
    ASSERT_EQ(op->compression, kCowCompressGz);
    ASSERT_EQ(op->new_block, 51);
    ASSERT_TRUE(reader.ReadData(*op, &sink));
}

// Only return 1-byte buffers, to stress test the partial read logic in
// CowReader.
class HorribleStringSink : public StringSink {
  public:
    void* GetBuffer(size_t, size_t* actual) override { return StringSink::GetBuffer(1, actual); }
};

TEST_F(CowTest, HorribleSink) {
    CowOptions options;
    options.compression = "gz";
    CowWriter writer(options);

    ASSERT_TRUE(writer.Initialize(cow_->fd));

    std::string data = "This is some data, believe it";
    data.resize(options.block_size, '\0');

    ASSERT_TRUE(writer.AddRawBlocks(50, data.data(), data.size()));
    ASSERT_TRUE(writer.Finalize());

    ASSERT_EQ(lseek(cow_->fd, 0, SEEK_SET), 0);

    CowReader reader;
    ASSERT_TRUE(reader.Parse(cow_->fd));

    auto iter = reader.GetOpIter();
    ASSERT_NE(iter, nullptr);
    ASSERT_FALSE(iter->Done());

    HorribleStringSink sink;
    auto op = &iter->Get();
    ASSERT_TRUE(reader.ReadData(*op, &sink));
    ASSERT_EQ(sink.stream(), data);
}

}  // namespace snapshot
}  // namespace android

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
