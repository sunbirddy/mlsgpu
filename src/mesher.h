/*
 * mlsgpu: surface reconstruction from point clouds
 * Copyright (C) 2013  University of Cape Town
 *
 * This file is part of mlsgpu.
 *
 * mlsgpu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Data structures for storing the output of @ref Marching.
 *
 * The classes in this file are @ref MesherBase, an abstract base class, and
 * one concrete instantiation of it.
 */

#ifndef MESHER_H
#define MESHER_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <CL/cl.h>

#include <string>
#include <vector>
#include <map>
#include <string>
#include <iosfwd>
#include <utility>
#include <boost/array.hpp>
#include <boost/noncopyable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/optional.hpp>
#include <boost/foreach.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/vector.hpp>
#include "tr1_unordered_map.h"
#include "tr1_unordered_set.h"
#include "marching.h"
#include "fast_ply.h"
#include "union_find.h"
#include "work_queue.h"
#include "worker_group.h"
#include "statistics.h"
#include "allocator.h"
#include "timeplot.h"
#include "circular_buffer.h"
#include "chunk_id.h"
#include "progress.h"

class TestTmpWriterWorkerGroup;

namespace boost
{
namespace serialization
{

template<typename Archive, typename T, typename Alloc>
inline void serialize(Archive &ar, Statistics::Container::vector<T, Alloc> &v, const unsigned int)
{
    ar & boost::serialization::base_object<std::vector<T, Alloc> >(v);
}

template<typename Archive>
inline void save(Archive &ar, const boost::filesystem::path &path, const unsigned int)
{
    ar << path.string();
}

template<typename Archive>
inline void load(Archive &ar, boost::filesystem::path &path, const unsigned int)
{
    boost::filesystem::path::string_type s;
    ar >> s;
    path = s;
}

template<typename Archive>
inline void serialize(Archive &ar, boost::filesystem::path &path, const unsigned int version)
{
    boost::serialization::split_free(ar, path, version);
}

} // namespace serialization
} // namespace boost

/**
 * Enumeration of the supported mesher types
 */
enum MesherType
{
    OOC_MESHER
};

/**
 * Wrapper around @ref MesherType for use with @ref Choice.
 */
class MesherTypeWrapper
{
public:
    typedef MesherType type;
    static std::map<std::string, MesherType> getNameMap();
};

/**
 * Data about a mesh passed in to a @ref MesherBase::InputFunctor. It contains
 * host mesh data that may still be being read asynchronously from a device,
 * together with the events that will signal data readiness.
 */
struct MesherWork
{
    ChunkId chunkId;               ///< Chunk containing this mesh
    HostKeyMesh mesh;              ///< Mesh data (may be empty)
    bool hasEvents;                ///< If false, the event fields have undefined values
    cl::Event verticesEvent;       ///< Signaled when vertices may be read
    cl::Event vertexKeysEvent;     ///< Signaled when vertex keys may be read
    cl::Event trianglesEvent;      ///< Signaled when triangles may be read
};

/**
 * Model of @ref MesherBase::Namer that always returns a fixed filename.
 */
class TrivialNamer
{
private:
    std::string name;

public:
    typedef std::string result_type;
    const std::string &operator()(const ChunkId &chunkId) const
    {
        (void) chunkId;
        return name;
    }

    TrivialNamer(const std::string &name) : name(name) {}
};

/**
 * Model of @ref MesherBase::Namer that adds the chunk ID into the name.
 *
 * The generated name is
 * <i>base</i><code>_</code><i>XXXX</i><code>_</code><i>YYYY</i><code>_</code><i>ZZZZ</i><code>.ply</code>,
 * where @a base is the base name given to the constructor and @a XXXX, @a YYYY
 * and @a ZZZZ are the coordinates.
 */
class ChunkNamer
{
private:
    std::string baseName;

public:
    typedef std::string result_type;
    std::string operator()(const ChunkId &chunkId) const;

    ChunkNamer(const std::string &baseName) : baseName(baseName) {}
};

