// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "arrow/csv/options.h"
#include "arrow/csv/reader.h"
#include "arrow/csv/test_common.h"
#include "arrow/io/interfaces.h"
#include "arrow/io/memory.h"
#include "arrow/status.h"
#include "arrow/table.h"
#include "arrow/testing/future_util.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/util/async_generator.h"
#include "arrow/util/future.h"
#include "arrow/util/thread_pool.h"

namespace arrow {
namespace csv {

// Allows the streaming reader to be used in tests that expect a table reader
class StreamingReaderAsTableReader : public TableReader {
 public:
  explicit StreamingReaderAsTableReader(std::shared_ptr<StreamingReader> reader)
      : reader_(std::move(reader)) {}
  virtual ~StreamingReaderAsTableReader() = default;
  virtual Result<std::shared_ptr<Table>> Read() {
    std::shared_ptr<Table> table;
    RETURN_NOT_OK(reader_->ReadAll(&table));
    return table;
  }
  virtual Future<std::shared_ptr<Table>> ReadAsync() {
    auto reader = reader_;
    AsyncGenerator<std::shared_ptr<RecordBatch>> gen = [reader] {
      return reader->ReadNextAsync();
    };
    return CollectAsyncGenerator(std::move(gen))
        .Then([](const RecordBatchVector& batches) {
          return Table::FromRecordBatches(batches);
        });
  }

