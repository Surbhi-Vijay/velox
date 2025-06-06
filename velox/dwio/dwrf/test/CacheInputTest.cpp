/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/Random.h>
#include <folly/container/F14Map.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/caching/FileIds.h"
#include "velox/common/caching/tests/CacheTestUtil.h"
#include "velox/common/file/FileSystems.h"
#include "velox/common/io/IoStatistics.h"
#include "velox/common/io/Options.h"
#include "velox/common/memory/MmapAllocator.h"
#include "velox/dwio/common/CachedBufferedInput.h"
#include "velox/dwio/dwrf/common/Common.h"
#include "velox/dwio/dwrf/test/TestReadFile.h"
#include "velox/exec/tests/utils/TempDirectoryPath.h"

#include <fcntl.h>
#include <gtest/gtest.h>

using namespace facebook::velox;
using namespace facebook::velox::dwio;
using namespace facebook::velox::dwio::common;
using namespace facebook::velox::cache;
using facebook::velox::common::Region;

using memory::MemoryAllocator;
using IoStatisticsPtr = std::shared_ptr<IoStatistics>;
DECLARE_bool(velox_ssd_odirect);

class CacheTest : public ::testing::Test {
 protected:
  static constexpr int32_t kMaxStreams = 50;

  // Describes a piece of file potentially read by this test.
  struct StripeData {
    TestReadFile* file;
    std::unique_ptr<CachedBufferedInput> input;
    std::vector<std::unique_ptr<SeekableInputStream>> streams;
    std::vector<Region> regions;
    bool prefetched;
  };

  static void SetUpTestCase() {
    memory::MemoryManager::testingSetInstance(memory::MemoryManager::Options{});
  }

  void SetUp() override {
    // executor_ = std::make_unique<folly::IOThreadPoolExecutor>(10, 10);
    rng_.seed(1);
    ioStats_ = std::make_shared<IoStatistics>();
    fsStats_ = std::make_shared<filesystems::File::IoStats>();
    filesystems::registerLocalFileSystem();
  }

  void TearDown() override {
    shutdownCache();
  }

  void shutdownCache() {
    if (cache_ != nullptr) {
      cache_->shutdown();
    }
    if (executor_ != nullptr) {
      executor_.reset();
    }
    if (cache_ != nullptr) {
      auto* ssdCache = cache_->ssdCache();
      if (ssdCache != nullptr) {
        ssdCacheHelper_->deleteFiles();
      }
    }
  }

  void initializeCache(
      uint64_t maxBytes,
      uint64_t ssdBytes = 0,
      bool checksumEnabled = false) {
    shutdownCache();

    if (executor_ == nullptr) {
      executor_ = std::make_unique<folly::IOThreadPoolExecutor>(10, 10);
    }

    std::unique_ptr<SsdCache> ssd;
    if (ssdBytes > 0) {
      FLAGS_velox_ssd_odirect = false;
      tempDirectory_ = exec::test::TempDirectoryPath::create();
      const SsdCache::Config config(
          fmt::format("{}/cache", tempDirectory_->getPath()),
          ssdBytes,
          1,
          executor_.get(),
          0,
          false,
          checksumEnabled,
          checksumEnabled);
      ssd = std::make_unique<SsdCache>(config);
      ssdCacheHelper_ = std::make_unique<test::SsdCacheTestHelper>(ssd.get());
      groupStats_ = &ssd->groupStats();
    }
    memory::MmapAllocator::Options options;
    options.capacity = maxBytes;
    allocator_ = std::make_shared<memory::MmapAllocator>(options);
    cache_ = AsyncDataCache::create(allocator_.get(), std::move(ssd));
    asyncDataCacheHelper_ =
        std::make_unique<test::AsyncDataCacheTestHelper>(cache_.get());
    cache_->setVerifyHook(checkEntry);
    for (auto i = 0; i < kMaxStreams; ++i) {
      streamIds_.push_back(std::make_unique<dwrf::DwrfStreamIdentifier>(
          i, i, 0, dwrf::StreamKind_DATA));
    }
    streamStarts_.resize(kMaxStreams + 1);
    streamStarts_[0] = 0;
    int32_t spacing = 100;
    for (auto i = 1; i <= kMaxStreams; ++i) {
      streamStarts_[i] = streamStarts_[i - 1] + spacing * i;
      if (i < kMaxStreams / 3) {
        spacing += 1'000;
      } else if (i < kMaxStreams / 3 * 2) {
        spacing += 20'000;
      } else if (i > kMaxStreams - 5) {
        spacing += 2'000'000;
      }
    }
  }

  // Corrupts the file by invalidate its content.
  static void corruptSsdFile(const std::string& path) {
    const auto fd = ::open(path.c_str(), O_WRONLY);
    const auto size = ::lseek(fd, 0, SEEK_END);
    ASSERT_EQ(ftruncate(fd, 0), 0);
    ASSERT_EQ(ftruncate(fd, size), 0);
  }