/**
 * Abstract base class for output collectors for @ref Marching. This class
 * only captures the host side of the process. It needs to be wrapped in
 * using @ref deviceMesher or @ref MesherGroup to satisfy
 * the requirements for @ref Marching.
 *
 * The basic procedure for using one of these classes is:
 * -# Instantiate it.
 * -# Call @ref setPruneThreshold.
 * -# Call @ref numPasses to determine how many passes are required.
 * -# For each pass, call @ref functor to obtain a functor, then
 *    make as many calls to @ref Marching::generate as desired using this
 *    functor. Each call should set @a keyOffset so that vertex keys line up.
 *    Each pass must generate exactly the same geometry, but the blocks may
 *    be generated in different order within each chunk (chunks must be in
 *    order).
 * -# Call @ref write.
 *
 * @warning The functor is @em not required to be thread-safe. The caller must
 * serialize calls if necessary (@ref MesherGroup only uses one thread).
 */
class MesherBase
{
public:
    /**
     * Type returned by @ref functor. The argument is a mesh to be processed.
     * After the function returns the mesh is not used again, so it may be
     * modified as past of the implementation.
     */
    typedef boost::function<void(MesherWork &work, Timeplot::Worker &tworker)> InputFunctor;

    /**
     * Function object that generates a filename from a chunk ID.
     */
    typedef boost::function<std::string(const ChunkId &chunkId)> Namer;

    /**
     * Constructor. The mesher object retains a reference to @a writer and so it
     * must persist until the mesher is destroyed. The @a namer is copied and so
     * may be transient.
     *
     * The @a writer must not be open when the constructor is called, nor
     * should it be directly accessed when the mesher exists. The mesher will
     * open and close the writer once per output file.
     *
     * @param writer         Writer that will be used to emit output files.
     * @param namer          Callback function to assign names to output files.
     */
    MesherBase(FastPly::Writer &writer, const Namer &namer)
        : pruneThreshold(0.0), reorderCapacity(4 * 1024 * 1024), writer(writer), namer(namer) {}

    /// Virtual destructor to allow destruction via base class pointer
    virtual ~MesherBase() {}

    /// Number of passes required.
    virtual unsigned int numPasses() const = 0;

    /**
     * Sets the lower bound on component size. All components that are
     * smaller will be pruned from the output, if supported by the mesher
     * type. The default is not to prune anything.
     *
     * @param threshold The lower bound, specified as a fraction of the total
     * number of pre-pruning vertices.
     */
    void setPruneThreshold(double threshold) { pruneThreshold = threshold; }

    /**
     * Sets the capacity (in bytes) of the reorder buffer, if there is one.
     */
    void setReorderCapacity(std::size_t bytes) { reorderCapacity = bytes; }

    /// Retrieve the value set with @ref setPruneThreshold.
    double getPruneThreshold() const { return pruneThreshold; }

    /// Retrieve the value set with @ref setReorderCapacity.
    std::size_t getReorderCapacity() const { return reorderCapacity; }

    /**
     * Retrieves a functor that will accept data in a specific pass.
     * Multi-pass classes may do finalization on a previous pass before
     * returning the functor, so this function should only be called for
     * pass @a pass once pass @a pass - 1 has completed. It must also
     * only be called once per pass.
     *
     * The functor might perform file I/O (depending on the subclass), in which
     * case it may throw any of the exceptions documented for @ref write.
     *
     * @pre @a pass is less than @ref numPasses().
     *
     * @warning The returned functor is @em not required to be thread-safe.
     */
    virtual InputFunctor functor(unsigned int pass) = 0;

    /**
     * Instead of calling @ref write, one may instead call this function. It will
     * serialize the state necessary to complete the writing into @a path. Later
     * (usually in a separate process), call @ref resume on a newly constructed
     * mesher of the same class with the same filename to complete the write operation.
     */
    virtual void checkpoint(Timeplot::Worker &tworker, const boost::filesystem::path &path) = 0;

    /**
     * Write the output files after a checkpoint.
     *
     * @param tworker         Timeplot worker for the current thread.
     * @param path            Checkpoint file written by @ref checkpoint.
     * @param progressStream  If non-NULL, a log stream for a progress meter.
     * @return Number of output files written
     *
     * @see @ref checkpoint
     */
    virtual std::size_t resume(Timeplot::Worker &tworker, const boost::filesystem::path &path,
                               std::ostream *progressStream = NULL) = 0;

