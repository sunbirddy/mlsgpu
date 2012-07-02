/**
 * @file
 *
 * Implementation of normal-computing using bucketing to handle large inputs OOC.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <memory>
#include <cmath>
#include <stdexcept>
#include <boost/program_options.hpp>
#include <boost/foreach.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/ref.hpp>
#include <stxxl.h>
#include <Eigen/Core>
#include "../src/bucket.h"
#include "../src/statistics.h"
#include "../src/splat_set.h"
#include "../src/fast_ply.h"
#include "../src/logging.h"
#include "../src/progress.h"
#include "../src/options.h"
#include "../src/worker_group.h"
#include "normals.h"
#include "normals_bucket.h"
#include "knng.h"

namespace po = boost::program_options;

namespace Option
{
    static const char *maxHostSplats() { return "max-host-splats"; }
    static const char *maxSplit()      { return "max-split"; }
    static const char *leafSize()      { return "leaf-size"; }
};

void addBucketOptions(po::options_description &opts)
{
    opts.add_options()
        (Option::maxHostSplats(), po::value<std::size_t>()->default_value(8000000), "Maximum splats per bin")
        (Option::maxSplit(),     po::value<int>()->default_value(2097152), "Maximum fan-out in partitioning")
        (Option::leafSize(),     po::value<double>()->default_value(2560.0), "Size of top-level octree leaves");
}

template<typename S, typename T>
class TransformSplatSet : public S
{
public:
    typedef T Transform;

    SplatSet::SplatStream *makeSplatStream() const
    {
        std::auto_ptr<SplatSet::SplatStream> child(S::makeSplatStream());
        SplatSet::SplatStream *stream = new MySplatStream(child.get(), transform);
        child.release();
        return stream;
    }

    template<typename RangeIterator>
    SplatSet::SplatStream *makeSplatStream(RangeIterator first, RangeIterator last) const
    {
        std::auto_ptr<SplatSet::SplatStream> child(S::makeSplatStream(first, last));
        SplatSet::SplatStream *stream = new MySplatStream(child.get(), transform);
        child.release();
        return stream;
    }

    SplatSet::BlobStream *makeBlobStream(const Grid &grid, Grid::size_type bucketSize) const
    {
        return new SplatSet::SimpleBlobStream(makeSplatStream(), grid, bucketSize);
    }

    void setTransform(const Transform &transform)
    {
        this->transform = transform;
    }

private:
    Transform transform;

    class MySplatStream : public SplatSet::SplatStream
    {
    private:
        boost::scoped_ptr<SplatSet::SplatStream> child;
        Transform transform;

    public:
        MySplatStream(SplatSet::SplatStream *child, const Transform &transform)
            : child(child), transform(transform) {}

        virtual SplatStream &operator++()
        {
            ++*child;
            return *this;
        }

        virtual Splat operator*() const
        {
            return boost::unwrap_ref(transform)(**child);
        }

        virtual bool empty() const
        {
            return child->empty();
        }

        virtual SplatSet::splat_id currentId() const
        {
            return child->currentId();
        }
    };
};

class TransformSetRadius
{
private:
    float radius;

public:
    explicit TransformSetRadius(float radius = 0.0) : radius(radius) {}

    Splat operator()(Splat s) const
    {
        s.radius = radius;
        return s;
    }
};


struct NormalItem
{
    Grid binGrid;
    int numNeighbors;
    float maxDistance2;
    ProgressDisplay *progress;

    Statistics::Container::vector<Splat> splats;

    NormalItem() : splats("mem.splats") {}
};

class NormalWorker
{
private:
    Statistics::Variable &neighborStat;
    Statistics::Variable &computeStat;
    Statistics::Variable &qualityStat;
    Statistics::Variable &angleStat;

public:
    NormalWorker()
    :
        neighborStat(Statistics::getStatistic<Statistics::Variable>("neighbors")),
        computeStat(Statistics::getStatistic<Statistics::Variable>("normal.worker.time")),
        qualityStat(Statistics::getStatistic<Statistics::Variable>("quality")),
        angleStat(Statistics::getStatistic<Statistics::Variable>("angle"))
    {}

    void start() {}
    void stop() {}

    void operator()(int gen, NormalItem &item)
    {
        Statistics::Timer timer(computeStat);
        (void) gen;

        std::vector<std::vector<std::pair<float, std::tr1::uint32_t> > > nn
            = knng(item.splats, item.numNeighbors, item.maxDistance2);

        std::vector<Eigen::Vector3f> neighbors;
        neighbors.reserve(item.numNeighbors);
        for (std::size_t i = 0; i < item.splats.size(); i++)
        {
            const Splat &s = item.splats[i];
            float vertexCoords[3];
            item.binGrid.worldToVertex(s.position, vertexCoords);
            bool inside = true;
            for (int j = 0; j < 3; j++)
                inside &= vertexCoords[j] >= 0.0f && vertexCoords[j] < item.binGrid.numVertices(j);
            if (inside)
            {
                neighbors.clear();

                for (std::size_t j = 0; j < nn[i].size(); j++)
                {
                    int idx = nn[i][j].second;
                    const Splat &sn = item.splats[idx];
                    neighbors.push_back(Eigen::Vector3f(sn.position[0], sn.position[1], sn.position[2]));
                }
                neighborStat.add(neighbors.size() == std::size_t(item.numNeighbors));

                if (neighbors.size() == std::size_t(item.numNeighbors))
                {
                    float angle, quality;
                    Eigen::Vector3f normal;
                    normal = computeNormal(s, neighbors, angle, quality);
                    angleStat.add(angle);
                    qualityStat.add(quality);
                }
            }
        }
        if (item.progress != NULL)
            *item.progress += item.binGrid.numCells();
    }
};

class NormalWorkerGroup : public WorkerGroup<NormalItem, int, NormalWorker, NormalWorkerGroup>
{
public:
    NormalWorkerGroup(std::size_t numWorkers, std::size_t spare)
        : WorkerGroup<NormalItem, int, NormalWorker, NormalWorkerGroup>(
            numWorkers, spare,
            Statistics::getStatistic<Statistics::Variable>("normal.worker.push"),
            Statistics::getStatistic<Statistics::Variable>("normal.worker.pop.first"),
            Statistics::getStatistic<Statistics::Variable>("normal.worker.pop"),
            Statistics::getStatistic<Statistics::Variable>("normal.worker.get"))
    {
        for (std::size_t i = 0; i < numWorkers; i++)
            addWorker(new NormalWorker());
        for (std::size_t i = 0; i < numWorkers + spare; i++)
            addPoolItem(boost::make_shared<NormalItem>());
    }
};

template<typename Splats>
class BinProcessor
{
private:
    NormalWorkerGroup &outGroup;

    int numNeighbors;
    float maxDistance2;

    ProgressDisplay *progress;

    Statistics::Variable &loadStat;

public:
    BinProcessor(
        NormalWorkerGroup &outGroup,
        int numNeighbors,
        float maxDistance,
        ProgressDisplay *progress = NULL)
    :
        outGroup(outGroup),
        numNeighbors(numNeighbors), maxDistance2(maxDistance * maxDistance),
        progress(progress),
        loadStat(Statistics::getStatistic<Statistics::Variable>("load.time"))
    {}

    void operator()(const typename SplatSet::Traits<Splats>::subset_type &subset,
                    const Grid &binGrid, const Bucket::Recursion &recursionState)
    {
        (void) recursionState;
        Log::log[Log::debug] << binGrid.numCells(0) << " x " << binGrid.numCells(1) << " x " << binGrid.numCells(2) << '\n';

        boost::shared_ptr<NormalItem> item = outGroup.get();

        {
            Statistics::Timer timer(loadStat);
            item->splats.reserve(subset.maxSplats());
            boost::scoped_ptr<SplatSet::SplatStream> stream(subset.makeSplatStream());
            item->splats.clear();
            while (!stream->empty())
            {
                Splat s = **stream;
                item->splats.push_back(s);
                ++*stream;
            }
            item->binGrid = binGrid;
            item->numNeighbors = numNeighbors;
            item->maxDistance2 = maxDistance2;
            item->progress = progress;
        }
        outGroup.push(0, item);
    }
};

void runBucket(const po::variables_map &vm)
{
    const int bucketSize = 256;
    const float leafSize = vm[Option::leafSize()].as<double>();
    const float spacing = leafSize / bucketSize;
    const float radius = vm[Option::radius()].as<double>();
    const int numNeighbors = vm[Option::neighbors()].as<int>();

    const std::size_t maxHostSplats = vm[Option::maxHostSplats()].as<std::size_t>();
    const std::size_t maxSplit = vm[Option::maxSplit()].as<int>();
    const std::vector<std::string> &names = vm[Option::inputFile()].as<std::vector<std::string> >();
    const FastPly::ReaderType readerType = vm[Option::reader()].as<Choice<FastPly::ReaderTypeWrapper> >();

    typedef TransformSplatSet<SplatSet::FileSet, TransformSetRadius> Set0;
    typedef SplatSet::FastBlobSet<Set0, stxxl::VECTOR_GENERATOR<SplatSet::BlobData>::result > Splats;
    Splats splats;
    splats.setTransform(TransformSetRadius(radius));

    BOOST_FOREACH(const std::string &name, names)
    {
        std::auto_ptr<FastPly::ReaderBase> reader(FastPly::createReader(readerType, name, 1.0f));
        splats.addFile(reader.get());
        reader.release();
    }

    try
    {
        Statistics::Timer timer("bbox.time");
        splats.computeBlobs(spacing, bucketSize, &Log::log[Log::info]);
    }
    catch (std::length_error &e)
    {
        std::cerr << "At least one input point is required.\n";
        std::exit(1);
    }

    Grid grid = splats.getBoundingGrid();
    ProgressDisplay progress(grid.numCells(), Log::log[Log::info]);

    NormalWorkerGroup normalGroup(8, 4);
    BinProcessor<Splats> binProcessor(normalGroup, numNeighbors, radius, &progress);

    normalGroup.producerStart(0);
    normalGroup.start();
    Bucket::bucket(splats, grid, maxHostSplats, bucketSize, 0, true, maxSplit,
                   boost::ref(binProcessor), &progress);
    normalGroup.producerStop(0);
    normalGroup.stop();
}