  static void checkEntry(const cache::AsyncDataCacheEntry& entry) {
    uint64_t seed = entry.key().fileNum.id();
    if (entry.tinyData()) {
      checkData(entry.tinyData(), entry.offset(), entry.size(), seed);
    } else {
      int64_t bytesLeft = entry.size();
      auto runOffset = entry.offset();
      for (auto i = 0; i < entry.data().numRuns(); ++i) {
        auto run = entry.data().runAt(i);
        checkData(
            run.data<char>(),
            runOffset,
            std::min<int64_t>(run.numBytes(), bytesLeft),
            seed);
        bytesLeft -= run.numBytes();
        runOffset += run.numBytes();
        if (bytesLeft <= 0) {
          break;
        }
      }
    }
  }

  static void
  checkData(const char* data, uint64_t offset, int32_t size, uint64_t seed) {
    uint8_t expected = seed + offset;
    for (auto i = 0; i < size; ++i) {
      auto cached = reinterpret_cast<const uint8_t*>(data)[i];
      if (cached != expected) {
        ASSERT_EQ(expected, cached) << " at " << (offset + i);
      }
      ++expected;
    }
  }

  uint64_t seedByPath(const std::string& path) {
    StringIdLease lease(fileIds(), path);
    return lease.id();
  }

  std::shared_ptr<TestReadFile> inputByPath(
      const std::string& path,
      StringIdLease& fileId,
      StringIdLease& groupId) {
    std::lock_guard<std::mutex> l(mutex_);
    fileId = StringIdLease{fileIds(), path};
    groupId = StringIdLease{fileIds(), fmt::format("group{}", fileId.id() / 2)};
    auto it = pathToInput_.find(fileId.id());
    if (it != pathToInput_.end()) {
      return it->second;
    }
    fileIds_.push_back(fileId);
    fileIds_.push_back(groupId);
    // Creates an extremely large read file for test.
    auto stream = std::make_shared<TestReadFile>(
        fileId.id(), 1UL << 63, std::make_shared<filesystems::File::IoStats>());
    pathToInput_[fileId.id()] = stream;
    return stream;
  }

  // Makes a CachedBufferedInput with a subset of the testing streams enqueued.
  // 'numColumns' streams are evenly selected from kMaxStreams.
  std::unique_ptr<StripeData> makeStripeData(
      std::shared_ptr<TestReadFile> readFile,
      int32_t numColumns,
      std::shared_ptr<ScanTracker> tracker,
      const StringIdLease& fileId,
      const StringIdLease& groupId,
      int64_t offset,
      bool noCacheRetention,
      const IoStatisticsPtr& ioStats,
      const std::shared_ptr<filesystems::File::IoStats>& fsStats) {
    auto data = std::make_unique<StripeData>();
    auto readOptions = io::ReaderOptions(pool_.get());
    readOptions.setNoCacheRetention(noCacheRetention);
    data->input = std::make_unique<CachedBufferedInput>(
        readFile,
        MetricsLog::voidLog(),
        fileId,
        cache_.get(),
        tracker,
        groupId,
        ioStats,
        fsStats,
        executor_.get(),
        readOptions);
    data->file = readFile.get();
    for (auto i = 0; i < numColumns; ++i) {
      const int32_t streamIndex = i * (kMaxStreams / numColumns);

      // Each region covers half the space from its start to the start of the
      // next or at max a little under 20MB.
      const Region region{
          offset + streamStarts_[streamIndex],
          std::min<uint64_t>(
              (1 << 20) - 11,
              (streamStarts_[streamIndex + 1] - streamStarts_[streamIndex]) /
                  2)};
      auto stream = data->input->enqueue(region, streamIds_[streamIndex].get());
      if (cache_->ssdCache() != nullptr) {
        const auto name =
            static_cast<const CacheInputStream&>(*stream).getName();
        EXPECT_TRUE(
            name.find("ssdFile=" + cache_->ssdCache()->filePrefix()) !=
            name.npos)
            << name;
      }
      data->streams.push_back(std::move(stream));
      data->regions.push_back(region);
    }
    return data;
  }

  bool shouldRead(
      const StripeData& stripe,
      int32_t columnIndex,
      int32_t readPct,
      int32_t modulo) {
    uint32_t random;
    if (deterministic_) {
      auto region = stripe.regions[columnIndex];
      random = folly::hasher<uint64_t>()(region.offset + columnIndex);
    } else {
      std::lock_guard<std::mutex> l(mutex_);
      random = folly::Random::rand32(rng_);
    }
    return random % 100 < readPct / ((columnIndex % modulo) + 1);
  }