    /**
     * Performs any final file I/O.
     *
     * @param tworker         Timeplot worker for the current thread
     * @param progressStream  If non-NULL, a log stream for a progress meter.
     * @throw std::ios_base::failure on I/O failure (including failure to open the file).
     * @throw std::overflow_error if too many connected components were found.
     * @throw std::overflow_error if too many vertices were found in one output chunk.
     * @return The number of output files written
     */
    virtual std::size_t write(Timeplot::Worker &tworker, std::ostream *progressStream = NULL) = 0;

protected:
    FastPly::Writer &getWriter() const { return writer; }
    std::string getOutputName(const ChunkId &id) const { return namer(id); }

private:
    /// Threshold set by @ref setPruneThreshold
    double pruneThreshold;
    /// Capacity set by @ref setReorderCapacity
    std::size_t reorderCapacity;

    FastPly::Writer &writer;       ///< Writer for output files
    const Namer namer;             ///< Output file namer
};

/**
 * Mesher class that can handle huge output meshes out-of-core.
 * It stores the data in temporary files before reordering and concatenating them
 * It thus requires storage roughly equal to the size of the output files (perhaps
 * smaller because it doesn't need a vertex count per polygon, but perhaps larger
 * because it keeps components that are later discarded).
 *
 * Component identification is implemented with a two-level approach. Within each
 * block, a union-find is performed to identify local components. These
 * components are referred to as @em clumps. Each vertex is given a <em>clump
 * id</em>. During welding, external vertices are used to identify clumps that
 * form part of the same component, and this is recorded in a union-find
 * structure over the clumps. Clumps are represented in both the per-chunk data
 * and globally, but "clump IDs" refer to the global representation, over which
 * the union-find tree is built.
 *
 * Vertices in a block are reordered by clump, and within a clump the vertices are
 * first the internal ones, then the external ones. External vertices that already
 * appeared in a previous clump in the same chunk are elided.
 *
 * Triangles are also ordered by clump. Internal vertices use clump-local coordinates,
 * while external vertices use an index that counts over the external indices
 * of the chunk, with all bits inverted (operator ~) to distinguish them. This
 * encoding is unambiguous provided that the total external vertices in a chunk
 * plus the total internal in a clump do not exceed 2^32 (at which point PLY
 * would be useless for output anyway).
 *
 * External vertices are entered into a hash table that maps their keys to
 * their (global) chunk ID, and a chunk-local hash table that maps it to the
 * triangle index used to encode it.
 */
class OOCMesher : public MesherBase
{
    friend class ::TestTmpWriterWorkerGroup;
    friend class boost::serialization::access;
public:
    typedef boost::array<float, 3> vertex_type;
    typedef boost::array<cl_uint, 3> triangle_type;

protected:
    static const int reorderSlots;

    typedef std::tr1::int32_t clump_id;

    /**
     * Data for a single chunk.
     */
    class Chunk
    {
    public:
        /**
         * Chunk-local clump data. This is used for referencing either the
         * temporary files long-term, or the reorder buffers short-term.
         */
        struct Clump
        {
            /// Index within intermediate vertices of the first vertex
            std::tr1::uint64_t firstVertex;
            /// Number of internal vertices, starting from @ref firstVertex
            std::tr1::uint32_t numInternalVertices;
            /**
             * Number of external vertices, starting from @ref firstVertex +
             * @ref numInternalVertices. External vertices that are present
             * in a previous clump of the same chunk are not output and are
             * not included in this count.
             */
            std::tr1::uint32_t numExternalVertices;
            /// Index within intermediate triangles of the first triangle
            std::tr1::uint64_t firstTriangle;
            /// Number of triangles, starting from @ref firstTriangle
            std::tr1::uint32_t numTriangles;
            /// Index within @ref OOCMesher::clumps of this clump
            clump_id globalId;

