/**
 * @file
 *
 * Data structures for storing the output of @ref Marching.
 *
 * The classes in this file are @ref MesherBase, an abstract base class, and
 * several concrete instantiations of it. They differ in terms of
 *  - the number of passes needed
 *  - the amount of temporary memory required.
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

/**
 * Enumeration of the supported mesher types
 */
enum MesherType
{
    OOC_MESHER
};

/**
 * Unique ID for an output file chunk. It consists of a @em generation number,
 * which is increased monotonically, and a set of @em coordinates which are used
 * to name the file.
 *
 * Comparison of generation numbers does not necessarily correspond to
 * lexicographical ordering of coordinates, but there is a one-to-one
 * relationship that is preserved across passes.
 */
struct ChunkId
{
    typedef std::tr1::uint32_t gen_type;

    /// Monotonically increasing generation number
    gen_type gen;
    /**
     * Chunk coordinates. The chunks form a regular grid and the coordinates
     * give the position within the grid, starting from (0,0,0).
     */
    boost::array<Grid::size_type, 3> coords;

    /// Default constructor (does zero initialization)
    ChunkId() : gen(0)
    {
        for (unsigned int i = 0; i < 3; i++)
            coords[i] = 0;
    }

    /// Comparison by generation number
    bool operator<(const ChunkId &b) const
    {
        return gen < b.gen;
    }
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
    MesherBase(FastPly::WriterBase &writer, const Namer &namer)
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
     * Performs any final file I/O.
     *
     * @param tworker         Timeplot worker for the current thread
     * @param progressStream  If non-NULL, a log stream for a progress meter.
     * @throw std::ios_base::failure on I/O failure (including failure to open the file).
     * @throw std::overflow_error if too many connected components were found.
     * @throw std::overflow_error if too many vertices were found in one output chunk.
     */
    virtual void write(Timeplot::Worker &tworker, std::ostream *progressStream = NULL) = 0;

protected:
    FastPly::WriterBase &getWriter() const { return writer; }
    std::string getOutputName(const ChunkId &id) const { return namer(id); }

private:
    /// Threshold set by @ref setPruneThreshold
    double pruneThreshold;
    /// Capacity set by @ref setReorderCapacity
    std::size_t reorderCapacity;