 private:
  std::shared_ptr<StreamingReader> reader_;
};

using TableReaderFactory =
    std::function<Result<std::shared_ptr<TableReader>>(std::shared_ptr<io::InputStream>)>;
using StreamingReaderFactory = std::function<Result<std::shared_ptr<StreamingReader>>(
    std::shared_ptr<io::InputStream>)>;

void TestEmptyTable(TableReaderFactory reader_factory) {
  auto empty_buffer = std::make_shared<Buffer>("");
  auto empty_input = std::make_shared<io::BufferReader>(empty_buffer);
  auto maybe_reader = reader_factory(empty_input);
  // Streaming reader fails on open, table readers fail on first read
  if (maybe_reader.ok()) {
    ASSERT_FINISHES_AND_RAISES(Invalid, (*maybe_reader)->ReadAsync());
  } else {
    ASSERT_TRUE(maybe_reader.status().IsInvalid());
  }
}

void TestHeaderOnly(TableReaderFactory reader_factory) {
  auto header_only_buffer = std::make_shared<Buffer>("a,b,c\n");
  auto input = std::make_shared<io::BufferReader>(header_only_buffer);
  ASSERT_OK_AND_ASSIGN(auto reader, reader_factory(input));
  ASSERT_FINISHES_OK_AND_ASSIGN(auto table, reader->ReadAsync());
  ASSERT_EQ(table->schema()->num_fields(), 3);
  ASSERT_EQ(table->num_rows(), 0);
}

void TestHeaderOnlyStreaming(StreamingReaderFactory reader_factory) {
  auto header_only_buffer = std::make_shared<Buffer>("a,b,c\n");
  auto input = std::make_shared<io::BufferReader>(header_only_buffer);
  ASSERT_OK_AND_ASSIGN(auto reader, reader_factory(input));
  std::shared_ptr<RecordBatch> next_batch;
  ASSERT_OK(reader->ReadNext(&next_batch));
  ASSERT_EQ(next_batch, nullptr);
}

void StressTableReader(TableReaderFactory reader_factory) {
#ifdef ARROW_VALGRIND
  const int NTASKS = 10;
  const int NROWS = 100;
#else
  const int NTASKS = 100;
  const int NROWS = 1000;
#endif
  ASSERT_OK_AND_ASSIGN(auto table_buffer, MakeSampleCsvBuffer(NROWS));

  std::vector<Future<std::shared_ptr<Table>>> task_futures(NTASKS);
  for (int i = 0; i < NTASKS; i++) {
    auto input = std::make_shared<io::BufferReader>(table_buffer);
    ASSERT_OK_AND_ASSIGN(auto reader, reader_factory(input));
    task_futures[i] = reader->ReadAsync();
  }
  auto combined_future = All(task_futures);
  combined_future.Wait();

  ASSERT_OK_AND_ASSIGN(std::vector<Result<std::shared_ptr<Table>>> results,
                       combined_future.result());
  for (auto&& result : results) {
    ASSERT_OK_AND_ASSIGN(auto table, result);
    ASSERT_EQ(NROWS, table->num_rows());
  }
}

void StressInvalidTableReader(TableReaderFactory reader_factory) {
#ifdef ARROW_VALGRIND
  const int NTASKS = 10;
  const int NROWS = 100;
#else
  const int NTASKS = 100;
  const int NROWS = 1000;
#endif
  ASSERT_OK_AND_ASSIGN(auto table_buffer, MakeSampleCsvBuffer(NROWS, false));

  std::vector<Future<std::shared_ptr<Table>>> task_futures(NTASKS);
  for (int i = 0; i < NTASKS; i++) {
    auto input = std::make_shared<io::BufferReader>(table_buffer);
    ASSERT_OK_AND_ASSIGN(auto reader, reader_factory(input));
    task_futures[i] = reader->ReadAsync();
  }
  auto combined_future = All(task_futures);
  combined_future.Wait();

  ASSERT_OK_AND_ASSIGN(std::vector<Result<std::shared_ptr<Table>>> results,
                       combined_future.result());
  for (auto&& result : results) {
    ASSERT_RAISES(Invalid, result);
  }
}

void TestNestedParallelism(std::shared_ptr<internal::ThreadPool> thread_pool,
                           TableReaderFactory reader_factory) {
  const int NROWS = 1000;
  ASSERT_OK_AND_ASSIGN(auto table_buffer, MakeSampleCsvBuffer(NROWS));
  auto input = std::make_shared<io::BufferReader>(table_buffer);
  ASSERT_OK_AND_ASSIGN(auto reader, reader_factory(input));

  Future<std::shared_ptr<Table>> table_future;

  auto read_task = [&reader, &table_future]() mutable {
    table_future = reader->ReadAsync();
    return Status::OK();
  };
  ASSERT_OK_AND_ASSIGN(auto future, thread_pool->Submit(read_task));

  ASSERT_FINISHES_OK(future);
  ASSERT_FINISHES_OK_AND_ASSIGN(auto table, table_future);
  ASSERT_EQ(table->num_rows(), NROWS);
}  // namespace csv

TableReaderFactory MakeSerialFactory() {
  return [](std::shared_ptr<io::InputStream> input_stream) {
    auto read_options = ReadOptions::Defaults();
    read_options.block_size = 1 << 10;
    read_options.use_threads = false;
    return TableReader::Make(io::default_io_context(), input_stream, read_options,
                             ParseOptions::Defaults(), ConvertOptions::Defaults());
  };
}

TEST(SerialReaderTests, Empty) { TestEmptyTable(MakeSerialFactory()); }
TEST(SerialReaderTests, HeaderOnly) { TestHeaderOnly(MakeSerialFactory()); }
TEST(SerialReaderTests, Stress) { StressTableReader(MakeSerialFactory()); }
TEST(SerialReaderTests, StressInvalid) { StressInvalidTableReader(MakeSerialFactory()); }
TEST(SerialReaderTests, NestedParallelism) {
  ASSERT_OK_AND_ASSIGN(auto thread_pool, internal::ThreadPool::Make(1));
  TestNestedParallelism(thread_pool, MakeSerialFactory());
}

Result<TableReaderFactory> MakeAsyncFactory(
    std::shared_ptr<internal::ThreadPool> thread_pool = nullptr) {
  if (!thread_pool) {
    ARROW_ASSIGN_OR_RAISE(thread_pool, internal::ThreadPool::Make(1));
  }
  return [thread_pool](std::shared_ptr<io::InputStream> input_stream)
             -> Result<std::shared_ptr<TableReader>> {
    ReadOptions read_options = ReadOptions::Defaults();
    read_options.use_threads = true;
    read_options.block_size = 1 << 10;
    auto table_reader =
        TableReader::Make(io::IOContext(thread_pool.get()), input_stream, read_options,
                          ParseOptions::Defaults(), ConvertOptions::Defaults());
    return table_reader;
  };
}

TEST(AsyncReaderTests, Empty) {
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeAsyncFactory());
  TestEmptyTable(table_factory);
}
TEST(AsyncReaderTests, HeaderOnly) {
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeAsyncFactory());
  TestHeaderOnly(table_factory);
}
TEST(AsyncReaderTests, Stress) {
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeAsyncFactory());
  StressTableReader(table_factory);
}
TEST(AsyncReaderTests, StressInvalid) {
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeAsyncFactory());
  StressInvalidTableReader(table_factory);
}
TEST(AsyncReaderTests, NestedParallelism) {
  ASSERT_OK_AND_ASSIGN(auto thread_pool, internal::ThreadPool::Make(1));
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeAsyncFactory(thread_pool));
  TestNestedParallelism(thread_pool, table_factory);
}