            /**
             * Constructor. Parameters correspond to data members of the same name.
             */
            Clump(
                std::tr1::uint64_t firstVertex,
                std::tr1::uint32_t numInternalVertices,
                std::tr1::uint32_t numExternalVertices,
                std::tr1::uint64_t firstTriangle,
                std::tr1::uint32_t numTriangles,
                clump_id globalId)
                : firstVertex(firstVertex),
                numInternalVertices(numInternalVertices),
                numExternalVertices(numExternalVertices),
                firstTriangle(firstTriangle),
                numTriangles(numTriangles),
                globalId(globalId)
            {
            }

        private:
            friend class boost::serialization::access;

            /// Constructor used only by serialization
            Clump() {}

            template<typename Archive>
            void serialize(Archive &ar, const unsigned int)
            {
                ar & firstVertex;
                ar & numInternalVertices;
                ar & numExternalVertices;
                ar & firstTriangle;
                ar & numTriangles;
                ar & globalId;
            }
        };

        typedef Statistics::Container::unordered_map<cl_ulong, std::tr1::uint32_t> vertex_id_map_type;

        /// ID for this chunk, used to generate the filename
        ChunkId chunkId;
        /// All written clumps in this chunk, in the order they are recorded in the output vectors
        Statistics::Container::vector<Clump> clumps;
        /// Clumps that are still in the reorder buffer
        Statistics::Container::vector<Clump> bufferedClumps;
        /// Maps an external vertex key to the number of preceeding external vertices
        vertex_id_map_type vertexIdMap;
        /// Number of distinct external vertices in this chunk
        std::size_t numExternalVertices;

        /// Constructor
        explicit Chunk(const ChunkId chunkId = ChunkId())
            : chunkId(chunkId),
            clumps("mem.mesher.chunk.clumps"),
            bufferedClumps("mem.mesher.chunk.bufferedClumps"),
            vertexIdMap("mem.mesher.vertexIdMap"),
            numExternalVertices(0) {}

        template<typename Archive>
        void serialize(Archive &ar, const unsigned int)
        {
            ar & chunkId;
            ar & clumps;
            ar & numExternalVertices;
            // bufferedClumps and vertexIdMap are not needed
        }
    };

    /**
     * Component within a single block. The root clump also tracks the number of
     * vertices and triangles in a component.
     */
    class Clump : public UnionFind::Node<clump_id>
    {
    public:
        std::tr1::uint64_t vertices;   ///< Total unique vertices in the component (only valid at roots)
        std::tr1::uint64_t triangles;  ///< Total triangles in the component (only valid at roots)

        void merge(Clump &b)
        {
            UnionFind::Node<cl_int>::merge(b);
            vertices += b.vertices;
            triangles += b.triangles;
        }

        /**
         * Constructor for a new clump.
         * @param numVertices        The number of vertices in the clump.
         *
         * @post
         * - <code>vertices == numVertices</code>
         * - <code>triangles == 0</code>
         */
        Clump(std::tr1::uint64_t numVertices) : vertices(numVertices), triangles(0)
        {
        }

    private:
        friend class boost::serialization::access;

        /// Constructor used only for serialization
        Clump() {}

        template<typename Archive>
        void serialize(Archive &ar, const unsigned int)
        {
            ar & boost::serialization::base_object<UnionFind::Node<clump_id> >(*this);
            ar & vertices;
            ar & triangles;
        }
    };

    /**
     * Data to be written asynchronously to the temporary files. The interface is
     * similar to @c writev: @ref vertexRanges references ranges within @ref vertices
     * that must be written consecutively to the vertices temp file, and similarly for
     * @ref triangleRanges and @ref triangles.
     */
    struct TmpWriterItem
    {
        /// Backing store for vertices
        Statistics::Container::vector<vertex_type> vertices;
        /// Backing store for triangles
        Statistics::Container::vector<triangle_type> triangles;
        /**
         * Ranges of @ref vertices to write. Each range is of [first, last) form.
         */
        Statistics::Container::vector<std::pair<std::size_t, std::size_t> > vertexRanges;
        /**
         * Ranges of @ref triangles to write. Each range is of [first, last) form.
         */
        Statistics::Container::vector<std::pair<std::size_t, std::size_t> > triangleRanges;

        /// Allocation from the circular buffer for this item
        CircularBufferBase::Allocation alloc;

        TmpWriterItem();
    };

    class TmpWriterWorkerGroup;

