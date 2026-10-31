#pragma once

#include <stdint.h>
#include <vector>

#include "Maths.h"
#include "SmartPointer.h"

class Node;
class StaticMesh;
class StaticMesh3D;

// A water surface moved on the CPU: a flat grid of points (x across, z along, every mStep), each frame raised and
// lowered by a swell (sines across the water, summed) and by rings spreading from points (each a short train of waves
// riding out, dying away), faded to nothing at the grid's rim so the water never pulls away from its walls. The heights
// are written into the vertices of the meshes given (AddLayer): each a grid mesh of exactly these points, in order,
// row by row (z), each row x ascending.
//
// The GPU draws one frame while the CPU works on the next, so each layer is two nodes with their own copies of the
// mesh: each frame the copy not drawn last frame is written and shown, the other hidden. (On GameCube each written copy
// is flushed from the CPU's cache and the GPU's vertex cache cleared once for them all; elsewhere
// StaticMesh::UpdateVertexResource sends it up.) On GameCube the meshes must stay float vertices: keep them out of GFX_SetQuantizedMeshes
// (GFX_SetQuantizeFilter), or AddLayer refuses them.
//
// The arithmetic is kept cheap for the GameCube: one sinf and cosf a wave a frame (the swell's per-point part worked
// out once), no sqrtf (the Gekko's reciprocal square root estimate and a Newton step), each ring only over the
// points it can reach.
struct WaveGridDesc
{
    float mMinX = 0.0f, mMaxX = 1.0f;
    float mMinZ = 0.0f, mMaxZ = 1.0f;
    float mStep = 1.0f;                 // between the grid's points
    float mEdgeFade = 0.75f;            // in from the rim to full height
    float mShadeGain = 1.0f;            // the slope the normals see, of the real one (more: waves shaded deeper)
    float mRingSpeed = 2.0f;            // a second the waves ride out
    float mRingWavelength = 3.0f;       // between a ring's crests
    float mRingWidth = 3.0f;            // either side of a ring's middle it reaches
    float mRingLife = 2.5f;             // s for a ring to die to a third (e^-1)
    float mRingHeight = 0.3f;           // a full ring's (Ring's strength 1)
    float mRingCutoff = 0.04f;          // a ring dropped once this faint
    uint32_t mMaxRings = 6;
};

class WaveGrid
{
public:

    explicit WaveGrid(const WaveGridDesc& desc);

    // A sine across the water: its direction (x, z), wavelength, height, speed (1: deep water's for the wavelength)
    // and phase. Summed with the others.
    void AddWave(float dirX, float dirZ, float wavelength, float height, float speed, float phase);

    // A layer: two nodes showing their own copies of a grid mesh (see above); lit, its normals written too (smooth
    // shading). False, with the reason logged, if the meshes don't fit.
    bool AddLayer(StaticMesh3D* nodeA, StaticMesh3D* nodeB, bool lit);

    // A ring spreading from (x, z), strength 1 a full one (off the grid: none). With all out, it takes the oldest's
    // place.
    void Ring(float x, float z, float strength = 1.0f);

    // The water's height at (x, z) over its rest, as drawn this frame (between the points blended); 0 off the grid.
    float HeightAt(float x, float z) const;

    // A frame: the heights, normals and the next copy of each layer written and shown.
    void Update(float deltaTime);

    uint32_t GetNumColumns() const { return mColumns; }
    uint32_t GetNumRows() const { return mRows; }

private:

    struct Wave { float mDirX, mDirZ, mK, mOmega, mAmp, mPhase, mTime; };
    struct RingData { float mX, mZ, mAge, mAmp; };
    struct Layer
    {
        WeakPtr<Node> mNode[2];
        StaticMesh* mMesh[2] = { nullptr, nullptr };
        float mBaseY = 0.0f;
        bool mLit = false;
    };

    void BuildSpatial();

    WaveGridDesc mDesc;
    uint32_t mColumns = 0, mRows = 0;
    std::vector<float> mGridX, mGridZ, mFade, mHeight;
    std::vector<glm::vec3> mNormal;
    std::vector<Wave> mWaves;
    std::vector<float> mSpatial;        // each point's fade * height * sin, cos of its place along each wave
    bool mSpatialDirty = true;
    std::vector<RingData> mRings;
    std::vector<Layer> mLayers;
    uint32_t mFrame = 0;
};