Result<TableReaderFactory> MakeStreamingFactory() {
  return [](std::shared_ptr<io::InputStream> input_stream)
             -> Result<std::shared_ptr<TableReader>> {
    auto read_options = ReadOptions::Defaults();
    read_options.block_size = 1 << 10;
    read_options.use_threads = true;
    ARROW_ASSIGN_OR_RAISE(
        auto streaming_reader,
        StreamingReader::Make(io::default_io_context(), input_stream, read_options,
                              ParseOptions::Defaults(), ConvertOptions::Defaults()));
    return std::make_shared<StreamingReaderAsTableReader>(std::move(streaming_reader));
  };
}

Result<StreamingReaderFactory> MakeStreamingReaderFactory() {
  return [](std::shared_ptr<io::InputStream> input_stream)
             -> Result<std::shared_ptr<StreamingReader>> {
    auto read_options = ReadOptions::Defaults();
    read_options.block_size = 1 << 10;
    read_options.use_threads = true;
    return StreamingReader::Make(io::default_io_context(), input_stream, read_options,
                                 ParseOptions::Defaults(), ConvertOptions::Defaults());
  };
}

TEST(StreamingReaderTests, Empty) {
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeStreamingFactory());
  TestEmptyTable(table_factory);
}
TEST(StreamingReaderTests, HeaderOnly) {
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeStreamingReaderFactory());
  TestHeaderOnlyStreaming(table_factory);
}
TEST(StreamingReaderTests, Stress) {
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeStreamingFactory());
  StressTableReader(table_factory);
}
TEST(StreamingReaderTests, StressInvalid) {
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeStreamingFactory());
  StressInvalidTableReader(table_factory);
}
TEST(StreamingReaderTests, NestedParallelism) {
  ASSERT_OK_AND_ASSIGN(auto thread_pool, internal::ThreadPool::Make(1));
  ASSERT_OK_AND_ASSIGN(auto table_factory, MakeStreamingFactory());
  TestNestedParallelism(thread_pool, table_factory);
}

TEST(StreamingReaderTest, BytesRead) {
  ASSERT_OK_AND_ASSIGN(auto thread_pool, internal::ThreadPool::Make(1));
  auto table_buffer =
      std::make_shared<Buffer>("a,b,c\n123,456,789\n101,112,131\n415,161,718\n");

  // Basic read without any skips and small block size
  {
    auto input = std::make_shared<io::BufferReader>(table_buffer);

    auto read_options = ReadOptions::Defaults();
    read_options.block_size = 20;
    read_options.use_threads = false;
    ASSERT_OK_AND_ASSIGN(
        auto streaming_reader,
        StreamingReader::Make(io::default_io_context(), input, read_options,
                              ParseOptions::Defaults(), ConvertOptions::Defaults()));
    std::shared_ptr<RecordBatch> batch;
    int64_t bytes = 6;  // Size of header (counted during StreamingReader::Make)
    do {
      ASSERT_EQ(bytes, streaming_reader->bytes_read());
      ASSERT_OK(streaming_reader->ReadNext(&batch));
      bytes += 12;  // Add size of each row
    } while (bytes <= 42);
    ASSERT_EQ(42, streaming_reader->bytes_read());
    ASSERT_EQ(batch.get(), nullptr);
  }

  // Interaction of skip_rows and bytes_read()
  {
    auto input = std::make_shared<io::BufferReader>(table_buffer);

    auto read_options = ReadOptions::Defaults();
    read_options.skip_rows = 1;
    read_options.block_size = 32;
    ASSERT_OK_AND_ASSIGN(
        auto streaming_reader,
        StreamingReader::Make(io::default_io_context(), input, read_options,
                              ParseOptions::Defaults(), ConvertOptions::Defaults()));
    std::shared_ptr<RecordBatch> batch;
    // The header (6 bytes) and first skipped row (12 bytes) are counted during
    // StreamingReader::Make
    ASSERT_EQ(18, streaming_reader->bytes_read());
    ASSERT_OK(streaming_reader->ReadNext(&batch));
    ASSERT_NE(batch.get(), nullptr);
    ASSERT_EQ(30, streaming_reader->bytes_read());
    ASSERT_OK(streaming_reader->ReadNext(&batch));
    ASSERT_NE(batch.get(), nullptr);
    ASSERT_EQ(42, streaming_reader->bytes_read());
    ASSERT_OK(streaming_reader->ReadNext(&batch));
    ASSERT_EQ(batch.get(), nullptr);
  }

  // Interaction of skip_rows_after_names and bytes_read()
  {
    auto input = std::make_shared<io::BufferReader>(table_buffer);

    auto read_options = ReadOptions::Defaults();
    read_options.block_size = 32;
    read_options.skip_rows_after_names = 1;

    ASSERT_OK_AND_ASSIGN(
        auto streaming_reader,
        StreamingReader::Make(io::default_io_context(), input, read_options,
                              ParseOptions::Defaults(), ConvertOptions::Defaults()));
    std::shared_ptr<RecordBatch> batch;

    // The header is read as part of StreamingReader::Make
    ASSERT_EQ(6, streaming_reader->bytes_read());
    ASSERT_OK(streaming_reader->ReadNext(&batch));
    ASSERT_NE(batch.get(), nullptr);
    // Next the skipped batch (12 bytes) and 1 row (12 bytes)
    ASSERT_EQ(30, streaming_reader->bytes_read());
    ASSERT_OK(streaming_reader->ReadNext(&batch));
    ASSERT_NE(batch.get(), nullptr);
    ASSERT_EQ(42, streaming_reader->bytes_read());
    ASSERT_OK(streaming_reader->ReadNext(&batch));
    ASSERT_EQ(batch.get(), nullptr);
  }
}