    /**
     * Worker for asynchronous writes to the temporary files. These is only ever one of
     * these workers, so there are no race conditions.
     */
    class TmpWriterWorker : public WorkerBase
    {
    private:
        TmpWriterWorkerGroup &owner;   ///< Owning worker group
        std::ostream &verticesFile;    ///< File for temporary vertices
        std::ostream &trianglesFile;   ///< File for temporary triangles
    public:
        TmpWriterWorker(TmpWriterWorkerGroup &owner, std::ostream &verticesFile, std::ostream &trianglesFile)
            : WorkerBase("tmpwriter", 0),
            owner(owner), verticesFile(verticesFile), trianglesFile(trianglesFile) {}
        void operator()(TmpWriterItem &item);
    };

    /**
     * Asynchronous writing of data to the temporary files. This class manages
     * creation of the temporary files when it is started, but it does not
     * handle their removal once no longer needed. It does, however, close the
     * files when the group is stopped.
     *
     * Errors while writing the temporary files immediately terminate the program.
     */
    class TmpWriterWorkerGroup : public WorkerGroup<TmpWriterItem, TmpWriterWorker, TmpWriterWorkerGroup>
    {
        friend class ::TestTmpWriterWorkerGroup;
        friend class boost::serialization::access;
    private:
        /// File to which vertices are written
        boost::filesystem::ofstream verticesFile;
        /// File to which triangles are written
        boost::filesystem::ofstream trianglesFile;
        /// Filename for @ref verticesFile
        boost::filesystem::path verticesPath;
        /// Filename for @ref trianglesFile
        boost::filesystem::path trianglesPath;

        /// Allocator for items
        CircularBufferBase itemAllocator;
        /// Backing store of items
        std::vector<boost::shared_ptr<TmpWriterItem> > itemPool;

        template<typename Archive>
        void serialize(Archive &ar, const unsigned int)
        {
            ar & verticesPath;
            ar & trianglesPath;
        }
    public:
        /**
         * Constructor.
         */
        explicit TmpWriterWorkerGroup(std::size_t slots);

        /**
         * @copydoc WorkerGroup::start
         *
         * This also handles opening the temporary files.
         */
        void start();

        /**
         * Close the temporary files. This should not be called directly (it is called
         * by @ref WorkerGroup).
         */
        void stopPostJoin();

        boost::shared_ptr<TmpWriterItem> get(Timeplot::Worker &tworker, std::size_t size);

        void freeItem(boost::shared_ptr<TmpWriterItem> item);

        /**
         * Get the path to the temporary file for vertices. If @ref start has
         * not been called this will return an empty path.
         */
        const boost::filesystem::path &getVerticesPath() const { return verticesPath; }
        /**
         * Get the path to the temporary file for triangles. If @ref start has
         * not been called this will return an empty path.
         */
        const boost::filesystem::path &getTrianglesPath() const { return trianglesPath; }
    };

    // Needed to enable the curiously recursive template pattern
    friend class WorkerGroup<TmpWriterItem, TmpWriterWorker, TmpWriterWorkerGroup>;

private:
    /**
     * @name
     * @{
     * Temporary buffers.
     * These are stored in the object so that memory can be recycled if
     * possible, rather than thrashing the allocator.
     */
    Statistics::Container::vector<UnionFind::Node<std::tr1::int32_t> > tmpNodes;
    Statistics::Container::PODBuffer<clump_id> tmpClumpId;
    Statistics::Container::PODBuffer<std::tr1::uint32_t> tmpVertexLabel;
    Statistics::Container::PODBuffer<std::tr1::int32_t> tmpFirstVertex;
    Statistics::Container::PODBuffer<std::tr1::int32_t> tmpNextVertex;
    Statistics::Container::PODBuffer<std::tr1::int32_t> tmpFirstTriangle;
    Statistics::Container::PODBuffer<std::tr1::int32_t> tmpNextTriangle;
    /** @} */

    /// Total number of vertices written to temporary file
    std::tr1::uint64_t writtenVerticesTmp;
    /// Total number of triangles written to temporary file
    std::tr1::uint64_t writtenTrianglesTmp;

