/**
 * @file
 *
 * Marching tetrahedra algorithm.
 */

#ifndef MARCHING_H
#define MARCHING_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <CL/cl.hpp>
#include <cstddef>
#include <vector>
#include <boost/function.hpp>
#include <clcpp/clcpp.h>

class TestMarching;

class Marching
{
    friend class TestMarching;
private:
    static const unsigned int NUM_CUBES = 256;      ///< Number of possible vertex codes for a cube (2^vertices)
    static const unsigned int NUM_EDGES = 19;       ///< Number of edges in each cube
    static const unsigned int NUM_TETRAHEDRA = 6;   ///< Number of tetrahedra in each cube

    /**
     * The vertices incident on each edge. It is important that the vertex indices
     * are in order in each edge.
     */
    static const unsigned char edgeIndices[NUM_EDGES][2];

    /**
     * The vertices of each tetrahedron in a cube. The vertices must be wound
     * consistently such that the first three appear counter-clockwise when
     * viewed from the fourth in a right-handed coordinate system.
     */
    static const unsigned char tetrahedronIndices[NUM_TETRAHEDRA][4];

    std::size_t width, height, depth;

    cl::Context context;

    /**
     * Buffer of uchar2 values, indexed by cube code. The two elements are
     * the number of vertices and indices generated by the cell.
     */
    cl::Buffer countTable;
    /**
     * Buffer of ushort2 values, indexed by cube code. The two elements are
     * the positions of the index array and vertex array in @ref dDataTable.
     * It has one extra element at the end so that the element range for
     * the last cube code can be found.
     */
    cl::Buffer startTable;
    /**
     * Buffer of uchar values, which are either indices to be emitted
     * (after biasing), or vertices represented as an edge ID. The range
     * of vertices or indices for a particular cube code is determined by
     * two adjacent elements of @ref dCountTable.
     */
    cl::Buffer dataTable;

    cl::Buffer viCount;
    cl::Buffer cells;
    cl::Buffer occupied;

    cl::Image2D backingImages[2];
    cl::Image2D *images[2];

    cl::Program program;
    cl::Kernel countOccupiedKernel;
    cl::Kernel compactKernel;
    cl::Kernel countElementsKernel;
    cl::Kernel generateElementsKernel;

    clcpp::Scan scanOccupied;
    clcpp::Scan scanElements;

    /**
     * Finds the edge incident on vertices v0 and v1.
     *
     * @pre edge (v0, v1) is one of the existing edges
     */
    static unsigned int findEdgeByVertexIds(unsigned int v0, unsigned int v1);

    /**
     * Determines the parity of a permutation.
     *
     * The permutation can contain any unique values - they do not need to be
     * 0..n-1. It is considered to be the permutation that would map the
     * sorted sequence to the given sequence.
     *
     * @param first, last The range to measure (forward iterators)
     * @retval 0 if the permutation contains an odd number of swaps
     * @retval 1 if the permutation contains an even number of swaps.
     */
    template<typename Iterator>
    static unsigned int permutationParity(Iterator first, Iterator last);

    void makeTables();

public:
    typedef boost::function<void(const cl::CommandQueue &, const cl::Image2D &, cl_uint z, const std::vector<cl::Event> *, cl::Event *)> Functor;

    Marching(const cl::Context &context, const cl::Device &device,
             std::size_t width, std::size_t height, std::size_t depth);

    void enqueue(const cl::CommandQueue &queue, const Functor &functor,
                 const cl_float3 &gridScale, const cl_float3 &gridBias,
                 cl::Buffer &vertices, cl::Buffer &indices,
                 cl_uint2 *totals,
                 const std::vector<cl::Event> *events,
                 cl::Event *event);
};

#endif /* !MARCHING_H */
