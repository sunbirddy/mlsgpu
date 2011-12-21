#define NUM_EDGES 19

__constant sampler_t nearest = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

inline uint makeCode(const float iso[8])
{
    return (iso[0] >= 0.0f ? 0x01U : 0U)
        | (iso[1] >= 0.0f ? 0x02U : 0U)
        | (iso[2] >= 0.0f ? 0x04U : 0U)
        | (iso[3] >= 0.0f ? 0x08U : 0U)
        | (iso[4] >= 0.0f ? 0x10U : 0U)
        | (iso[5] >= 0.0f ? 0x20U : 0U)
        | (iso[6] >= 0.0f ? 0x40U : 0U)
        | (iso[7] >= 0.0f ? 0x80U : 0U);
}

inline bool isValid(const float iso[8])
{
    return isfinite(iso[0])
        && isfinite(iso[1])
        && isfinite(iso[2])
        && isfinite(iso[3])
        && isfinite(iso[4])
        && isfinite(iso[5])
        && isfinite(iso[6])
        && isfinite(iso[7]);
}

__kernel void countOccupied(
    __global uint *occupied,
    __read_only image2d_t isoA,
    __read_only image2d_t isoB)
{
    uint2 gid = (uint2) (get_global_id(0), get_global_id(1));
    uint linearId = gid.y * get_global_size(0) + gid.x;

    float iso[8];
    iso[0] = read_imagef(isoA, nearest, convert_int2(gid + (uint2) (0, 0))).x;
    iso[1] = read_imagef(isoA, nearest, convert_int2(gid + (uint2) (1, 0))).x;
    iso[2] = read_imagef(isoA, nearest, convert_int2(gid + (uint2) (0, 1))).x;
    iso[3] = read_imagef(isoA, nearest, convert_int2(gid + (uint2) (1, 1))).x;
    iso[4] = read_imagef(isoB, nearest, convert_int2(gid + (uint2) (0, 0))).x;
    iso[5] = read_imagef(isoB, nearest, convert_int2(gid + (uint2) (1, 0))).x;
    iso[6] = read_imagef(isoB, nearest, convert_int2(gid + (uint2) (0, 1))).x;
    iso[7] = read_imagef(isoB, nearest, convert_int2(gid + (uint2) (1, 1))).x;

    uint code = makeCode(iso);
    bool valid = isValid(iso);
    occupied[linearId] = (valid && code != 0 && code != 255);
}

__kernel void compact(
    __global uint2 * restrict cells,
    __global const uint * restrict occupiedRemap)
{
    uint2 gid = (uint2) (get_global_id(0), get_global_id(1));
    uint linearId = gid.y * get_global_size(0) + gid.x;

    uint pos = occupiedRemap[linearId];
    uint next = occupiedRemap[linearId + 1];
    if (next != pos)
        cells[pos] = gid;
}

__kernel void countElements(
    __global uint2 * restrict viCount,
    __global const uint2 * restrict cells,
    __read_only image2d_t isoA,
    __read_only image2d_t isoB,
    __global const uchar2 * restrict countTable)
{
    uint gid = get_global_id(0);
    uint2 cell = cells[gid];

    float iso[8];
    iso[0] = read_imagef(isoA, nearest, convert_int2(cell + (uint2) (0, 0))).x;
    iso[1] = read_imagef(isoA, nearest, convert_int2(cell + (uint2) (1, 0))).x;
    iso[2] = read_imagef(isoA, nearest, convert_int2(cell + (uint2) (0, 1))).x;
    iso[3] = read_imagef(isoA, nearest, convert_int2(cell + (uint2) (1, 1))).x;
    iso[4] = read_imagef(isoB, nearest, convert_int2(cell + (uint2) (0, 0))).x;
    iso[5] = read_imagef(isoB, nearest, convert_int2(cell + (uint2) (1, 0))).x;
    iso[6] = read_imagef(isoB, nearest, convert_int2(cell + (uint2) (0, 1))).x;
    iso[7] = read_imagef(isoB, nearest, convert_int2(cell + (uint2) (1, 1))).x;

    uint code = makeCode(iso);
    viCount[gid] = convert_uint2(countTable[code]);
}

