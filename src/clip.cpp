/**
 * @file
 *
 * Marching output functor for boundary clipping.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <CL/cl.hpp>
#include <boost/function.hpp>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include "errors.h"
#include "marching.h"
#include "clip.h"
#include "statistics.h"
#include "clh.h"

Clip::Clip(const cl::Context &context, const cl::Device &device,
           std::size_t maxVertices, std::size_t maxTriangles)
:
    maxVertices(maxVertices), maxTriangles(maxTriangles),
    distances(context, CL_MEM_READ_WRITE, maxVertices * sizeof(cl_float)),
    vertexCompact(context, CL_MEM_READ_WRITE, (maxVertices + 1) * sizeof(cl_uint)),
    triangleCompact(context, CL_MEM_READ_WRITE, (maxTriangles + 1) * sizeof(cl_uint)),
    compactScan(context, device, clogs::TYPE_UINT),
    outMesh(context, CL_MEM_READ_WRITE, maxVertices, 0, maxTriangles)
{
    std::vector<cl::Device> devices(1, device);
    program = CLH::build(context, devices, "kernels/clip.cl");
    vertexInitKernel = cl::Kernel(program, "vertexInit");
    classifyKernel = cl::Kernel(program, "classify");
    triangleCompactKernel = cl::Kernel(program, "triangleCompact");
    vertexCompactKernel = cl::Kernel(program, "vertexCompact");
}

void Clip::setDistanceFunctor(const DistanceFunctor &distanceFunctor)
{
    this->distanceFunctor = distanceFunctor;
}

void Clip::setOutput(const Marching::OutputFunctor &output)
{
    this->output = output;
}

void Clip::operator()(
    const cl::CommandQueue &queue,
    const DeviceKeyMesh &mesh,
    const std::vector<cl::Event> *events,
    cl::Event *event)
{
    MLSGPU_ASSERT(mesh.numVertices <= maxVertices, std::length_error);
    MLSGPU_ASSERT(mesh.numTriangles <= maxTriangles, std::length_error);

    cl::Event distanceEvent;
    distanceFunctor(queue, distances, mesh.vertices, mesh.numVertices, events, &distanceEvent);

    std::vector<cl::Event> wait;

    /* TODO:
     * - Pretty much every call needs to use an explicit work group size
     *   and have some manner to handle the leftover bits.
     * - Move setArg into constructor where feasible
     * - If interpolation is being done, probably need to split welding
     *   out of Marching and re-use it here.
     * - Change interface to pass in wait events, to avoid waitForEvents
     *   below.
     */

    /*** Classify and compact vertices and indices ***/

    cl::Event vertexInitEvent;
    vertexInitKernel.setArg(0, vertexCompact);
    queue.enqueueNDRangeKernel(vertexInitKernel,
                               cl::NullRange,
                               cl::NDRange(mesh.numVertices),
                               cl::NullRange,
                               NULL, &vertexInitEvent);

    wait.resize(2);
    wait[0] = distanceEvent;
    wait[1] = vertexInitEvent;
    cl::Event classifyEvent;
    classifyKernel.setArg(0, triangleCompact);
    classifyKernel.setArg(1, vertexCompact);
    classifyKernel.setArg(2, mesh.triangles);
    classifyKernel.setArg(3, distances);
    queue.enqueueNDRangeKernel(classifyKernel,
                               cl::NullRange,
                               cl::NDRange(mesh.numTriangles),
                               cl::NullRange,
                               &wait, &classifyEvent);

    /*** Compact vertices and their keys ***/

    wait.resize(1);
    wait[0] = classifyEvent;
    cl::Event vertexScanEvent;
    compactScan.enqueue(queue, vertexCompact, mesh.numVertices + 1, NULL, &wait, &vertexScanEvent);

    wait.resize(1);
    wait[0] = vertexScanEvent;
    cl_uint vertexCount = 0, internalVertexCount = 0;
    cl::Event vertexCountEvent, internalVertexCountEvent;
    queue.enqueueReadBuffer(vertexCompact, CL_FALSE,
                            mesh.numInternalVertices * sizeof(cl_uint), sizeof(cl_uint),
                            &internalVertexCount, &wait, &internalVertexCountEvent);
    queue.enqueueReadBuffer(vertexCompact, CL_FALSE,
                            mesh.numVertices * sizeof(cl_uint), sizeof(cl_uint),
                            &vertexCount, &wait, &vertexCountEvent);

    wait.resize(1);
    wait[0] = vertexScanEvent;
    cl::Event vertexCompactEvent;
    vertexCompactKernel.setArg(0, outMesh.vertices);
    vertexCompactKernel.setArg(1, outMesh.vertexKeys);
    vertexCompactKernel.setArg(2, vertexCompact);
    vertexCompactKernel.setArg(3, mesh.vertices);
    vertexCompactKernel.setArg(4, mesh.vertexKeys);
    queue.enqueueNDRangeKernel(vertexCompactKernel,
                               cl::NullRange,
                               cl::NDRange(mesh.numVertices),
                               cl::NullRange,
                               &wait, &vertexCompactEvent);

    /*** Compact triangles, while also rewriting the indices ***/

    wait.resize(1);
    wait[0] = classifyEvent;
    cl::Event triangleScanEvent;
    compactScan.enqueue(queue, triangleCompact, mesh.numTriangles + 1, NULL, &wait, &triangleScanEvent);

    wait.resize(1);
    wait[0] = triangleScanEvent;
    cl::Event triangleCountEvent;
    cl_uint triangleCount = 0;
    queue.enqueueReadBuffer(triangleCompact, CL_FALSE,
                            mesh.numTriangles * sizeof(cl_uint), sizeof(cl_uint),
                            &triangleCount, &wait, &triangleCountEvent);

    wait.resize(2);
    wait[0] = triangleScanEvent;
    wait[1] = vertexScanEvent;
    cl::Event triangleCompactEvent;
    triangleCompactKernel.setArg(0, outMesh.triangles);
    triangleCompactKernel.setArg(1, triangleCompact);
    triangleCompactKernel.setArg(2, mesh.triangles);
    triangleCompactKernel.setArg(3, vertexCompact);
    queue.enqueueNDRangeKernel(triangleCompactKernel,
                               cl::NullRange,
                               cl::NDRange(mesh.numTriangles),
                               cl::NullRange,
                               &wait, &triangleCompactEvent);

    // Some of these happen-after others and so some steps are redundant, but
    // checking for all of them is safe.
    wait.resize(3);
    wait[0] = internalVertexCountEvent;
    wait[1] = vertexCountEvent;
    wait[2] = triangleCountEvent;
    queue.flush();
    cl::Event::waitForEvents(wait);

    wait.resize(2);
    wait[0] = vertexCompactEvent;
    wait[1] = triangleCompactEvent;
    if (vertexCount > 0)
    {
        outMesh.numVertices = vertexCount;
        outMesh.numInternalVertices = internalVertexCount;
        outMesh.numTriangles = triangleCount;
        output(queue, outMesh, &wait, event);
    }
    else if (event != NULL)
        CLH::enqueueMarkerWithWaitList(queue, &wait, event);
}