  void readStream(const StripeData& stripe, int32_t columnIndex) {
    const void* data;
    int32_t size;
    int64_t numRead = 0;
    auto& stream = *stripe.streams[columnIndex];
    const auto region = stripe.regions[columnIndex];
    do {
      stream.Next(&data, &size);
      stripe.file->checkData(data, region.offset + numRead, size);
      numRead += size;
    } while (size > 0);
    ASSERT_EQ(numRead, region.length);
    if (testRandomSeek_) {
      // Test random access
      std::vector<uint64_t> offsets = {
          0, region.length / 3, region.length * 2 / 3};
      PositionProvider positions(offsets);
      for (auto i = 0; i < offsets.size(); ++i) {
        stream.seekToPosition(positions);
        checkRandomRead(stripe, stream, offsets, i, region);
      }
    }
  }

  void checkRandomRead(
      const StripeData& stripe,
      SeekableInputStream& stream,
      const std::vector<uint64_t>& offsets,
      int32_t i,
      const Region& region) {
    const void* data;
    int32_t size;
    int64_t numRead = 0;
    auto offset = offsets[i];
    // Reads from offset to half-way to the next offset or end.
    const auto toRead =
        ((i == offsets.size() - 1 ? region.length : offsets[i + 1]) - offset) /
        2;
    do {
      stream.Next(&data, &size);
      stripe.file->checkData(data, region.offset + offset, size);
      numRead += size;
      offset += size;
      if (size == 0 && numRead == 0) {
        VELOX_FAIL("Stream end prematurely after random seek");
      }
    } while (numRead < toRead);
  }

  // Makes a series of kReadAhead CachedBufferedInputs for consecutive stripes
  // and starts background load guided by the load frequency in the previous
  // stripes for 'stripeWindow' ahead of the stripe being read. When at end,
  // destroys the CachedBufferedInput for the pre-read stripes while they are in
  // a background loading state. A window size of 1 means that only one
  // CachedbufferedInput actives at a time.
  //
  // 'readPct' is the probability any given stripe will access any given column.
  // 'readPctModulo' biases the read probability of as a function of the column
  // number. If this is 1, all columns will be read at 'readPct'. If this is 4,
  // 'readPct is divided by 1 + columnId % readPctModulo, so that multiples of 4
  // get read at readPct and columns with id % 4 == 3 get read at 1/4 of
  // readPct.
  void readLoop(
      const std::string& filename,
      int numColumns,
      int32_t readPct,
      int32_t readPctModulo,
      int32_t numStripes,
      int32_t stripeWindow,
      bool noCacheRetention,
      const IoStatisticsPtr& ioStats,
      const std::shared_ptr<filesystems::File::IoStats>& fsStats) {
    auto tracker = std::make_shared<ScanTracker>(
        "testTracker",
        nullptr,
        io::ReaderOptions::kDefaultLoadQuantum,
        groupStats_);
    std::vector<std::unique_ptr<StripeData>> stripes;
    StringIdLease fileId;
    StringIdLease groupId;
    auto readFile = inputByPath(filename, fileId, groupId);
    if (groupStats_) {
      groupStats_->recordFile(fileId.id(), groupId.id(), numStripes);
    }
    for (auto stripeIndex = 0; stripeIndex < numStripes; ++stripeIndex) {
      const auto firstPrefetchStripe = stripeIndex + stripes.size();
      const auto window = std::min(stripeIndex + 1, stripeWindow);
      const auto lastPrefetchStripe =
          std::min(numStripes, stripeIndex + window);
      for (auto prefetchStripeIndex = firstPrefetchStripe;
           prefetchStripeIndex < lastPrefetchStripe;
           ++prefetchStripeIndex) {
        stripes.push_back(makeStripeData(
            readFile,
            numColumns,
            tracker,
            fileId,
            groupId,
            prefetchStripeIndex * streamStarts_[kMaxStreams - 1],
            noCacheRetention,
            ioStats,
            fsStats));
        if (stripes.back()->input->shouldPreload()) {
          stripes.back()->input->load(LogType::TEST);
          stripes.back()->prefetched = true;
        } else {
          stripes.back()->prefetched = false;
        }
      }
      auto currentStripe = std::move(stripes.front());
      stripes.erase(stripes.begin());
      if (!currentStripe->prefetched) {
        currentStripe->input->load(LogType::TEST);
      }
      for (auto columnIndex = 0; columnIndex < numColumns; ++columnIndex) {
        if (shouldRead(*currentStripe, columnIndex, readPct, readPctModulo)) {
          readStream(*currentStripe, columnIndex);
        }
      }
    }
  }

  // Reads a files from prefix<from> to prefix<to>. The other parameters have
  // the same meaning as with readLoop().
  void readFiles(
      const std::string& prefix,
      int32_t from,
      int32_t to,
      int numColumns,
      int32_t readPct,
      int32_t readPctModulo,
      int32_t numStripes,
      int32_t stripeWindow = 8) {
    for (auto i = from; i < to; ++i) {
      readLoop(
          fmt::format("{}{}", prefix, i),
          numColumns,
          readPct,
          readPctModulo,
          numStripes,
          stripeWindow,
          /*noCacheRetention=*/false,
          ioStats_,
          fsStats_);
    }
  }