    /**
     * Reorder buffer. Initially only the vertices and triangles are placed
     * here. During @ref flushBuffer, the ranges to write are filled in from
     * the per-chunk information.
     *
     * This is initially a null pointer, and is also null immediately after a
     * call to @ref flushBuffer. Functions (including @ref write) must be
     * prepared to deal with this.
     */
    boost::shared_ptr<TmpWriterItem> reorderBuffer;

    Statistics::Container::vector<Clump> clumps;  ///< All clumps seen so far

    typedef Statistics::Container::unordered_map<cl_ulong, clump_id> clump_id_map_type;
    /// Maps external vertex keys to global clump IDs
    clump_id_map_type clumpIdMap;

    /**
     * Identifies components with a local set of triangles, and
     * returns a union-find tree for them.
     *
     * @param numVertices    Number of vertices indexed by @a triangles.
     *                       Also the size of the returned union-find tree.
     * @param numTriangles   Number of triangles in @a triangles.
     * @param triangles      The vertex indices of the triangles.
     * @param[out] nodes     A union-find tree over the vertices.
     */
    static void computeLocalComponents(
        std::size_t numVertices,
        std::size_t numTriangles,
        const triangle_type *triangles,
        Statistics::Container::vector<UnionFind::Node<std::tr1::int32_t> > &nodes);

    /**
     * Create global clumps from local union-find tree. The clumps are populated
     * with the appropriate vertex and triangle counts, but are not merged together
     * using shared external vertices.
     *
     * @param numTriangles   Number of triangles in @a triangles.
     * @param nodes          Union-find tree over the block vertices (see @ref computeLocalComponents).
     * @param triangles      Triangles in the block.
     * @param[out] clumpId   Clump IDs, one per vertex passed in.
     */
    void updateGlobalClumps(
        std::size_t numTriangles,
        const Statistics::Container::vector<UnionFind::Node<std::tr1::int32_t> > &nodes,
        const triangle_type *triangles,
        Statistics::Container::PODBuffer<clump_id> &clumpId);

    /**
     * Update @ref clumpIdMap and merge global clumps that share external vertices.
     *
     * @param numVertices    Total number of vertices in @a clumpId
     * @param numExternalVertices Number of external vertices in @a keys
     * @param keys           Vertex keys in the mesh.
     * @param clumpId        Vertex clump IDs computed by @ref updateGlobalClumps.
     *
     * Note that the internal vertices in @a clumpId are ignored, but must still be present.
     */
    void updateClumpKeyMap(
        std::size_t numVertices,
        std::size_t numExternalVertices,
        const cl_ulong *keys,
        const Statistics::Container::PODBuffer<clump_id> &clumpId);

    /**
     * Populate the per-chunk clump data and write the geometry to external
     * memory. This also does chunk-level welding to update @ref Chunk::vertexIdMap.
     *
     * @param chunk          The chunk to update.
     * @param globalClumpId  The clump IDs generated by @ref updateGlobalClumps
     * @param clumpIdFirst   Smallest value in @a globalClumpId
     * @param clumpIdLast    One greater than largest value in @a globalClumpId
     * @param mesh           The original data. All fields (vertices, triangles and keys)
     *                       must have finished loading.
     * @param tworker        Timeplot worker for recording interactions with the writer worker group
     */
    void updateLocalClumps(
        Chunk &chunk,
        const Statistics::Container::PODBuffer<clump_id> &globalClumpId,
        clump_id clumpIdFirst,
        clump_id clumpIdLast,
        HostKeyMesh &mesh,
        Timeplot::Worker &tworker);

    /**
     * Start async transfer any data in the reordering buffer to the temporary files.
     */
    void flushBuffer(Timeplot::Worker &tworker);

    /// Implementation of the functor
    void add(MesherWork &work, Timeplot::Worker &worker);

    /**
     * Serialize just enough data that @ref write can be run on the reconstituted structure
     */
    template<typename Archive>
    void serialize(Archive &ar, const unsigned int)
    {
        ar & tmpWriter;
        ar & chunks;
        ar & clumps;
    }

protected:
    /// If set to true, will not delete the temporary files
    bool retainFiles;

