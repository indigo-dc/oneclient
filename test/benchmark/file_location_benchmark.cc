/**
 * @file fileLocation_test.cc
 * @author Rafal Grzeszczuk
 * @copyright (C) 2018 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "folly/Benchmark.h"
#include "folly/Foreach.h"
#include "messages/fuse/fileBlock.h"
#include "messages/fuse/fileLocation.h"
#include <boost/random.hpp>
#include <boost/range/irange.hpp>
#include <utility>
using namespace one::messages::fuse;

constexpr auto blockSize = 1024; // 1KB
BENCHMARK(benchmarkPutSingleBlock)
{
    auto fileLocation = FileLocation{};
    fileLocation.putBlock(0, blockSize, FileBlock{" ", " "});
    folly::doNotOptimizeAway(fileLocation);
}
BENCHMARK(benchmarkPutHugeBlock)
{
    auto fileLocation = FileLocation{};
    fileLocation.putBlock(
        0, blockSize * blockSize * blockSize, FileBlock{" ", " "});
    folly::doNotOptimizeAway(fileLocation);
}
BENCHMARK(benchmarkPutManyBlocks)
{
    FOR_EACH_RANGE(i, 0, 1000000)
    {
        auto fileLocation = FileLocation{};
        fileLocation.putBlock(blockSize * i, blockSize, FileBlock{" ", " "});
        folly::doNotOptimizeAway(fileLocation);
    }
}
BENCHMARK(benchmarkPutBlockRandomly)
{
    auto fileLocation = FileLocation{};
    auto randomBlockNumber = 0;
    BENCHMARK_SUSPEND
    {
        for (auto i : boost::irange(1, 1024))
            fileLocation.putBlock(
                i * blockSize, blockSize, FileBlock{" ", " "});
    }
    BENCHMARK_SUSPEND
    {
        std::time_t now = std::time(0);
        boost::random::mt19937 gen{static_cast<std::uint32_t>(now)};
        boost::random::uniform_int_distribution<> randomBlock(1, 1024 * 1024);
        randomBlockNumber = randomBlock(gen);
        folly::doNotOptimizeAway(fileLocation);
        folly::doNotOptimizeAway(randomBlockNumber);
    }

    fileLocation.putBlock(randomBlockNumber, blockSize, FileBlock{" ", " "});
}
BENCHMARK(benchmarkPutManyBlocksRandomly)
{
    auto fileLocation = FileLocation{};
    auto randomBlockNumber = 0;
    BENCHMARK_SUSPEND
    {
        for (auto i : boost::irange(1, 1024))
            fileLocation.putBlock(
                i * blockSize, blockSize, FileBlock{" ", " "});
    }
    FOR_EACH_RANGE(i, 0, 100000)
    {
        BENCHMARK_SUSPEND
        {
            std::time_t now = std::time(0);
            boost::random::mt19937 gen{static_cast<std::uint32_t>(now)};
            boost::random::uniform_int_distribution<> randomBlock(
                1, 1024 * 1024);
            randomBlockNumber = randomBlock(gen);
            folly::doNotOptimizeAway(fileLocation);
            folly::doNotOptimizeAway(randomBlockNumber);
        }

        fileLocation.putBlock(
            randomBlockNumber, blockSize, FileBlock{" ", " "});
    }
}
BENCHMARK(benchmarkFileBlockCreation)
{
    auto fileBlock = FileBlock{" ", " "};

    folly::doNotOptimizeAway(fileBlock);
}
BENCHMARK(benchmarkToString)
{
    auto fileLocation = FileLocation{};
    BENCHMARK_SUSPEND
    {
        fileLocation.putBlock(0, blockSize, FileBlock{" ", " "});
        folly::doNotOptimizeAway(fileLocation);
    }
    fileLocation.toString();
}
BENCHMARK(benchmarkProgressString)
{
    auto fileLocation = FileLocation{};
    BENCHMARK_SUSPEND
    {
        fileLocation.putBlock(0, blockSize, FileBlock{" ", " "});
        folly::doNotOptimizeAway(fileLocation);
    }
    fileLocation.progressString(blockSize, 10);
}
BENCHMARK(benchmarkReplicationProgress)
{
    auto fileLocation = FileLocation{};
    BENCHMARK_SUSPEND
    {
        fileLocation.putBlock(0, blockSize, FileBlock{" ", " "});
        folly::doNotOptimizeAway(fileLocation);
    }
    fileLocation.replicationProgress(blockSize);
}
BENCHMARK(benchmarkBoostInterval)
{
    auto fileLocation = FileLocation{};
    BENCHMARK_SUSPEND
    {
        fileLocation.putBlock(0, blockSize, FileBlock{" ", " "});
        folly::doNotOptimizeAway(fileLocation);
    }
    auto interval =
        boost::icl::discrete_interval<off_t>::right_open(0, blockSize);
    folly::doNotOptimizeAway(interval);
}
int main() { folly::runBenchmarks(); }