  void waitForWrite() {
    auto ssd = cache_->ssdCache();
    if (ssd) {
      while (ssd->writeInProgress()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // NOLINT
      }
    }
  }

  // Serializes 'pathToInput_' and 'fileIds_' in multithread test.
  std::mutex mutex_;
  std::vector<StringIdLease> fileIds_;
  folly::F14FastMap<uint64_t, std::shared_ptr<TestReadFile>> pathToInput_;
  std::shared_ptr<exec::test::TempDirectoryPath> tempDirectory_;
  cache::FileGroupStats* groupStats_ = nullptr;
  std::shared_ptr<memory::MemoryAllocator> allocator_;
  std::shared_ptr<AsyncDataCache> cache_;
  std::unique_ptr<test::AsyncDataCacheTestHelper> asyncDataCacheHelper_;
  std::unique_ptr<test::SsdCacheTestHelper> ssdCacheHelper_;
  std::shared_ptr<IoStatistics> ioStats_;
  std::shared_ptr<filesystems::File::IoStats> fsStats_;
  std::unique_ptr<folly::IOThreadPoolExecutor> executor_;
  std::shared_ptr<memory::MemoryPool> pool_{
      memory::memoryManager()->addLeafPool()};

  // Id of simulated streams. Corresponds 1:1 to 'streamStarts_'.
  std::vector<std::unique_ptr<dwrf::DwrfStreamIdentifier>> streamIds_;

  // Start offset of each simulated stream in a simulated stripe.
  std::vector<uint64_t> streamStarts_;
  // Set to true if whether something is read should be deterministic by the
  // column and position.
  bool deterministic_{false};
  folly::Random::DefaultGenerator rng_;

  // Specifies if random seek follows bulk read in tests. We turn this off so as
  // not to inflate cache hits.
  bool testRandomSeek_{true};
};

TEST_F(CacheTest, window) {
  constexpr int32_t kMB = 1 << 20;
  initializeCache(64 * kMB);
  auto tracker = std::make_shared<ScanTracker>(
      "testTracker",
      nullptr,
      io::ReaderOptions::kDefaultLoadQuantum,
      groupStats_);
  StringIdLease fileId;
  StringIdLease groupId;
  auto file = inputByPath("test_for_window", fileId, groupId);
  auto input = std::make_unique<CachedBufferedInput>(
      file,
      MetricsLog::voidLog(),
      fileId,
      cache_.get(),
      tracker,
      groupId,
      ioStats_,
      fsStats_,
      executor_.get(),
      io::ReaderOptions(pool_.get()));
  auto begin = 4 * kMB;
  auto end = 17 * kMB;
  auto stream = input->read(begin, end - begin, LogType::TEST);
  auto cacheInput = dynamic_cast<CacheInputStream*>(stream.get());
  EXPECT_TRUE(cacheInput != nullptr);
  ASSERT_EQ(cacheInput->getName(), "CacheInputStream 0 of 13631488");
  const void* buffer;
  int32_t size;
  int32_t numRead = 0;
  while (numRead < end - begin) {
    EXPECT_TRUE(stream->Next(&buffer, &size));
    numRead += size;
  }
  EXPECT_FALSE(stream->Next(&buffer, &size));

  // We seek to 0.5 MB below the boundary of the 8MB load quantum and make a
  // clone to read a range on either side of the load quantum boundary.
  std::vector<uint64_t> positions = {7 * kMB + kMB / 2};
  auto provider = PositionProvider(positions);
  // We seek the first stream to 7.5MB inside its range.
  cacheInput->seekToPosition(provider);
  // We make a second stream that ranges over a subset of the range of the first
  // one.
  auto clone = cacheInput->clone();
  clone->SkipInt64(100);
  clone->setRemainingBytes(kMB);
  auto previousRead = ioStats_->rawBytesRead();
  EXPECT_TRUE(clone->Next(&buffer, &size));
  // Half MB minus the 100 bytes skipped above should be left in the first load
  // quantum of 8MB.
  EXPECT_EQ(kMB / 2 - 100, size);
  EXPECT_TRUE(clone->Next(&buffer, &size));
  EXPECT_EQ(kMB / 2 + 100, size);
  // There should be no more data in the window.
  EXPECT_FALSE(clone->Next(&buffer, &size));
  EXPECT_EQ(kMB, ioStats_->rawBytesRead() - previousRead);
}