    /// Writer for temporary data
    TmpWriterWorkerGroup tmpWriter;

    /**
     * All chunks seen so far. This is indexed by the generation number in the
     * chunk ID. If non-contiguous IDs are found, there will be default-constructed
     * chunks plugging the holes.
     */
    Statistics::Container::vector<Chunk> chunks;

    /**
     * Flush out any temporary data to the temporary file writer then shut it down
     */
    void finalize(Timeplot::Worker &tworker);

    /**
     * Compute the number of components, vertices and triangles retained overall,
     * and update statistics. If @a record is true (the default), the statistics
     * registry will record the results. This is only called after all the geometry
     * has been received.
     *
     * @param[out] thresholdVertices  Number of vertices that must appear in a component
     *                                for it to appear in the output file (see @ref setPruneThreshold).
     * @param[out] keptComponents,keptVertices,keptTriangles     Number of
     *                                connected components / vertices / triangles that
     *                                passed the threshold test
     * @param record                  Whether to record statistics
     *
     */
    void getStatistics(
        std::tr1::uint64_t &thresholdVertices,
        clump_id &keptComponents,
        std::tr1::uint64_t &keptVertices,
        std::tr1::uint64_t &keptTriangles,
        bool record = true) const;

    /**
     * Compute the number of vertices and triangles retained for a chunk. This
     * is only called after all the geometry has been received.
     *
     * @param thresholdVertices Threshold for retaining components (see @ref getStatistics)
     * @param chunk             The chunk to evaluate
     * @param[out] keptVertices, keptTriangles Number of vertices/triangles that
     *                          will be in the output file
     * @param[out] totalExternal Total number of external vertices in the chunk,
     *                          including those that will not be in the output file
     *                          due to the threshold.
     */
    void getChunkStatistics(
        std::tr1::uint64_t thresholdVertices,
        const Chunk &chunk,
        std::tr1::uint64_t &keptVertices,
        std::tr1::uint64_t &keptTriangles,
        std::tr1::uint64_t &totalExternal) const;

    /**
     * Compute minimum number of bytes needed for the async writer. This is
     * only called after all the geometry has been received.
     *
     * @param thresholdVertices Threshold for retaining components (see @ref getStatistics)
     */
    std::size_t getAsyncMem(std::tr1::uint64_t thresholdVertices) const;

    /**
     * Transform triangles from their temporary file form to their output form.
     * Each output index is compared to @a externalBoundary. If it is greater
     * (indicating an external vertex), it is bit-wise negated then used as
     * an index in @a externalRemap. Otherwise, @a offset is added to it.
     *
     * @param numTriangles      Number of triangles to transform.
     * @param externalBoundary  Threshold for separating internal/external vertices
     * @param externalRemap     Maps external vertex indices to final indices
     * @param offset            Bias added to internal vertex indices
     * @param[in] inTriangles   Input triangles
     * @param[out] outTriangles Output triangles, in raw form for @ref FastPly::Writer
     */
    static void rewriteTriangles(
        std::size_t numTriangles,
        std::tr1::uint32_t externalBoundary,
        const std::tr1::uint32_t *externalRemap,
        std::tr1::uint32_t offset,
        const triangle_type *inTriangles,
        std::tr1::uint8_t *outTriangles);

    /**
     * Compute write positions and remapping table for one output chunk.
     *
     * @param chunk             Fully-defined output chunk
     * @param thresholdVertices Threshold for retaining components (see @ref getStatistics)
     * @param chunkExternal     Total number of external vertices in the chunk (see @ref getChunkStatistics)
     * @param[out] startVertex  Position in output file for vertices in each clump, as a vertex count (undefined for dropped clumps)
     * @param[out] startTriangle Position in output file for triangles each clump, as a triangle count (undefined for dropped clumps)
     * @param[out] externalRemap Maps external vertex indices to final indices
     */
    void writeChunkPrepare(
        const Chunk &chunk,
        std::tr1::uint64_t thresholdVertices,
        std::size_t chunkExternal,
        Statistics::Container::PODBuffer<std::tr1::uint32_t> &startVertex,
        Statistics::Container::PODBuffer<FastPly::Writer::size_type> &startTriangle,
        Statistics::Container::PODBuffer<std::tr1::uint32_t> &externalRemap);