TEST(CountRowsAsync, Basics) {
  constexpr int NROWS = 4096;
  ASSERT_OK_AND_ASSIGN(auto table_buffer, MakeSampleCsvBuffer(NROWS));
  {
    auto reader = std::make_shared<io::BufferReader>(table_buffer);
    auto read_options = ReadOptions::Defaults();
    auto parse_options = ParseOptions::Defaults();
    ASSERT_FINISHES_OK_AND_EQ(
        NROWS, CountRowsAsync(io::default_io_context(), reader,
                              internal::GetCpuThreadPool(), read_options, parse_options));
  }
  {
    auto reader = std::make_shared<io::BufferReader>(table_buffer);
    auto read_options = ReadOptions::Defaults();
    read_options.skip_rows = 20;
    auto parse_options = ParseOptions::Defaults();
    ASSERT_FINISHES_OK_AND_EQ(NROWS - 20, CountRowsAsync(io::default_io_context(), reader,
                                                         internal::GetCpuThreadPool(),
                                                         read_options, parse_options));
  }
  {
    auto reader = std::make_shared<io::BufferReader>(table_buffer);
    auto read_options = ReadOptions::Defaults();
    read_options.autogenerate_column_names = true;
    auto parse_options = ParseOptions::Defaults();
    ASSERT_FINISHES_OK_AND_EQ(NROWS + 1, CountRowsAsync(io::default_io_context(), reader,
                                                        internal::GetCpuThreadPool(),
                                                        read_options, parse_options));
  }
  {
    auto reader = std::make_shared<io::BufferReader>(table_buffer);
    auto read_options = ReadOptions::Defaults();
    read_options.block_size = 1024;
    auto parse_options = ParseOptions::Defaults();
    ASSERT_FINISHES_OK_AND_EQ(
        NROWS, CountRowsAsync(io::default_io_context(), reader,
                              internal::GetCpuThreadPool(), read_options, parse_options));
  }
}

TEST(CountRowsAsync, Errors) {
  ASSERT_OK_AND_ASSIGN(auto table_buffer, MakeSampleCsvBuffer(4096, /*valid=*/false));
  auto reader = std::make_shared<io::BufferReader>(table_buffer);
  auto read_options = ReadOptions::Defaults();
  auto parse_options = ParseOptions::Defaults();
  ASSERT_FINISHES_AND_RAISES(
      Invalid, CountRowsAsync(io::default_io_context(), reader,
                              internal::GetCpuThreadPool(), read_options, parse_options));
}

}  // namespace csv
}  // namespace arrow