TEST_F(CacheTest, bufferedInput) {
  // Size 160 MB. Frequent evictions and not everything fits in prefetch window.
  initializeCache(160 << 20);
  readLoop(
      "testfile",
      30,
      70,
      10,
      20,
      4,
      /*noCacheRetention=*/false,
      ioStats_,
      fsStats_);
  readLoop(
      "testfile",
      30,
      70,
      10,
      20,
      4,
      /*noCacheRetention=*/false,
      ioStats_,
      fsStats_);
  readLoop(
      "testfile2",
      30,
      70,
      70,
      20,
      4,
      /*noCacheRetention=*/false,
      ioStats_,
      fsStats_);
}

// Calibrates the data read for a densely and sparsely read stripe of test data.
// Fills the SSD cache with test data. Reads 2x cache size worth of data and
// checks that the cache population settles to a stable state.  Shifts the
// reading pattern so that half the working set drops out and another half is
// added. Checks that the working set stabilizes again.
TEST_F(CacheTest, ssd) {
  constexpr int64_t kSsdBytes = 256 << 20;
  // 64 RAM, 256MB SSD
  initializeCache(64 << 20, kSsdBytes);
  testRandomSeek_ = false;
  deterministic_ = true;

  // We read one stripe with all columns.
  readLoop(
      "testfile",
      30,
      100,
      1,
      1,
      1,
      /*noCacheRetention=*/false,
      ioStats_,
      fsStats_);
  // This is a cold read, so expect no hits.
  EXPECT_EQ(0, ioStats_->ramHit().sum());
  // Expect some extra reading from coalescing.
  EXPECT_LT(0, ioStats_->rawOverreadBytes());
  auto fullStripeBytes = ioStats_->rawBytesRead();
  auto bytes = ioStats_->rawBytesRead();
  cache_->clear();
  // We read 10 stripes with some columns sparsely accessed.
  readLoop(
      "testfile",
      30,
      70,
      10,
      10,
      1,
      /*noCacheRetention=*/false,
      ioStats_,
      fsStats_);
  auto sparseStripeBytes = (ioStats_->rawBytesRead() - bytes) / 10;
  EXPECT_LT(sparseStripeBytes, fullStripeBytes / 4);
  // Expect the dense fraction of columns to have read ahead.
  EXPECT_LT(400'000, ioStats_->prefetch().sum());

  constexpr int32_t kStripesPerFile = 10;
  auto bytesPerFile = fullStripeBytes * kStripesPerFile;
  // Read kSsdBytes worth of files to prime SSD cache.
  readFiles(
      "prefix1_", 0, kSsdBytes / bytesPerFile, 30, 100, 1, kStripesPerFile, 4);

  waitForWrite();
  cache_->clear();
  // Read double this to get some eviction from SSD.
  readFiles(
      "prefix1_",
      0,
      kSsdBytes * 2 / bytesPerFile,
      30,
      100,
      1,
      kStripesPerFile,
      4);
  // Expect some hits from SSD.
  EXPECT_LE(kSsdBytes / 8, ioStats_->ssdRead().sum());
  // We expec some prefetch but the quantity is nondeterminstic
  // because cases where the main thread reads the data ahead of
  // background reader does not count as prefetch even if prefetch was
  // issued. Also, the head of each file does not get prefetched
  // because each file has its own tracker.
  EXPECT_LE(kSsdBytes / 8, ioStats_->prefetch().sum());

  readFiles(
      "prefix1_",
      kSsdBytes / bytesPerFile,
      4 * kSsdBytes / bytesPerFile,
      30,
      100,
      1,
      kStripesPerFile,
      4);
}

TEST_F(CacheTest, singleFileThreads) {
  initializeCache(1 << 30);

  const int numThreads = 4;
  std::vector<std::thread> threads;
  threads.reserve(numThreads);
  for (int i = 0; i < numThreads; ++i) {
    threads.push_back(std::thread([this, i]() {
      readLoop(
          fmt::format("testfile{}", i),
          10,
          70,
          10,
          20,
          4,
          /*noCacheRetention=*/false,
          ioStats_,
          fsStats_);
    }));
  }
  for (auto i = 0; i < numThreads; ++i) {
    threads[i].join();
  }
}

TEST_F(CacheTest, ssdThreads) {
  initializeCache(64 << 20, 1024 << 20);
  deterministic_ = true;
  constexpr int32_t kNumThreads = 8;
  std::vector<IoStatisticsPtr> stats;
  stats.reserve(kNumThreads);
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  std::vector<std::shared_ptr<filesystems::File::IoStats>> fsStats;
  fsStats.reserve(kNumThreads);

  // We read 4 files on 8 threads. Threads 0 and 1 read file 0, 2 and 3 read
  // file 1 etc. Each tread reads its file 4 times.
  for (int i = 0; i < kNumThreads; ++i) {
    stats.push_back(std::make_shared<io::IoStatistics>());
    fsStats.push_back(std::make_shared<filesystems::File::IoStats>());
    threads.push_back(std::thread(
        [i, this, threadStats = stats.back(), fsStat = fsStats.back()]() {
          for (auto counter = 0; counter < 4; ++counter) {
            readLoop(
                fmt::format("testfile{}", i / 2),
                10,
                70,
                10,
                20,
                2,
                /*noCacheRetention=*/false,
                threadStats,
                fsStat);
          }
        }));
  }
  for (int i = 0; i < kNumThreads; ++i) {
    threads[i].join();
  }
  executor_->join();
  for (auto i = 0; i < kNumThreads; ++i) {
    // All threads access the same amount. Where the data comes from varies.
    EXPECT_EQ(stats[0]->rawBytesRead(), stats[i]->rawBytesRead());

    EXPECT_GE(stats[i]->rawBytesRead(), stats[i]->ramHit().sum());

    // Prefetch is <= read from storage + read from SSD.
    EXPECT_LE(
        stats[i]->prefetch().sum(),
        stats[i]->read().sum() + stats[i]->ssdRead().sum());
  }
  LOG(INFO) << cache_->toString();
}

class FileWithReadAhead {
 public:
  static constexpr int32_t kFileSize = 21 << 20;
  static constexpr int64_t kLoadQuantum = 6 << 20;
  FileWithReadAhead(
      const std::string& name,
      cache::AsyncDataCache* cache,
      IoStatisticsPtr stats,
      std::shared_ptr<filesystems::File::IoStats> fsStats,
      memory::MemoryPool& pool,
      folly::Executor* executor)
      : options_(&pool) {
    fileId_ = std::make_unique<StringIdLease>(fileIds(), name);
    file_ = std::make_shared<TestReadFile>(fileId_->id(), kFileSize, fsStats);
    options_.setNoCacheRetention(true);
    bufferedInput_ = std::make_unique<CachedBufferedInput>(
        file_,
        MetricsLog::voidLog(),
        *fileId_,
        cache,
        nullptr,
        StringIdLease{},
        stats,
        fsStats,
        executor,
        options_);
    auto sequential = StreamIdentifier::sequentialFile();
    stream_ = bufferedInput_->enqueue(Region{0, file_->size()}, &sequential);
    VELOX_CHECK(reinterpret_cast<CacheInputStream*>(stream_.get())
                    ->testingNoCacheRetention());
    // Trigger load of next 4MB after reading the first 2MB of the previous 4MB
    // quantum.
    reinterpret_cast<CacheInputStream*>(stream_.get())->setPrefetchPct(50);
    bufferedInput_->load(LogType::FILE);
  }

  bool next(const void*& buffer, int32_t& size) {
    return stream_->Next(&buffer, &size);
  }

 private:
  std::unique_ptr<StringIdLease> fileId_;
  std::unique_ptr<CachedBufferedInput> bufferedInput_;
  std::unique_ptr<SeekableInputStream> stream_;
  std::shared_ptr<TestReadFile> file_;
  io::ReaderOptions options_;
};

TEST_F(CacheTest, readAhead) {
  constexpr int32_t kNumThreads = 3;
  constexpr int32_t kFilesPerThread = 100;
  constexpr int32_t kMinRead = 700000;

  constexpr int64_t kExpectedSize =
      kNumThreads * kFilesPerThread * FileWithReadAhead::kLoadQuantum;
  initializeCache(kExpectedSize * 1.7, 0);
  deterministic_ = true;
  std::vector<IoStatisticsPtr> stats;
  stats.reserve(kNumThreads);
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);
  std::vector<std::shared_ptr<filesystems::File::IoStats>> fsStats;
  fsStats.reserve(kNumThreads);

  // We read kFilesPerThread on each thread. The files are read in parallel,
  // advancing each file in turn. Read-ahead is triggered when a fraction of the
  // current cache entry of each file is consumed.

  for (int threadIndex = 0; threadIndex < kNumThreads; ++threadIndex) {
    stats.push_back(std::make_shared<io::IoStatistics>());
    fsStats.push_back(std::make_shared<filesystems::File::IoStats>());
    threads.push_back(std::thread([threadIndex,
                                   this,
                                   threadStats = stats.back(),
                                   fsStat = fsStats.back()]() {
      std::vector<std::unique_ptr<FileWithReadAhead>> files;
      auto firstFileNumber = threadIndex * kFilesPerThread;
      for (auto i = 0; i < kFilesPerThread; ++i) {
        auto name = fmt::format("prefetch_{}", i + firstFileNumber);
        files.push_back(std::make_unique<FileWithReadAhead>(
            name, cache_.get(), threadStats, fsStat, *pool_, executor_.get()));
      }
      std::vector<int64_t> totalRead(kFilesPerThread);
      std::vector<int64_t> bytesLeft(kFilesPerThread);
      for (auto counter = 0; counter < 100; ++counter) {
        for (auto i = 0; i < kFilesPerThread; ++i) {
          if (!files[i]) {
            continue; // This set of files is finished.
          }
          // Read from the next file. Different files advance at slightly
          // different rates.
          auto bytesNeeded = kMinRead + i * 1000;
          while (bytesLeft[i] < bytesNeeded) {
            const void* buffer;
            int32_t size;
            if (!files[i]->next(buffer, size)) {
              // End of file. Check that a multiple of file size has been read.
              EXPECT_EQ(0, totalRead[i] % FileWithReadAhead::kFileSize);
              if (totalRead[i] >= 3 * FileWithReadAhead::kFileSize) {
                files[i] = nullptr;
                break;
              }
              // Open a new file with a different unique name.
              auto newName = fmt::format(
                  "prefetch_{}",
                  (static_cast<int64_t>(firstFileNumber) + i + i) * 1000000000 +
                      totalRead[i]);
              files[i] = std::make_unique<FileWithReadAhead>(
                  newName,
                  cache_.get(),
                  threadStats,
                  fsStat,
                  *pool_,
                  executor_.get());
              continue;
            }
            totalRead[i] += size;
            bytesLeft[i] += size;
          }
          bytesLeft[i] -= bytesNeeded;
        }
      }
    }));
  }
  int64_t bytes = 0;
  int32_t count = 0;
  for (int i = 0; i < kNumThreads; ++i) {
    threads[i].join();
    bytes += stats[i]->prefetch().sum();
    count += stats[i]->prefetch().count();
  }
  executor_->join();

  LOG(INFO) << count << " prefetches with total " << bytes << " bytes";
}