    /**
     * Transfer clumps from the vertices temporary file to the output file.
     *
     * @param tworker           Worker to pass to @ref AsyncWriter::get
     * @param verticesTmpRead   Reader for the vertices temporary file
     * @param asyncWriter       Asynchronous writer to schedule through
     * @param chunk             Output chunk to write
     * @param thresholdVertices Threshold for retaining components (see @ref getStatistics)
     * @param startVertex       Position (in vertices) to start writing each clump (see @ref writeChunkPrepare)
     * @param progress          If non-NULL, updated with the number of triangles processed
     * @param firstClump, lastClump Range of clumps from the chunk to process.
     *
     * @note The progress meter is updated in triangles not vertices. This
     * avoids the need to worry about double-counting of external vertices.
     *
     * @pre @ref finalize has been called
     */
    void writeChunkVertices(
        Timeplot::Worker &tworker,
        BinaryReader &verticesTmpRead,
        AsyncWriter &asyncWriter,
        const Chunk &chunk,
        std::tr1::uint64_t thresholdVertices,
        const std::tr1::uint32_t *startVertex,
        ProgressMeter *progress,
        std::size_t firstClump, std::size_t lastClump);

    /**
     * Transfer clumps from the triangles temporary file to the output file.
     *
     * @param tworker           Worker to pass to @ref AsyncWriter::get
     * @param trianglesTmpRead  Reader for the triangles temporary file
     * @param asyncWriter       Asynchronous writer to schedule through
     * @param chunk             Output chunk to write
     * @param thresholdVertices Threshold for retaining components (see @ref getStatistics)
     * @param chunkExternal     Total number of external vertices for the chunk (see @ref getChunkStatistics)
     * @param startVertex       Position (in vertices) to start writing each
     *                          clump (see @ref writeChunkPrepare). This is
     *                          needed to adjust indices for internal vertices,
     *                          which are clump-relative in the input.
     * @param startTriangle     Position (in triangles) to start writing each clump
     * @param externalRemap     Maps external vertex indices to final indices
     * @param[in,out] triangles Temporary buffer the callee may use to hold data
     * @param progress          If non-NULL, updated with the number of triangles written
     * @param firstClump, lastClump Range of clumps to process
     *
     * @pre @ref finalize has been called
     */
    void writeChunkTriangles(
        Timeplot::Worker &tworker,
        BinaryReader &trianglesTmpRead,
        AsyncWriter &asyncWriter,
        const Chunk &chunk,
        std::tr1::uint64_t thresholdVertices,
        std::size_t chunkExternal,
        const std::tr1::uint32_t *startVertex,
        const FastPly::Writer::size_type *startTriangle,
        const std::tr1::uint32_t *externalRemap,
        Statistics::Container::PODBuffer<triangle_type> &triangles,
        ProgressMeter *progress,
        std::size_t firstClump, std::size_t lastClump);

public:
    /**
     * @copydoc MesherBase::MesherBase
     */
    OOCMesher(FastPly::Writer &writer, const Namer &namer);

    ~OOCMesher();

    virtual unsigned int numPasses() const { return 1; }
    virtual InputFunctor functor(unsigned int pass);
    virtual std::size_t write(Timeplot::Worker &tworker, std::ostream *progressStream = NULL);
    virtual void checkpoint(Timeplot::Worker &tworker, const boost::filesystem::path &path);
    virtual std::size_t resume(Timeplot::Worker &tworker, const boost::filesystem::path &path,
                               std::ostream *progressStream = NULL);
};

/**
 * Creates an adapter between @ref MesherBase::InputFunctor and @ref Marching::OutputFunctor
 * that reads the mesh from the device to the host synchronously.
 *
 * @param in        The mesher functor which will receive the host copy of the mesh.
 * @param chunkId   Chunk ID to pass to @a in.
 * @param tworker   The timeplot worker for the thread that will call the functor.
 */
Marching::OutputFunctor deviceMesher(const MesherBase::InputFunctor &in,
                                     const ChunkId &chunkId,
                                     Timeplot::Worker &tworker);

#endif /* !MESHER_H */