inline float3 interp(float iso0, float iso1, float3 cell, float3 offset0, float3 offset1, float3 scale, float3 bias)
{
    float inv = 1.0f / (iso0 - iso1);
    float3 lcoord = cell + (iso1 * offset0 - iso0 * offset1) * inv;
    return fma(lcoord, scale, bias);
}

#define INTERP(a, b) \
    interp(iso[a], iso[b], cellf, (float3) (a & 1, (a >> 1) & 1, (a >> 2) & 1), (float3) (b & 1, (b >> 1) & 1, (b >> 2) & 1), scale, bias)

__kernel void generateElements(
    __global float3 *vertices,
    __global uint *indices,
    __global const uint2 * restrict viStart,
    __global const uint2 * restrict cells,
    __read_only image2d_t isoA,
    __read_only image2d_t isoB,
    __global const ushort2 * restrict startTable,
    __global const uchar * restrict dataTable,
    uint z,
    float3 scale,
    float3 bias,
    __local float3 *lvertices)
{
    const uint gid = get_global_id(0);
    const uint lid = get_local_id(0);
    uint3 cell;
    cell.xy = cells[gid];
    cell.z = z;
    const float3 cellf = convert_float3(cell);
    __local float3 *lverts = lvertices + NUM_EDGES * lid;

    float iso[8];
    iso[0] = read_imagef(isoA, nearest, convert_int2(cell.xy + (uint2) (0, 0))).x;
    iso[1] = read_imagef(isoA, nearest, convert_int2(cell.xy + (uint2) (1, 0))).x;
    iso[2] = read_imagef(isoA, nearest, convert_int2(cell.xy + (uint2) (0, 1))).x;
    iso[3] = read_imagef(isoA, nearest, convert_int2(cell.xy + (uint2) (1, 1))).x;
    iso[4] = read_imagef(isoB, nearest, convert_int2(cell.xy + (uint2) (0, 0))).x;
    iso[5] = read_imagef(isoB, nearest, convert_int2(cell.xy + (uint2) (1, 0))).x;
    iso[6] = read_imagef(isoB, nearest, convert_int2(cell.xy + (uint2) (0, 1))).x;
    iso[7] = read_imagef(isoB, nearest, convert_int2(cell.xy + (uint2) (1, 1))).x;

    lverts[0] = INTERP(0, 1);
    lverts[1] = INTERP(0, 2);
    lverts[2] = INTERP(0, 3);
    lverts[3] = INTERP(1, 3);
    lverts[4] = INTERP(2, 3);
    lverts[5] = INTERP(0, 4);
    lverts[6] = INTERP(0, 5);
    lverts[7] = INTERP(1, 5);
    lverts[8] = INTERP(4, 5);
    lverts[9] = INTERP(0, 6);
    lverts[10] = INTERP(2, 6);
    lverts[11] = INTERP(4, 6);
    lverts[12] = INTERP(0, 7);
    lverts[13] = INTERP(1, 7);
    lverts[14] = INTERP(2, 7);
    lverts[15] = INTERP(3, 7);
    lverts[16] = INTERP(4, 7);
    lverts[17] = INTERP(5, 7);
    lverts[18] = INTERP(6, 7);

    uint code = makeCode(iso);
    uint2 viNext = viStart[gid];
    uint vNext = viNext.s0;
    uint iNext = viNext.s1;

    ushort2 start = startTable[code];
    ushort2 end = startTable[code + 1];

    for (uint i = 0; i < end.x - start.x; i++)
    {
        vertices[vNext + i] = lverts[dataTable[start.x + i]];
    }
    for (uint i = 0; i < end.y - start.y; i++)
    {
        indices[iNext + i] = vNext + dataTable[start.y + i];
    }
}