TEST_F(CacheTest, noCacheRetention) {
  const int64_t cacheSize = 1LL << 30;
  struct {
    bool noCacheRetention;
    bool hasSsdCache;
    int readPct;

    std::string debugString() const {
      return fmt::format(
          "noCacheRetention {}, hasSsdCache {}, readPct {}",
          noCacheRetention,
          hasSsdCache,
          readPct);
    }
  } testSettings[] = {
      {true, true, 100},
      {true, false, 100},
      {false, false, 100},
      {false, true, 100},
      {true, true, 10},
      {true, false, 100},
      {false, false, 100},
      {false, true, 100}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    // 1GB RAM with 1GB SSD or not so there is always sufficient space to cache
    // all the read data in both ram and ssd caches.
    initializeCache(cacheSize, testData.hasSsdCache ? cacheSize : 0);
    testRandomSeek_ = true;
    deterministic_ = true;

    // We read one stripe with all columns,
    readLoop(
        "noCacheRetention",
        20,
        testData.readPct,
        1,
        5,
        1,
        testData.noCacheRetention,
        ioStats_,
        fsStats_);
    // This is a cold read, so expect no hits.
    ASSERT_EQ(ioStats_->ramHit().sum(), 0);
    // Only one reference per column so there is no prefetch.
    ASSERT_LT(0, ioStats_->prefetch().sum());
    // Expect some extra reading from coalescing.
    ASSERT_LT(0, ioStats_->rawOverreadBytes());
    ASSERT_LT(0, ioStats_->rawBytesRead());
    auto* ssdCache = cache_->ssdCache();
    if (ssdCache != nullptr) {
      ssdCache->waitForWriteToFinish();
      if (testData.noCacheRetention) {
        ASSERT_EQ(ssdCache->stats().entriesCached, 0);
      } else {
        ASSERT_GT(ssdCache->stats().entriesCached, 0);
      }
      ASSERT_EQ(ssdCache->stats().regionsEvicted, 0);
    }
    const auto cacheEntries = asyncDataCacheHelper_->cacheEntries();
    for (const auto& cacheEntry : cacheEntries) {
      const auto cacheEntryHelper =
          std::make_unique<test::AsyncDataCacheEntryTestHelper>(cacheEntry);
      if (testData.noCacheRetention) {
        ASSERT_EQ(cacheEntryHelper->accessStats().numUses, 0);
        ASSERT_EQ(cacheEntryHelper->accessStats().lastUse, 0);
      } else {
        ASSERT_GE(cacheEntryHelper->accessStats().numUses, 0);
        ASSERT_NE(cacheEntryHelper->accessStats().lastUse, 0);
      }
    }
    const auto stats = cache_->refreshStats();
    ASSERT_EQ(stats.numEvict, 0);
    ASSERT_EQ(stats.numEntries, cacheEntries.size());
  }
}