    FastPly::WriterBase &writer;   ///< Writer for output files
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
public:
    typedef boost::array<float, 3> vertex_type;
    typedef boost::array<cl_uint, 3> triangle_type;

private:
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
    };

    struct TmpWriterItem
    {
        Statistics::Container::vector<vertex_type> vertices;
        Statistics::Container::vector<triangle_type> triangles;
        Statistics::Container::vector<std::pair<std::size_t, std::size_t> > vertexRanges;
        Statistics::Container::vector<std::pair<std::size_t, std::size_t> > triangleRanges;

        TmpWriterItem();
    };

    class TmpWriterWorkerGroup;

    class TmpWriterWorker : public WorkerBase
    {
    private:
        TmpWriterWorkerGroup &owner;
        std::ostream &verticesFile;
        std::ostream &trianglesFile;
    public:
        TmpWriterWorker(TmpWriterWorkerGroup &owner, std::ostream &verticesFile, std::ostream &trianglesFile)
            : WorkerBase("tmpwriter", 0),
            owner(owner), verticesFile(verticesFile), trianglesFile(trianglesFile) {}
        void operator()(TmpWriterItem &item);
    };

    /**
     * Asynchronous writing of data to the temporary files.
     */
    class TmpWriterWorkerGroup : public WorkerGroup<TmpWriterItem, TmpWriterWorker, TmpWriterWorkerGroup>
    {
    private:
        boost::filesystem::ofstream verticesFile;
        boost::filesystem::ofstream trianglesFile;
        boost::filesystem::path verticesPath;
        boost::filesystem::path trianglesPath;
    public:
        explicit TmpWriterWorkerGroup(std::size_t spare);

        void start();
        void stopPostJoin();

        const boost::filesystem::path &getVerticesPath() const { return verticesPath; }
        const boost::filesystem::path &getTrianglesPath() const { return trianglesPath; }
    };

    friend class WorkerGroup<TmpWriterItem, TmpWriterWorker, TmpWriterWorkerGroup>;

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

    /// Writer for temporary data
    TmpWriterWorkerGroup tmpWriter;
    /// Total number of vertices written to temporary file
    std::tr1::uint64_t writtenVerticesTmp;
    /// Total number of triangles written to temporary file
    std::tr1::uint64_t writtenTrianglesTmp;

    /**
     * Reorder buffer. Initially only the vertices and triangles are placed
     * here. During @ref flushBuffer, the ranges to write are filled in from
     * the per-chunk information.
     */
    boost::shared_ptr<TmpWriterItem> reorderBuffer;

    /**
     * All chunks seen so far. This is indexed by the generation number in the
     * chunk ID. If non-contiguous IDs are found, there will be default-constructed
     * chunks plugging the holes.
     */
    Statistics::Container::vector<Chunk> chunks;
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
     * @param triangles      The vertex indices of the triangles.
     * @param[out] nodes     A union-find tree over the vertices.
     */
    static void computeLocalComponents(
        std::size_t numVertices,
        const Statistics::Container::vector<triangle_type> &triangles,
        Statistics::Container::vector<UnionFind::Node<std::tr1::int32_t> > &nodes);

    /**
     * Create global clumps from local union-find tree. The clumps are populated
     * with the appropriate vertex and triangle counts, but are not merged together
     * using shared external vertices.
     *
     * @param nodes          Union-find tree over the block vertices (see @ref computeLocalComponents).
     * @param triangles      Triangles in the block.
     * @param[out] clumpId   Clump IDs, one per vertex passed in.
     */
    void updateGlobalClumps(
        const Statistics::Container::vector<UnionFind::Node<std::tr1::int32_t> > &nodes,
        const Statistics::Container::vector<triangle_type> &triangles,
        Statistics::Container::PODBuffer<clump_id> &clumpId);

    /**
     * Update @ref clumpIdMap and merge global clumps that share external vertices.
     *
     * @param numVertices    Total number of vertices in @a clumpId
     * @param keys           Vertex keys in the mesh.
     * @param clumpId        Vertex clump IDs computed by @ref updateGlobalClumps.
     *
     * Note that the internal vertices in @a clumpId are ignored, but must still be present.
     */
    void updateClumpKeyMap(
        std::size_t numVertices,
        const Statistics::Container::vector<cl_ulong> &keys,
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
     * Transfer any data in the reordering buffer to the temporary files.
     */
    void flushBuffer(Timeplot::Worker &tworker);

    /// Implementation of the functor
    void add(MesherWork &work, Timeplot::Worker &worker);

public:
    /**
     * @copydoc MesherBase::MesherBase
     */
    OOCMesher(FastPly::WriterBase &writer, const Namer &namer)
        : MesherBase(writer, namer),
        tmpNodes("mem.OOCMesher::tmpNodes"),
        tmpClumpId("mem.OOCMesher::tmpClumpId"),
        tmpVertexLabel("mem.OOCMesher::tmpVertexLabel"),
        tmpFirstVertex("mem.OOCMesher::tmpFirstVertex"),
        tmpNextVertex("mem.OOCMesher::tmpNextVertex"),
        tmpFirstTriangle("mem.OOCMesher::tmpFirstTriangle"),
        tmpNextTriangle("mem.OOCMesher::tmpNextTriangle"),
        tmpWriter(2),
        chunks("mem.OOCMesher::chunks"),
        clumps("mem.OOCMesher::clumps"),
        clumpIdMap("mem.OOCMesher::clumpIdMap")
    {
    }

    ~OOCMesher();

    virtual unsigned int numPasses() const { return 1; }
    virtual InputFunctor functor(unsigned int pass);
    virtual void write(Timeplot::Worker &tworker, std::ostream *progressStream = NULL);
};

/**
 * Factory function to create a mesher of the specified type.
 *
 * @param writer, namer     Parameters to @ref MesherBase::MesherBase.
 * @param type              The type of mesher to create.
 */
MesherBase *createMesher(MesherType type, FastPly::WriterBase &writer, const MesherBase::Namer &namer);

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
