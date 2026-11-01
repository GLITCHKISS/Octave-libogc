#include "WaveGrid.h"

#include <math.h>

#include "Log.h"
#include "Vertex.h"
#include "Assets/StaticMesh.h"
#include "Nodes/Node.h"
#include "Nodes/3D/StaticMesh3d.h"
#include "Graphics/GraphicsTypes.h"

#if API_GX
#include <gccore.h>
#endif

static const float kTwoPi = 6.28318531f;

static float Smooth01(float t)
{
    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

// sin(x) for any x: reduced to -pi..pi, then a parabola corrected (error under 0.1%) -- not newlib's sinf, which is
// slow on the GameCube (and slower still past ~200 radians)
static float FastSin(float x)
{
    x = x - kTwoPi * floorf(x * (1.0f / kTwoPi) + 0.5f);
    const float y = 1.27323954f * x - 0.405284735f * x * (x < 0.0f ? -x : x);
    return 0.225f * (y * (y < 0.0f ? -y : y) - y) + y;
}

// 1 / sqrt(x): on the GameCube its estimate and one Newton step, ~0.1% off (the Gekko has no square root
// instruction: sqrtf is a slow library call)
static float RSqrt(float x)
{
#if API_GX
    float y;
    __asm__("frsqrte %0,%1" : "=f"(y) : "f"(x));
    return y * (1.5f - 0.5f * x * y * y);
#else
    return 1.0f / sqrtf(x);
#endif
}

WaveGrid::WaveGrid(const WaveGridDesc& desc)
    : mDesc(desc)
{
    if (mDesc.mStep <= 0.0f) mDesc.mStep = 1.0f;
    if (mDesc.mMaxX < mDesc.mMinX) mDesc.mMaxX = mDesc.mMinX;
    if (mDesc.mMaxZ < mDesc.mMinZ) mDesc.mMaxZ = mDesc.mMinZ;
    mColumns = (uint32_t)floorf((mDesc.mMaxX - mDesc.mMinX) / mDesc.mStep + 0.5f) + 1;
    mRows = (uint32_t)floorf((mDesc.mMaxZ - mDesc.mMinZ) / mDesc.mStep + 0.5f) + 1;
    const uint32_t n = mColumns * mRows;
    mGridX.resize(n); mGridZ.resize(n); mFade.resize(n); mHeight.assign(n, 0.0f);
    mNormal.assign(n, glm::vec3(0.0f, 1.0f, 0.0f));
    for (uint32_t row = 0; row < mRows; ++row)
    {
        for (uint32_t col = 0; col < mColumns; ++col)
        {
            const uint32_t i = row * mColumns + col;
            const float x = mDesc.mMinX + col * mDesc.mStep, z = mDesc.mMinZ + row * mDesc.mStep;
            mGridX[i] = x;
            mGridZ[i] = z;
            float d = x - mDesc.mMinX;
            d = (mDesc.mMaxX - x < d) ? mDesc.mMaxX - x : d;
            d = (z - mDesc.mMinZ < d) ? z - mDesc.mMinZ : d;
            d = (mDesc.mMaxZ - z < d) ? mDesc.mMaxZ - z : d;
            mFade[i] = (mDesc.mEdgeFade > 0.0f) ? Smooth01(d / mDesc.mEdgeFade) : 1.0f;
        }
    }
    mRings.reserve(mDesc.mMaxRings);
}

void WaveGrid::AddWave(float dirX, float dirZ, float wavelength, float height, float speed, float phase)
{
    const float len = sqrtf(dirX * dirX + dirZ * dirZ);
    if (len <= 0.0f || wavelength <= 0.0f)
    {
        return;
    }
    Wave w;
    w.mDirX = dirX / len;
    w.mDirZ = dirZ / len;
    w.mK = kTwoPi / wavelength;
    w.mOmega = sqrtf(9.81f * w.mK) * speed;
    w.mAmp = height;
    w.mPhase = phase;
    w.mTime = 0.0f;
    mWaves.push_back(w);
    mSpatialDirty = true;
}

void WaveGrid::BuildSpatial()
{
    const uint32_t n = mColumns * mRows, nw = (uint32_t)mWaves.size();
    mSpatial.resize(n * nw * 2);
    for (uint32_t i = 0; i < n; ++i)
    {
        for (uint32_t w = 0; w < nw; ++w)
        {
            const Wave& wv = mWaves[w];
            const float place = wv.mK * (wv.mDirX * mGridX[i] + wv.mDirZ * mGridZ[i]) + wv.mPhase;
            mSpatial[(i * nw + w) * 2 + 0] = mFade[i] * wv.mAmp * sinf(place);
            mSpatial[(i * nw + w) * 2 + 1] = mFade[i] * wv.mAmp * cosf(place);
        }
    }
    mSpatialDirty = false;
}

bool WaveGrid::AddLayer(StaticMesh3D* nodeA, StaticMesh3D* nodeB, bool lit)
{
    if (nodeA == nullptr || nodeB == nullptr || nodeA == nodeB)
    {
        LogError("WaveGrid: a layer needs two nodes");
        return false;
    }
    Layer l;
    l.mMesh[0] = nodeA->GetStaticMesh();
    l.mMesh[1] = nodeB->GetStaticMesh();
    const uint32_t n = mColumns * mRows;
    if (l.mMesh[0] == nullptr || l.mMesh[1] == nullptr || l.mMesh[0] == l.mMesh[1] ||
        l.mMesh[0]->HasVertexColor() || l.mMesh[1]->HasVertexColor() ||
        l.mMesh[0]->GetVertices() == nullptr || l.mMesh[1]->GetVertices() == nullptr)
    {
        LogError("WaveGrid: %s / %s aren't two copies of a mesh (each its own, no vertex colours)",
                 nodeA->GetName().c_str(), nodeB->GetName().c_str());
        return false;
    }
#if API_GX && !EDITOR
    for (StaticMesh* mesh : l.mMesh)
    {
        if (mesh->GetResource()->mQuantized || mesh->GetResource()->mCompact)
        {
            LogError("WaveGrid: %s was made 16-bit or compact as it loaded: keep it out (GFX_SetQuantizeFilter)",
                     mesh->GetName().c_str());
            return false;
        }
    }
#endif
    for (StaticMesh* mesh : l.mMesh)
    {
        if (mesh->GetNumVertices() != n)
        {
            LogError("WaveGrid: %s has %u vertices, not the grid's %u (%u x %u)", mesh->GetName().c_str(),
                     mesh->GetNumVertices(), n, mColumns, mRows);
            return false;
        }
        const Vertex* v = mesh->GetVertices();
        for (uint32_t i = 0; i < n; ++i)
        {
            if (fabsf(v[i].mPosition.x - mGridX[i]) > mDesc.mStep * 0.25f ||
                fabsf(v[i].mPosition.z - mGridZ[i]) > mDesc.mStep * 0.25f)
            {
                LogError("WaveGrid: %s's vertices aren't the grid's points in order (vertex %u)", mesh->GetName().c_str(), i);
                return false;
            }
        }
    }
    l.mNode[0] = ResolveWeakPtr<Node>(nodeA);
    l.mNode[1] = ResolveWeakPtr<Node>(nodeB);
    l.mBaseY = l.mMesh[0]->GetVertices()[0].mPosition.y;
    l.mLit = lit;
    nodeA->SetVisible(true);
    nodeB->SetVisible(false);
    mLayers.push_back(l);
    return true;
}

void WaveGrid::Ring(float x, float z, float strength)
{
    if (mDesc.mMaxRings == 0 || x < mDesc.mMinX || x > mDesc.mMaxX || z < mDesc.mMinZ || z > mDesc.mMaxZ)
    {
        return;
    }
    RingData ring = { x, z, 0.0f, mDesc.mRingHeight * strength };
    if (mRings.size() < mDesc.mMaxRings)
    {
        mRings.push_back(ring);
    }
    else
    {
        size_t oldest = 0;
        for (size_t r = 1; r < mRings.size(); ++r) if (mRings[r].mAge > mRings[oldest].mAge) oldest = r;
        mRings[oldest] = ring;
    }
}

float WaveGrid::HeightAt(float x, float z) const
{
    const float gx = (x - mDesc.mMinX) / mDesc.mStep, gz = (z - mDesc.mMinZ) / mDesc.mStep;
    if (gx < 0.0f || gz < 0.0f || gx >= (float)(mColumns - 1) || gz >= (float)(mRows - 1))
    {
        return 0.0f;
    }
    const uint32_t c = (uint32_t)gx, r = (uint32_t)gz;
    const float s = gx - c, t = gz - r;
    const float* p = &mHeight[r * mColumns + c];
    return (p[0] * (1.0f - s) + p[1] * s) * (1.0f - t) + (p[mColumns] * (1.0f - s) + p[mColumns + 1] * s) * t;
}

void WaveGrid::Update(float dt)
{
    if (dt > 0.1f) dt = 0.1f;
    if (mSpatialDirty) BuildSpatial();
    const uint32_t n = mColumns * mRows, nw = (uint32_t)mWaves.size();

    // the swell: sin(place - time) = sin(place) cos(time) - cos(place) sin(time), its per-point part worked out once
    // (BuildSpatial): one sinf and cosf a wave a frame, a few multiply-adds a point
    float cosT[16], sinT[16];
    const uint32_t waves = nw < 16 ? nw : 16;
    for (uint32_t w = 0; w < waves; ++w)
    {
        Wave& wv = mWaves[w];
        wv.mTime += wv.mOmega * dt;
        if (wv.mTime > kTwoPi) wv.mTime -= kTwoPi * floorf(wv.mTime / kTwoPi);   // (kept within a turn)
        cosT[w] = cosf(wv.mTime);
        sinT[w] = sinf(wv.mTime);
    }
    // (the usual counts written out, no loop over the waves inside the loop over the points: on the GameCube that
    //  loop's counting and indexing cost about as much as the sums -- ~0.07 ms a frame over a couple of thousand points)
    const float* sp = mSpatial.data();
    float* height = mHeight.data();
    switch (waves)
    {
    case 0:
        for (uint32_t i = 0; i < n; ++i) height[i] = 0.0f;
        break;
    case 1:
        for (uint32_t i = 0; i < n; ++i, sp += 2)
            height[i] = sp[0] * cosT[0] - sp[1] * sinT[0];
        break;
    case 2:
        for (uint32_t i = 0; i < n; ++i, sp += 4)
            height[i] = sp[0] * cosT[0] - sp[1] * sinT[0] + sp[2] * cosT[1] - sp[3] * sinT[1];
        break;
    case 3:
        for (uint32_t i = 0; i < n; ++i, sp += 6)
            height[i] = sp[0] * cosT[0] - sp[1] * sinT[0] + sp[2] * cosT[1] - sp[3] * sinT[1] +
                        sp[4] * cosT[2] - sp[5] * sinT[2];
        break;
    case 4:
        for (uint32_t i = 0; i < n; ++i, sp += 8)
            height[i] = sp[0] * cosT[0] - sp[1] * sinT[0] + sp[2] * cosT[1] - sp[3] * sinT[1] +
                        sp[4] * cosT[2] - sp[5] * sinT[2] + sp[6] * cosT[3] - sp[7] * sinT[3];
        break;
    default:
        for (uint32_t i = 0; i < n; ++i, sp += nw * 2)
        {
            float h = 0.0f;
            for (uint32_t w = 0; w < waves; ++w) h += sp[w * 2] * cosT[w] - sp[w * 2 + 1] * sinT[w];
            height[i] = h;
        }
        break;
    }

    // the rings: each over the points it can reach (the square round it), those within its train of its middle
    // (mRingSpeed * age out) lifted by a wave under a smooth bump, dying with age
    const float k = kTwoPi / mDesc.mRingWavelength, width = mDesc.mRingWidth;
    const float invWidth2 = 1.0f / (width * width);
    for (size_t r = 0; r < mRings.size(); )
    {
        RingData& ring = mRings[r];
        ring.mAge += dt;
        const float life = expf(-ring.mAge / mDesc.mRingLife);
        if (life < mDesc.mRingCutoff)
        {
            mRings[r] = mRings.back();
            mRings.pop_back();
            continue;
        }
        const float amp = ring.mAmp * life;
        const float mid = mDesc.mRingSpeed * ring.mAge;
        const float lo = (mid > width) ? mid - width : 0.0f, hi = mid + width;
        const float lo2 = lo * lo, hi2 = hi * hi;
        int c0 = (int)floorf((ring.mX - hi - mDesc.mMinX) / mDesc.mStep), c1 = (int)ceilf((ring.mX + hi - mDesc.mMinX) / mDesc.mStep);
        int r0 = (int)floorf((ring.mZ - hi - mDesc.mMinZ) / mDesc.mStep), r1 = (int)ceilf((ring.mZ + hi - mDesc.mMinZ) / mDesc.mStep);
        c0 = c0 < 0 ? 0 : c0; c1 = c1 > (int)mColumns - 1 ? (int)mColumns - 1 : c1;
        r0 = r0 < 0 ? 0 : r0; r1 = r1 > (int)mRows - 1 ? (int)mRows - 1 : r1;
        for (int row = r0; row <= r1; ++row)
        {
            for (int col = c0; col <= c1; ++col)
            {
                const uint32_t i = row * mColumns + col;
                const float dx = mGridX[i] - ring.mX, dz = mGridZ[i] - ring.mZ;
                const float d2 = dx * dx + dz * dz;
                if (d2 < lo2 || d2 > hi2) continue;
                const float off = ((d2 > 1e-6f) ? d2 * RSqrt(d2) : 0.0f) - mid;
                float bump = 1.0f - (off * off) * invWidth2;
                bump *= bump;
                mHeight[i] += mFade[i] * amp * bump * FastSin(k * off);
            }
        }
        ++r;
    }

    // the normals (for lit layers): the slope across from the neighbours either side (one side at the rim),
    // (-dh/dx, 1, -dh/dz) made unit length -- the GPU blends the lighting across each triangle from them: smooth
    bool anyLit = false;
    for (const Layer& l : mLayers) anyLit = anyLit || l.mLit;
    if (anyLit)
    {
        const float gain = mDesc.mShadeGain, step = mDesc.mStep;
        for (uint32_t row = 0; row < mRows; ++row)
        {
            const uint32_t r0 = (row > 0) ? row - 1 : row, r1 = (row + 1 < mRows) ? row + 1 : row;
            const float invZ = (r1 > r0) ? gain / ((r1 - r0) * step) : 0.0f;
            for (uint32_t col = 0; col < mColumns; ++col)
            {
                const uint32_t c0 = (col > 0) ? col - 1 : col, c1 = (col + 1 < mColumns) ? col + 1 : col;
                const float invX = (c1 > c0) ? gain / ((c1 - c0) * step) : 0.0f;
                const float sx = (mHeight[row * mColumns + c1] - mHeight[row * mColumns + c0]) * invX;
                const float sz = (mHeight[r1 * mColumns + col] - mHeight[r0 * mColumns + col]) * invZ;
                const float inv = RSqrt(sx * sx + sz * sz + 1.0f);
                mNormal[row * mColumns + col] = glm::vec3(-sx * inv, inv, -sz * inv);
            }
        }
    }

    // each layer's copy not drawn last frame: written, sent to the GPU, shown; the other hidden
    const uint32_t copy = mFrame & 1;
    for (Layer& l : mLayers)
    {
        Node* shown = l.mNode[copy].Get();
        Node* hidden = l.mNode[copy ^ 1].Get();
        if (shown == nullptr || hidden == nullptr)
        {
            continue;                           // (a node gone)
        }
        Vertex* v = l.mMesh[copy]->GetVertices();
        for (uint32_t i = 0; i < n; ++i)
        {
            v[i].mPosition.y = l.mBaseY + mHeight[i];
        }
        if (l.mLit)
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                v[i].mNormal = mNormal[i];
            }
        }
#if API_GX && !EDITOR
        DCFlushRange(v, n * sizeof(Vertex));    // (the GPU's vertex cache cleared once, below, for every layer)
#else
        l.mMesh[copy]->UpdateVertexResource();
#endif
        shown->SetVisible(true);
        hidden->SetVisible(false);
    }
#if API_GX && !EDITOR
    GX_InvVtxCache();
#endif
    ++mFrame;
}