TEST_F(CacheTest, loadQuotumTooLarge) {
  initializeCache(64 << 20, 256 << 20);
  StringIdLease fileId{fileIds(), "foo"};
  auto readFile =
      std::make_shared<TestReadFile>(fileId.id(), 10 << 20, nullptr);
  auto readOptions = io::ReaderOptions(pool_.get());
  readOptions.setLoadQuantum(9 << 20 /*9MB*/);
  VELOX_ASSERT_THROW(
      std::make_unique<CachedBufferedInput>(
          readFile,
          MetricsLog::voidLog(),
          fileId,
          cache_.get(),
          nullptr,
          StringIdLease{},
          nullptr,
          nullptr,
          executor_.get(),
          readOptions),
      "Load quantum exceeded SSD cache entry size limit");
}

TEST_F(CacheTest, ssdReadVerification) {
  constexpr int64_t kMemoryBytes = 32 << 20;
  constexpr int64_t kSsdBytes = 256 << 20;
  // 32 RAM, 256MB SSD, with checksumWrite/checksumReadVerification enabled.
  initializeCache(kMemoryBytes, kSsdBytes, true);

  StringIdLease fileId;
  StringIdLease groupId;
  auto file = inputByPath("test_file", fileId, groupId);
  auto tracker = std::make_shared<ScanTracker>(
      "testTracker", nullptr, io::ReaderOptions::kDefaultLoadQuantum);
  auto input = std::make_unique<CachedBufferedInput>(
      file,
      MetricsLog::voidLog(),
      fileId,
      cache_.get(),
      tracker,
      groupId,
      ioStats_,
      fsStats_,
      executor_.get(),
      io::ReaderOptions(pool_.get()));

  const auto readData = [&](uint32_t numBytesRead) {
    const uint64_t kNumBytesPerRead = 4 << 20;
    for (uint64_t offset = 0; offset < numBytesRead;
         offset += kNumBytesPerRead) {
      auto stream = input->read(offset, kNumBytesPerRead, LogType::TEST);
      const void* buffer;
      int32_t size;
      int32_t bytes = 0;
      while (bytes < kNumBytesPerRead) {
        EXPECT_TRUE(stream->Next(&buffer, &size));
        bytes += size;
      }
    }
  };

  // Read kMemoryBytes of data.
  readData(kMemoryBytes);
  waitForWrite();
  auto stats = cache_->refreshStats();
  // This is a cold read, so expect no cache hit.
  ASSERT_EQ(stats.numHit, 0);
  ASSERT_EQ(stats.ssdStats->entriesRead, 0);
  ASSERT_EQ(stats.ssdStats->readSsdCorruptions, 0);
  ASSERT_GT(ioStats_->read().sum(), 0);
  ASSERT_EQ(ioStats_->ramHit().sum(), 0);
  ASSERT_EQ(ioStats_->ssdRead().sum(), 0);

  // Read kSsdBytes of data.
  readData(kSsdBytes);
  waitForWrite();
  stats = cache_->refreshStats();
  // Expect memory cache hits.
  ASSERT_GT(stats.numHit, 0);
  ASSERT_EQ(stats.ssdStats->entriesRead, 0);
  ASSERT_EQ(stats.ssdStats->readSsdCorruptions, 0);
  ASSERT_GT(ioStats_->read().sum(), 0);
  ASSERT_GT(ioStats_->ramHit().sum(), 0);
  ASSERT_EQ(ioStats_->ssdRead().sum(), 0);

  // Read kSsdBytes of data.
  readData(kSsdBytes);
  waitForWrite();
  stats = cache_->refreshStats();
  // Expect SSD cache hits.
  ASSERT_GT(stats.numHit, 0);
  ASSERT_GT(stats.ssdStats->entriesRead, 0);
  ASSERT_EQ(stats.ssdStats->readSsdCorruptions, 0);
  ASSERT_GT(ioStats_->read().sum(), 0);
  ASSERT_GT(ioStats_->ramHit().sum(), 0);
  ASSERT_GT(ioStats_->ssdRead().sum(), 0);

  // Corrupt SSD cache file.
  corruptSsdFile(fmt::format("{}/cache0", tempDirectory_->getPath()));
  // Clear memory cache to force ssd read.
  cache_->clear();

  // Record the baseline stats.
  const auto prevStats = cache_->refreshStats();
  const auto prevRead = ioStats_->read().sum();
  const auto prevRamHit = ioStats_->ramHit().sum();
  const auto prevSsdRead = ioStats_->ssdRead().sum();

  // Read from the corrupted cache.
  readData(kSsdBytes);
  waitForWrite();
  stats = cache_->refreshStats();
  // Expect all new reads to be recorded as corruptions.
  ASSERT_GT(ioStats_->read().sum(), prevRead);
  ASSERT_GT(stats.ssdStats->readSsdCorruptions, 0);
  ASSERT_EQ(
      stats.ssdStats->readSsdCorruptions,
      stats.ssdStats->entriesRead - prevStats.ssdStats->entriesRead);
  // Expect no new succeeded cache hits.
  ASSERT_EQ(stats.numHit, prevStats.numHit);
  ASSERT_EQ(ioStats_->ramHit().sum(), prevRamHit);
  ASSERT_EQ(ioStats_->ssdRead().sum(), prevSsdRead);
}
