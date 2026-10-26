#include "Nodes/3D/SkeletalMesh3d.h"
#include "Nodes/3D/StaticMesh3d.h"
#include "Assets/SkeletalMesh.h"
#include "Renderer.h"
#include "AssetManager.h"
#include "Log.h"
#include "Maths.h"
#include "Utilities.h"

#include "Graphics/Graphics.h"
#if API_GX
#include "Graphics/GX/GxUtils.h"
#endif

static const char* sBoneInfluenceModeStrings[] =
{
    "One Bone",
    "Four Bones"
};
static_assert(int32_t(BoneInfluenceMode::Num) == 2, "Need to update string conversion table");

static const char* sAnimationUpdateModeStrings[] =
{
    "Always Update",
    "Always Update Time",
    "Only When Rendered"
};
static_assert(int32_t(AnimationUpdateMode::Count) == 3, "Need to update string conversion table");

FORCE_LINK_DEF(SkeletalMesh3D);
DEFINE_NODE(SkeletalMesh3D, Mesh3D);

// (16: a game can layer more than 8 -- a character's 8 arm layers with its walk's 5 over them)
#define NUM_ANIMATION_SLOTS 16

struct DecompTransform
{
    glm::vec3 mPosition = { 0.0f, 0.0f, 0.0f };
    glm::quat mRotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    glm::vec3 mScale = { 1.0f, 1.0f, 1.0f };
    bool mValid = false;
};

bool SkeletalMesh3D::HandlePropChange(Datum* datum, uint32_t index, const void* newValue)
{
    Property* prop = static_cast<Property*>(datum);
    OCT_ASSERT(prop != nullptr);
    SkeletalMesh3D* meshComp = static_cast<SkeletalMesh3D*>(prop->mOwner);
    bool success = false;

    if (prop->mName == "Skeletal Mesh")
    {
        meshComp->SetSkeletalMesh(*(SkeletalMesh**) newValue);
        success = true;
    }
    else if (prop->mName == "Default Animation")
    {
        meshComp->StopAllAnimations();
        meshComp->mDefaultAnimation = *((std::string*) newValue);
        meshComp->PlayAnimation(meshComp->mDefaultAnimation.c_str(), true, 1.0f, 1.0f, 0);
        success = true;
    }

    return success;
}

static SkeletalMesh* GetDefaultMesh()
{
    return LoadAsset<SkeletalMesh>("SK_EighthNote");
}

SkeletalMesh3D::SkeletalMesh3D() :
    mSkeletalMesh(nullptr),
    mAnimationSpeed(1.0f),
    mAnimationPaused(false),
    mRevertToBindPose(true),
    mInheritPose(false),
    mHasAnimatedThisFrame(false),
    mBoneInfluenceMode(BoneInfluenceMode::Four),
    mAnimationUpdateMode(AnimationUpdateMode::OnlyUpdateWhenRendered)
{
    mName = "Skeletal Mesh";
    mCollisionEnabled = false;
    mReceiveSimpleShadows = false;
}

SkeletalMesh3D::~SkeletalMesh3D()
{

}

const char* SkeletalMesh3D::GetTypeName() const
{
    return "SkeletalMesh";
}

void SkeletalMesh3D::GatherProperties(std::vector<Property>& outProps)
{
    Mesh3D::GatherProperties(outProps);

    SCOPED_CATEGORY("Mesh");

    outProps.push_back(Property(DatumType::Asset, "Skeletal Mesh", this, &mSkeletalMesh, 1, HandlePropChange, int32_t(SkeletalMesh::GetStaticType())));
    outProps.push_back(Property(DatumType::String, "Default Animation", this, &mDefaultAnimation, 1, HandlePropChange));
    outProps.push_back(Property(DatumType::Float, "Animation Speed", this, &mAnimationSpeed));
    outProps.push_back(Property(DatumType::Bool, "Animation Paused", this, &mAnimationPaused));
    outProps.push_back(Property(DatumType::Bool, "Revert To Bind Pose", this, &mRevertToBindPose));
    outProps.push_back(Property(DatumType::Bool, "Inherit Pose", this, &mInheritPose));
    outProps.push_back(Property(DatumType::Integer, "Bone Influence Mode", this, &mBoneInfluenceMode, 1, nullptr, NULL_DATUM, (int32_t)BoneInfluenceMode::Num, sBoneInfluenceModeStrings));
    outProps.push_back(Property(DatumType::Integer, "Animation Update Mode", this, &mAnimationUpdateMode, 1, nullptr, NULL_DATUM, (int32_t)AnimationUpdateMode::Count, sAnimationUpdateModeStrings));
    outProps.push_back(Property(DatumType::Float, "Bounds Radius Override", this, &mBoundsRadiusOverride));
}

void SkeletalMesh3D::Create()
{
    Mesh3D::Create();
    GFX_CreateSkeletalMeshCompResource(this);
    SetSkeletalMesh(GetDefaultMesh());

    if (mDefaultAnimation != "")
    {
        PlayAnimation(mDefaultAnimation.c_str(), true, 1.0f, 1.0f, 0);
    }
}

void SkeletalMesh3D::Destroy()
{
    GFX_DestroySkeletalMeshCompResource(this);

    Mesh3D::Destroy();
}

SkeletalMeshCompResource* SkeletalMesh3D::GetResource()
{
    return &mResource;
}

void SkeletalMesh3D::Tick(float deltaTime)
{
    Mesh3D::Tick(deltaTime);
    TickCommon(deltaTime);
}

void SkeletalMesh3D::EditorTick(float deltaTime)
{
    Mesh3D::EditorTick(deltaTime);
    TickCommon(deltaTime);
}

void SkeletalMesh3D::TickCommon(float deltaTime)
{
    mHasAnimatedThisFrame = false;
}

bool SkeletalMesh3D::IsStaticMesh3D() const
{
    return false;
}

bool SkeletalMesh3D::IsSkeletalMesh3D() const
{
    return true;
}

void SkeletalMesh3D::SetSkeletalMesh(SkeletalMesh* skeletalMesh)
{
    if (mSkeletalMesh.Get() != skeletalMesh)
    {
        mSkeletalMesh = skeletalMesh;

        if (skeletalMesh != nullptr)
        {
            if (GFX_IsCpuSkinningRequired(this))
            {
                // Initialize skinned vertex array
                mSkinnedVertices.resize(skeletalMesh->GetNumVertices());
                const std::vector<VertexSkinned> srcVerts = skeletalMesh->GetVertices();
                for (uint32_t i = 0; i < mSkinnedVertices.size(); ++i)
                {
                    mSkinnedVertices[i].mPosition = srcVerts[i].mPosition;
                    mSkinnedVertices[i].mNormal = srcVerts[i].mNormal;
                    mSkinnedVertices[i].mTexcoord0 = srcVerts[i].mTexcoord0;
                    mSkinnedVertices[i].mTexcoord1 = srcVerts[i].mTexcoord1;
                }

                GFX_ReallocateSkeletalMeshCompVertexBuffer(this, skeletalMesh->GetNumVertices());
                GFX_UpdateSkeletalMeshCompVertexBuffer(this, mSkinnedVertices);
            }

            // Resize and clear matrices to identity matrix.
            mBoneMatrices.resize(skeletalMesh->GetNumBones(), glm::mat4(1));
        }
        else
        {
            mBoneMatrices.resize(0);
        }
    }
}

SkeletalMesh* SkeletalMesh3D::GetSkeletalMesh()
{
    return mSkeletalMesh.Get<SkeletalMesh>();
}

void SkeletalMesh3D::PlayAnimation(const char* animName, bool loop, float speed, float weight, int32_t slot)
{
    if (animName == nullptr ||
        animName[0] == '\0')
    {
        return;
    }

    if (slot >= NUM_ANIMATION_SLOTS)
    {
        LogWarning("Invalid animation slot in SkeletalMesh3D::PlayAnimation()");
        return;
    }

    // If a slot of -1 is passed in (or any negative number), that means, make it the next highest available slot.
    if (slot < 0)
    {
        for (uint32_t i = 0; i < mActiveAnimations.size(); ++i)
        {
            OCT_ASSERT(mActiveAnimations[i].mSlot >= 0);
            slot = glm::max<int32_t>(slot, mActiveAnimations[i].mSlot);
        }

        slot++;
        OCT_ASSERT(slot >= 0);
    }

    slot = glm::clamp<int32_t>(slot, 0, NUM_ANIMATION_SLOTS - 1);

    ActiveAnimation newAnimData;
    newAnimData.mName = animName;
    newAnimData.mTime = 0.0;
    newAnimData.mSpeed = speed;
    newAnimData.mWeight = weight;
    newAnimData.mLoop = loop;
    newAnimData.mSlot = slot;

    ActiveAnimation* anim = FindActiveAnimation(animName);

    if (anim != nullptr)
    {
        // Preserve the active animation time if looping
        newAnimData.mTime = loop ? anim->mTime : 0.0f;

        // The slot changed. We need to remove it from the active anim list
        // and then reinsert it to ensure that the array remains in ascending slot.
        if (anim->mSlot != slot)
        {
            for (uint32_t i = 0; i < mActiveAnimations.size(); ++i)
            {
                if (mActiveAnimations[i].mName == animName)
                {
                    mActiveAnimations.erase(mActiveAnimations.begin() + i);
                    break;
                }
            }

            anim = nullptr;
        }
    }

    if (mActiveAnimations.size() >= NUM_ANIMATION_SLOTS)
    {
        LogWarning("Too many active animations. Cannot play animation.");
        return;
    }

    if (anim == nullptr)
    {
        // Animation isn't playing, we need to insert it.
        for (uint32_t i = 0; i < mActiveAnimations.size(); ++i)
        {
            if (mActiveAnimations[i].mSlot == slot)
            {
                // There is a different animation in the same slot. Overwrite that animation.
                anim = &mActiveAnimations[i];
                break;
            }

            if (mActiveAnimations[i].mSlot > slot)
            {
                mActiveAnimations.insert(mActiveAnimations.begin() + i, ActiveAnimation());
                anim = &mActiveAnimations[i];
                break;
            }
        }

        if (anim == nullptr)
        {
            // This is the highest slot anim, place it on the end
            mActiveAnimations.push_back(ActiveAnimation());
            anim = &mActiveAnimations.back();
        }
    }

    OCT_ASSERT(anim != nullptr);
    if (anim != nullptr)
    {
        *anim = newAnimData;
    }
}

void SkeletalMesh3D::QueueAnimation(const char* animName, bool loop, const char* targetAnim, float speed, float weight, int32_t slot)
{
    if (animName == nullptr ||
        animName[0] == '\0')
    {
        return;
    }

    ActiveAnimation* dependentAnim = nullptr;
    QueuedAnimation* dependentQueuedAnim = nullptr;

    if (targetAnim == nullptr)
    {
        if (mActiveAnimations.size() == 0)
        {
            // Default queued, just play it immediately.
            PlayAnimation(animName, loop, speed, weight, slot);
        }
        else
        {
            dependentAnim = &mActiveAnimations.back();
        }
    }
    else
    {
        dependentAnim = FindActiveAnimation(targetAnim);
    }

    // Check queued anims for dependent anim (so you could make a chain of animations if you needed).
    if (dependentAnim == nullptr)
    {
        dependentQueuedAnim = FindQueuedAnimation(targetAnim);
    }

    if (dependentAnim != nullptr ||
        dependentQueuedAnim != nullptr)
    {
        QueuedAnimation* queuedAnim = nullptr;

        {
            // dependName may be invalid if vector resize happens
            const char* dependName = dependentAnim ? dependentAnim->mName.c_str() : dependentQueuedAnim->mName.c_str();
            queuedAnim = FindQueuedAnimation(animName, dependName);
        }

        if (queuedAnim == nullptr)
        {
            // No queuedAnim exists yet so create a new one.
            mQueuedAnimations.push_back(QueuedAnimation());
            queuedAnim = &(mQueuedAnimations.back());
        }

        OCT_ASSERT(queuedAnim);

        queuedAnim->mName = animName;
        queuedAnim->mDependentAnim = dependentAnim ? dependentAnim->mName : dependentQueuedAnim->mName;
        queuedAnim->mLoop = loop;
        queuedAnim->mSpeed = speed;
        queuedAnim->mWeight = weight;
        queuedAnim->mSlot = slot;
    }
}

void SkeletalMesh3D::StopAnimation(const char* animName, bool cancelQueued)
{
    for (uint32_t i = 0; i < mActiveAnimations.size(); ++i)
    {
        if (mActiveAnimations[i].mName == animName)
        {
            mActiveAnimations.erase(mActiveAnimations.begin() + i);
            break;
        }
    }

    if (cancelQueued)
    {
        // I think it's possible for multiple queued animations to exist with different dependent animations
        // So just to be safe, iterate through all queued vector.
        for (int32_t i = int32_t(mQueuedAnimations.size()) - 1; i >= 0; --i)
        {
            if (mQueuedAnimations[i].mName == animName)
            {
                mQueuedAnimations.erase(mQueuedAnimations.begin() + i);
            }
        }
    }
}

void SkeletalMesh3D::StopAllAnimations(bool cancelQueued)
{
    mActiveAnimations.clear();

    if (cancelQueued)
    {
        mQueuedAnimations.clear();
    }
}

void SkeletalMesh3D::CancelQueuedAnimation(const char* animName)
{
    for (int32_t i = int32_t(mQueuedAnimations.size()) - 1; i >= 0; --i)
    {
        if (mQueuedAnimations[i].mName == animName)
        {
            mQueuedAnimations.erase(mQueuedAnimations.begin() + i);
        }
    }
}

void SkeletalMesh3D::CancelAllQueuedAnimations()
{
    mQueuedAnimations.clear();
}

bool SkeletalMesh3D::IsAnimationPlaying(const char* animName)
{
    return (FindActiveAnimation(animName) != nullptr);
}

void SkeletalMesh3D::ResetAnimation()
{
    // Not sure what a "Reset" function should do? Will probably remove this
    for (uint32_t i = 0; i < mActiveAnimations.size(); ++i)
    {
        mActiveAnimations[i].mTime = 0.0f;
    }
}

float SkeletalMesh3D::GetAnimationSpeed() const
{
    return mAnimationSpeed;
}

void SkeletalMesh3D::SetAnimationSpeed(float speed)
{
    mAnimationSpeed = speed;
}

void SkeletalMesh3D::SetAnimationPaused(bool paused)
{
    mAnimationPaused = paused;
}

bool SkeletalMesh3D::IsAnimationPaused() const
{
    return mAnimationPaused;
}

void SkeletalMesh3D::SetInheritPose(bool inherit)
{
    mInheritPose = inherit;
}

bool SkeletalMesh3D::IsInheritPoseEnabled() const
{
    return mInheritPose;
}

void SkeletalMesh3D::SetBoundsRadiusOverride(float radius)
{
    mBoundsRadiusOverride = radius;
}

float SkeletalMesh3D::GetBoundsRadiusOverride() const
{
    return mBoundsRadiusOverride;
}

bool SkeletalMesh3D::HasAnimatedThisFrame() const
{
    return mHasAnimatedThisFrame;
}

ActiveAnimation* SkeletalMesh3D::FindActiveAnimation(const char* animName)
{
    ActiveAnimation* anim = nullptr;

    for (uint32_t i = 0; i < mActiveAnimations.size(); ++i)
    {
        if (mActiveAnimations[i].mName == animName)
        {
            anim = &mActiveAnimations[i];
            break;
        }
    }

    return anim;
}

std::vector<ActiveAnimation>& SkeletalMesh3D::GetActiveAnimations()
{
    return mActiveAnimations;
}

QueuedAnimation* SkeletalMesh3D::FindQueuedAnimation(const char* animName, const char* dependName)
{
    QueuedAnimation* anim = nullptr;

    for (uint32_t i = 0; i < mQueuedAnimations.size(); ++i)
    {
        if (mQueuedAnimations[i].mName == animName &&
            (dependName == nullptr || mQueuedAnimations[i].mDependentAnim == dependName))
        {
            anim = &mQueuedAnimations[i];
            break;
        }
    }

    return anim;
}

std::vector<QueuedAnimation>& SkeletalMesh3D::GetQueuedAnimations()
{
    return mQueuedAnimations;
}

int32_t SkeletalMesh3D::FindBoneIndex(const std::string& name) const
{
    int32_t boneIndex = -1;
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();

    if (mesh != nullptr)
    {
        boneIndex = mesh->FindBoneIndex(name);
    }

    return boneIndex;
}

glm::vec3 SkeletalMesh3D::GetBoneRestPosition(int32_t boneIndex) const
{
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    if (mesh == nullptr || boneIndex < 0 || boneIndex >= (int32_t)mesh->GetNumBones())
        return glm::vec3(0.0f);

    glm::mat4 rest = mesh->GetInvRootTransform() * mesh->GetBone(boneIndex).mInvOffsetMatrix;
    return glm::vec3(rest[3]);
}

int32_t SkeletalMesh3D::AddBoneChain(const std::string& rootBone, float stiffness, float damping, glm::vec3 gravity, float radius,
    float inertia, float limit)
{
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    int32_t root = FindBoneIndex(rootBone);
    if (mesh == nullptr || root < 0)
    {
        LogWarning("AddBoneChain: no bone %s", rootBone.c_str());
        return -1;
    }

    // down from the root while each bone has exactly one child
    const std::vector<Bone>& bones = mesh->GetBones();
    BoneChain chain;
    for (int32_t b = root; b != -1; )
    {
        chain.mBones.push_back(b);
        int32_t next = -1, children = 0;
        for (uint32_t i = 0; i < bones.size(); ++i)
        {
            if (bones[i].mParentIndex == b)
            {
                next = (int32_t)i;
                ++children;
            }
        }
        b = (children == 1) ? next : -1;
    }

    // the last bone's tip: on along the line by the length of the bone before it
    int32_t last = chain.mBones.back();
    glm::vec3 lastHead = GetBoneRestPosition(last);
    glm::vec3 step = (chain.mBones.size() > 1) ? lastHead - GetBoneRestPosition(chain.mBones[chain.mBones.size() - 2]) : glm::vec3(0.0f, -0.1f, 0.0f);
    glm::mat4 lastRest = mesh->GetInvRootTransform() * bones[last].mInvOffsetMatrix;
    chain.mTipLocal = glm::vec3(glm::inverse(lastRest) * glm::vec4(lastHead + step, 1.0f));

    chain.mStiffness = glm::clamp(stiffness, 0.0f, 1.0f);
    chain.mDamping = glm::clamp(damping, 0.0f, 1.0f);
    chain.mGravity = gravity;
    chain.mRadius = glm::max(radius, 0.0f);
    chain.mInertia = glm::max(inertia, 0.0f);
    chain.mLimit = glm::max(limit, 0.0f);
    mBoneChains.push_back(chain);
    return (int32_t)mBoneChains.size() - 1;
}

int32_t SkeletalMesh3D::AddBoneCollider(const std::string& bone, glm::vec3 restA, glm::vec3 restB, float radius)
{
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    int32_t index = FindBoneIndex(bone);
    if (mesh == nullptr || index < 0)
    {
        LogWarning("AddBoneCollider: no bone %s", bone.c_str());
        return -1;
    }

    glm::mat4 toLocal = glm::inverse(mesh->GetInvRootTransform() * mesh->GetBone(index).mInvOffsetMatrix);
    BoneCollider col;
    col.mBone = index;
    col.mA = glm::vec3(toLocal * glm::vec4(restA, 1.0f));
    col.mB = glm::vec3(toLocal * glm::vec4(restB, 1.0f));
    col.mRadius = glm::max(radius, 0.0f);
    mBoneColliders.push_back(col);
    return (int32_t)mBoneColliders.size() - 1;
}

void SkeletalMesh3D::SetBoneColliderRadius(int32_t index, float radius)
{
    if (index >= 0 && index < (int32_t)mBoneColliders.size())
        mBoneColliders[index].mRadius = glm::max(radius, 0.0f);
}

void SkeletalMesh3D::ResetBoneDynamics()
{
    for (BoneChain& chain : mBoneChains)
        chain.mReset = true;
}

bool SkeletalMesh3D::AddBoneChainLink(const std::string& rootA, const std::string& rootB)
{
    int32_t boneA = FindBoneIndex(rootA), boneB = FindBoneIndex(rootB);
    int32_t a = -1, b = -1;
    for (uint32_t i = 0; i < mBoneChains.size(); ++i)
    {
        if (!mBoneChains[i].mBones.empty() && mBoneChains[i].mBones[0] == boneA) a = (int32_t)i;
        if (!mBoneChains[i].mBones.empty() && mBoneChains[i].mBones[0] == boneB) b = (int32_t)i;
    }
    if (a < 0 || b < 0 || a == b)
    {
        LogWarning("AddBoneChainLink: no chains from %s and %s", rootA.c_str(), rootB.c_str());
        return false;
    }
    mBoneChainLinks.push_back({ a, b });
    return true;
}

void SkeletalMesh3D::ClearBoneDynamics()
{
    mBoneChains.clear();
    mBoneColliders.clear();
    mBoneChainLinks.clear();
}

// the nearest points of segments p1-q1 and p2-q2: s along the first, t along the second (0..1)
static void ClosestOnSegments(const glm::vec3& p1, const glm::vec3& q1, const glm::vec3& p2, const glm::vec3& q2, float& s, float& t)
{
    const glm::vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    const float eps = 0.00000001f;
    s = t = 0.0f;
    if (a <= eps && e <= eps)
        return;
    if (a <= eps)
    {
        t = glm::clamp(f / e, 0.0f, 1.0f);
        return;
    }
    const float c = glm::dot(d1, r);
    if (e <= eps)
    {
        s = glm::clamp(-c / a, 0.0f, 1.0f);
        return;
    }
    const float b = glm::dot(d1, d2), denom = a * e - b * b;
    s = (denom > eps) ? glm::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
    t = (b * s + f) / e;
    if (t < 0.0f)
    {
        t = 0.0f;
        s = glm::clamp(-c / a, 0.0f, 1.0f);
    }
    else if (t > 1.0f)
    {
        t = 1.0f;
        s = glm::clamp((b - c) / a, 0.0f, 1.0f);
    }
}

// pushes the segment p-q off a capsule (reach from its line a-b), its ends moved by how near the contact is to each;
// p pinned (not moving), only q moves, scaled up by how far along the contact is. Returns whether it touched
static bool PushSegmentOut(glm::vec3& p, glm::vec3& q, const glm::vec3& a, const glm::vec3& b, float reach, bool pMoves)
{
    float s, u;
    ClosestOnSegments(p, q, a, b, s, u);
    glm::vec3 away = glm::mix(p, q, s) - glm::mix(a, b, u);
    float dist = glm::length(away);
    if (dist >= reach || dist < 0.00001f)
        return false;

    glm::vec3 push = away * ((reach - dist) / dist);
    if (!pMoves)
    {
        if (s < 0.2f)                                       // (at the pinned end itself: nothing to move)
            return false;
        q += push / s;
    }
    else
    {
        float w = 1.0f / ((1.0f - s) * (1.0f - s) + s * s);
        p += push * ((1.0f - s) * w);
        q += push * (s * w);
    }
    return true;
}

void SkeletalMesh3D::SimulateBoneDynamics(float deltaTime)
{
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    if (mBoneChains.empty() || mesh == nullptr || mBoneMatrices.size() != mesh->GetNumBones())
        return;

    const std::vector<Bone>& bones = mesh->GetBones();
    const float dt = glm::min(deltaTime, 1.0f / 30.0f);      // (a hitch doesn't fling it)
    const float frames = dt * 60.0f;
    const float scale = glm::length(glm::vec3(mTransform[0]));

    // the colliders where the animation has them this frame, world space
    static std::vector<glm::vec3> sColA, sColB;
    sColA.resize(mBoneColliders.size());
    sColB.resize(mBoneColliders.size());
    for (uint32_t c = 0; c < mBoneColliders.size(); ++c)
    {
        const BoneCollider& col = mBoneColliders[c];
        glm::mat4 world = mTransform * mBoneMatrices[col.mBone] * bones[col.mBone].mInvOffsetMatrix;
        sColA[c] = glm::vec3(world * glm::vec4(col.mA, 1.0f));
        sColB[c] = glm::vec3(world * glm::vec4(col.mB, 1.0f));
    }

    glm::mat4 invTransform = glm::inverse(mTransform);

    // each chain moved on by itself first -- all of them: the links across them want every chain stepped before
    // they're kept off the colliders together
    for (BoneChain& chain : mBoneChains)
    {
        const uint32_t n = (uint32_t)chain.mBones.size();
        const uint32_t numPoints = n + 1;
        std::vector<glm::vec3>& target = chain.mTarget;

        // where the animation puts each joint (and the tip)
        chain.mModel.resize(n);
        target.resize(numPoints);
        for (uint32_t k = 0; k < n; ++k)
        {
            int32_t b = chain.mBones[k];
            chain.mModel[k] = mBoneMatrices[b] * bones[b].mInvOffsetMatrix;
            target[k] = glm::vec3(mTransform * chain.mModel[k][3]);
        }
        target[n] = glm::vec3(mTransform * chain.mModel[n - 1] * glm::vec4(chain.mTipLocal, 1.0f));
        chain.mLength.resize(numPoints);
        for (uint32_t i = 1; i < numPoints; ++i)
            chain.mLength[i] = glm::distance(target[i - 1], target[i]);

        if (chain.mReset ||
            chain.mOffset.size() != numPoints ||
            glm::distance(chain.mLastTarget[0], target[0]) > 1.0f)    // (teleported)
        {
            chain.mPos = target;
            chain.mOffset.assign(numPoints, glm::vec3(0.0f));
            chain.mPrevOffset.assign(numPoints, glm::vec3(0.0f));
            chain.mLastTarget = target;
            chain.mLastMove.assign(numPoints, glm::vec3(0.0f));
            chain.mHasMove = false;
            chain.mReset = false;
        }

        if (dt > 0.0f)
        {
            const float keep = powf(1.0f - chain.mDamping, frames);
            const float pull = 1.0f - powf(1.0f - chain.mStiffness, frames);
            const glm::vec3 fall = chain.mGravity * dt * dt;

            for (uint32_t i = 0; i < numPoints; ++i)
            {
                // the animation's move this frame, and how much it changed from the last (its acceleration: what
                // throws the chain -- none the first frame, nothing to compare with)
                glm::vec3 move = target[i] - chain.mLastTarget[i];
                glm::vec3 accel = chain.mHasMove ? (move - chain.mLastMove[i]) : glm::vec3(0.0f);
                chain.mLastMove[i] = move;
                chain.mLastTarget[i] = target[i];

                glm::vec3 offset = glm::vec3(0.0f);
                if (i > 0)
                {
                    offset = chain.mOffset[i] + (chain.mOffset[i] - chain.mPrevOffset[i]) * keep - accel * chain.mInertia + fall;
                    offset *= (1.0f - pull);
                }
                chain.mPos[i] = target[i] + offset;
            }
            chain.mHasMove = true;
        }
        else
        {
            for (uint32_t i = 0; i < numPoints; ++i)
                chain.mPos[i] = target[i] + chain.mOffset[i];
        }
    }

    if (dt > 0.0f)
    {
        // each pass: the lengths and the limit; the links across off the colliders; then each line off them -- the
        // colliders' the last word (the links last, their pushes left lines in his shoulders)
        for (uint32_t iter = 0; iter < 4; ++iter)
        {
            for (BoneChain& chain : mBoneChains)
            {
                const uint32_t numPoints = (uint32_t)chain.mBones.size() + 1;
                const std::vector<glm::vec3>& target = chain.mTarget;

                // lengths kept, from the root out
                for (uint32_t i = 1; i < numPoints; ++i)
                {
                    glm::vec3 d = chain.mPos[i] - chain.mPos[i - 1];
                    float len = glm::length(d);
                    if (len > 0.0001f)
                        chain.mPos[i] = chain.mPos[i - 1] + d * (chain.mLength[i] / len);
                }

                // within the limit of the animation (the tip the whole of it)
                if (chain.mLimit > 0.0f)
                {
                    for (uint32_t i = 1; i < numPoints; ++i)
                    {
                        float most = chain.mLimit * scale * float(i) / float(numPoints - 1);
                        glm::vec3 d = chain.mPos[i] - target[i];
                        float len = glm::length(d);
                        if (len > most)
                            chain.mPos[i] = target[i] + d * (most / len);
                    }
                }
            }

            // the links across, joint to joint below the pinned roots, and corner to corner across each panel
            // between them: the cloth between two lines kept off the colliders too -- a collider come up between
            // two lines met neither, and the cloth went through it; between two links across, through a panel's middle
            for (const std::pair<int32_t, int32_t>& link : mBoneChainLinks)
            {
                BoneChain& a = mBoneChains[link.first];
                BoneChain& b = mBoneChains[link.second];
                const uint32_t numPoints = (uint32_t)glm::min(a.mPos.size(), b.mPos.size());
                for (uint32_t c = 0; c < mBoneColliders.size(); ++c)
                {
                    float reach = (mBoneColliders[c].mRadius + 0.5f * (a.mRadius + b.mRadius)) * scale;
                    for (uint32_t i = 1; i < numPoints; ++i)
                    {
                        PushSegmentOut(a.mPos[i], b.mPos[i], sColA[c], sColB[c], reach, true);
                        if (i + 1 < numPoints)
                        {
                            PushSegmentOut(a.mPos[i], b.mPos[i + 1], sColA[c], sColB[c], reach, true);
                            PushSegmentOut(b.mPos[i], a.mPos[i + 1], sColA[c], sColB[c], reach, true);
                        }
                    }
                }
            }

            for (BoneChain& chain : mBoneChains)
            {
                const uint32_t numPoints = (uint32_t)chain.mBones.size() + 1;

                // out of the colliders: the whole of each segment, not only its joints (a collider can come up
                // between two), its two ends moved by how near the contact is to each -- the root's end stays
                for (uint32_t i = 1; i < numPoints; ++i)
                {
                    for (uint32_t c = 0; c < mBoneColliders.size(); ++c)
                    {
                        float reach = (mBoneColliders[c].mRadius + chain.mRadius) * scale;
                        if (i == 1)
                        {
                            // the pinned root already inside this collider (a shoulder's, at the root of an arm):
                            // its first segment can't come out of it, and pushing the next joint for it -- the push
                            // scaled up by 1/s -- shoved the chain into whatever lay beyond (the body). The next joint
                            // itself kept out of it though, straight out, as far as it's in: skipped whole, that first
                            // stretch of cloth lay in the shoulder (a thicker arm, its root inside, cut in more)
                            glm::vec3 ab = sColB[c] - sColA[c];
                            float ab2 = glm::dot(ab, ab);
                            float t = (ab2 > 0.000001f) ? glm::clamp(glm::dot(chain.mPos[0] - sColA[c], ab) / ab2, 0.0f, 1.0f) : 0.0f;
                            if (glm::distance(chain.mPos[0], sColA[c] + ab * t) < reach)
                            {
                                float t1 = (ab2 > 0.000001f) ? glm::clamp(glm::dot(chain.mPos[1] - sColA[c], ab) / ab2, 0.0f, 1.0f) : 0.0f;
                                glm::vec3 away = chain.mPos[1] - (sColA[c] + ab * t1);
                                float dist = glm::length(away);
                                if (dist < reach && dist > 0.00001f)
                                    chain.mPos[1] += away * ((reach - dist) / dist);
                                continue;
                            }
                        }
                        PushSegmentOut(chain.mPos[i - 1], chain.mPos[i], sColA[c], sColB[c], reach, i > 1);
                    }
                }
            }
        }
    }

    // each chain's bones posed from its particles
    for (BoneChain& chain : mBoneChains)
    {
        const uint32_t n = (uint32_t)chain.mBones.size();
        const uint32_t numPoints = n + 1;
        std::vector<glm::mat4>& model = chain.mModel;

        if (dt > 0.0f)
        {
            for (uint32_t i = 0; i < numPoints; ++i)
            {
                chain.mPrevOffset[i] = chain.mOffset[i];
                chain.mOffset[i] = chain.mPos[i] - chain.mTarget[i];
            }
        }

        // each bone turned to point at the next particle, the ones below it carried along
        for (uint32_t k = 0; k < n; ++k)
        {
            glm::vec3 head = glm::vec3(model[k][3]);
            glm::vec3 next = (k + 1 < n) ? glm::vec3(model[k + 1][3]) : glm::vec3(model[k] * glm::vec4(chain.mTipLocal, 1.0f));
            glm::vec3 want = glm::vec3(invTransform * glm::vec4(chain.mPos[k + 1], 1.0f)) - head;
            glm::vec3 have = next - head;
            if (glm::length(want) < 0.0001f || glm::length(have) < 0.0001f)
                continue;

            glm::quat turn = glm::rotation(glm::normalize(have), glm::normalize(want));
            glm::mat4 about = glm::translate(head) * glm::toMat4(turn) * glm::translate(-head);
            for (uint32_t j = k; j < n; ++j)
                model[j] = about * model[j];
        }

        for (uint32_t k = 0; k < n; ++k)
        {
            int32_t b = chain.mBones[k];
            mBoneMatrices[b] = model[k] * bones[b].mOffsetMatrix;
        }
    }
}

void SkeletalMesh3D::SetAnimEventHandler(AnimEventHandlerFP handler)
{
    mAnimEventHandler.mFuncPointer = handler;
}

AnimEventHandlerFP SkeletalMesh3D::GetAnimEventHandler()
{
    return mAnimEventHandler.mFuncPointer;
}

void SkeletalMesh3D::SetScriptAnimEventHandler(const ScriptFunc& func)
{
    mAnimEventHandler.mScriptFunc = func;
}

glm::vec3 SkeletalMesh3D::InterpolateScale(float time, const Channel& channel)
{
    if (channel.mScaleKeys.size() == 1)
    {
        return channel.mScaleKeys[0].mValue;
    }

    uint32_t index = FindScaleIndex(time, channel);
    uint32_t nextIndex = index + 1;
    OCT_ASSERT(nextIndex < channel.mScaleKeys.size());

    float deltaTime = channel.mScaleKeys[nextIndex].mTime - channel.mScaleKeys[index].mTime;
    float factor = (time - channel.mScaleKeys[index].mTime) / deltaTime;
    factor = glm::clamp(factor, 0.0f, 1.0f);
    OCT_ASSERT(factor >= 0.0f && factor <= 1.0f);

    glm::vec3 startScale = channel.mScaleKeys[index].mValue;
    glm::vec3 endScale = channel.mScaleKeys[nextIndex].mValue;
    glm::vec3 retScale = glm::mix(startScale, endScale, factor);
    return retScale;
}

glm::quat SkeletalMesh3D::InterpolateRotation(float time, const Channel& channel)
{
    if (channel.mRotationKeys.size() == 1)
    {
        return channel.mRotationKeys[0].mValue;
    }

    uint32_t index = FindRotationIndex(time, channel);
    uint32_t nextIndex = index + 1;
    OCT_ASSERT(nextIndex < channel.mRotationKeys.size());

    float deltaTime = channel.mRotationKeys[nextIndex].mTime - channel.mRotationKeys[index].mTime;
    float factor = (time - channel.mRotationKeys[index].mTime) / deltaTime;
    factor = glm::clamp(factor, 0.0f, 1.0f);
    OCT_ASSERT(factor >= 0.0f && factor <= 1.0f);

    glm::quat startQuat = channel.mRotationKeys[index].mValue;
    glm::quat endQuat = channel.mRotationKeys[nextIndex].mValue;
    glm::quat retQuat = glm::slerp(startQuat, endQuat, factor);
    retQuat = glm::normalize(retQuat);
    return retQuat;
}

glm::vec3 SkeletalMesh3D::InterpolatePosition(float time, const Channel& channel)
{
    if (channel.mPositionKeys.size() == 1)
    {
        return channel.mPositionKeys[0].mValue;
    }

    uint32_t index = FindPositionIndex(time, channel);
    uint32_t nextIndex = index + 1;
    OCT_ASSERT(nextIndex < channel.mPositionKeys.size());

    float deltaTime = channel.mPositionKeys[nextIndex].mTime - channel.mPositionKeys[index].mTime;
    float factor = (time - channel.mPositionKeys[index].mTime) / deltaTime;
    factor = glm::clamp(factor, 0.0f, 1.0f);
    OCT_ASSERT(factor >= 0.0f && factor <= 1.0f);

    glm::vec3 startPos = channel.mPositionKeys[index].mValue;
    glm::vec3 endPos = channel.mPositionKeys[nextIndex].mValue;
    glm::vec3 retPos = glm::mix(startPos, endPos, factor);
    return retPos;
}

void SkeletalMesh3D::DetectTriggeredAnimEvents(
    const Animation& animation,
    float prevTickTime,
    float tickTime,
    float animationSpeed,
    std::vector<AnimEvent>& outEvents)
{
    const std::vector<AnimEventTrack>& eventTracks = animation.mEventTracks;

    for (uint32_t trackIndex = 0; trackIndex < eventTracks.size(); ++trackIndex)
    {
        const std::vector<AnimEventKey>& keys = eventTracks[trackIndex].mEventKeys;
        for (uint32_t keyIndex = 0; keyIndex < keys.size(); ++keyIndex)
        {
            const AnimEventKey& key = keys[keyIndex];

            if ((animationSpeed > 0.0f && key.mTime > prevTickTime && key.mTime <= tickTime) ||
                (animationSpeed < 0.0f && key.mTime < prevTickTime && key.mTime >= tickTime) ||
                (animationSpeed > 0.0f && key.mTime > prevTickTime && tickTime < prevTickTime) || // Did we loop?
                (animationSpeed < 0.0f && key.mTime < prevTickTime && tickTime > prevTickTime))   // Did we loop?
            {
                AnimEvent newEvent;
                newEvent.mAnimation = animation.mName;
                newEvent.mName = eventTracks[trackIndex].mName;
                newEvent.mTime = key.mTime;
                newEvent.mValue = key.mValue;
                outEvents.push_back(newEvent);
            }
        }
    }
}

// The key before `time` (the one it interpolates from): the first i with time < keys[i + 1], or the second-last key
// past them all -- found by halving, not walked from the start. Animations baked a key a frame (hundreds
// of keys) walked every key up to the time, for every channel of every bone of every playing animation, each frame.
template<typename KeyT>
static uint32_t FindKeyIndex(const std::vector<KeyT>& keys, float time)
{
    const uint32_t n = uint32_t(keys.size());
    if (n < 2)
    {
        return 0;
    }
    uint32_t lo = 1, hi = n - 1;                // (the first key after `time` is in [lo, hi]; none: n - 1, the last)
    if (!(time < keys[hi].mTime))
    {
        return n - 2;
    }
    while (lo < hi)
    {
        const uint32_t mid = (lo + hi) / 2;
        if (time < keys[mid].mTime) hi = mid;
        else lo = mid + 1;
    }
    return lo - 1;
}

uint32_t SkeletalMesh3D::FindScaleIndex(float time, const Channel& channel)
{
    OCT_ASSERT(channel.mScaleKeys.size() > 0);
    return FindKeyIndex(channel.mScaleKeys, time);
}

uint32_t SkeletalMesh3D::FindRotationIndex(float time, const Channel& channel)
{
    OCT_ASSERT(channel.mRotationKeys.size() > 0);
    return FindKeyIndex(channel.mRotationKeys, time);
}

uint32_t SkeletalMesh3D::FindPositionIndex(float time, const Channel& channel)
{
    OCT_ASSERT(channel.mPositionKeys.size() > 0);
    return FindKeyIndex(channel.mPositionKeys, time);
}

glm::mat4 SkeletalMesh3D::GetBoneTransform(const std::string& name) const
{
    int32_t index = FindBoneIndex(name);
    return GetBoneTransform(index);
}

glm::vec3 SkeletalMesh3D::GetBonePosition(const std::string& name) const
{
    int32_t index = FindBoneIndex(name);
    return GetBonePosition(index);
}

glm::quat SkeletalMesh3D::GetBoneRotationQuat(const std::string& name) const
{
    int32_t index = FindBoneIndex(name);
    return GetBoneRotationQuat(index);
}

glm::vec3 SkeletalMesh3D::GetBoneRotationEuler(const std::string& name) const
{
    int32_t index = FindBoneIndex(name);
    return GetBoneRotationEuler(index);
}

glm::vec3 SkeletalMesh3D::GetBoneScale(const std::string& name) const
{
    int32_t index = FindBoneIndex(name);
    return GetBoneScale(index);
}

glm::mat4 SkeletalMesh3D::GetBoneTransform(int32_t index) const
{
    glm::mat4 retTransform;
    if (index >= 0 && index < (int32_t)mBoneMatrices.size())
    {
        retTransform = mBoneMatrices[index];
    }
    return retTransform;
}

glm::vec3 SkeletalMesh3D::GetBonePosition(int32_t boneIndex) const
{
    glm::vec3 retPosition(0.0f, 0.0f, 0.0f);
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();

    if (mesh != nullptr &&
        boneIndex >= 0 &&
        boneIndex < (int32_t)mBoneMatrices.size())
    {
        glm::mat4 offset = glm::inverse(mesh->GetBone(boneIndex).mOffsetMatrix);
        glm::mat4 transform = mTransform * mBoneMatrices[boneIndex] * offset;
        retPosition.x = transform[3][0];
        retPosition.y = transform[3][1];
        retPosition.z = transform[3][2];
    }

    return retPosition;
}

glm::quat SkeletalMesh3D::GetBoneRotationQuat(int32_t boneIndex) const
{
    glm::quat retRotation(0.0f, 0.0f, 0.0f, 1.0f);

    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    if (mesh != nullptr &&
        boneIndex >= 0 &&
        boneIndex < (int32_t)mBoneMatrices.size())
    {
        glm::mat4 offset = glm::inverse(mesh->GetBone(boneIndex).mOffsetMatrix);
        glm::mat4 transform = mTransform * mBoneMatrices[boneIndex] * offset;

        retRotation = Maths::ExtractRotation(transform);
    }

    return retRotation;
}

glm::vec3 SkeletalMesh3D::GetBoneRotationEuler(int32_t boneIndex) const
{
    glm::vec3 retRotation(0.0f, 0.0f, 0.0f);

    glm::quat retQuat = GetBoneRotationQuat(boneIndex);
    retRotation = glm::eulerAngles(retQuat) * RADIANS_TO_DEGREES;

    return retRotation;
}

glm::vec3 SkeletalMesh3D::GetBoneScale(int32_t boneIndex) const
{
    glm::vec3 retScale = glm::vec3(0, 0, 0);

    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    if (mesh != nullptr &&
        boneIndex >= 0 &&
        boneIndex < (int32_t)mBoneMatrices.size())
    {
        glm::mat4 offset = glm::inverse(mesh->GetBone(boneIndex).mOffsetMatrix);
        glm::mat4 transform = mTransform * mBoneMatrices[boneIndex] * offset;

        retScale = Maths::ExtractScale(transform);
    }

    return retScale;
}

void SkeletalMesh3D::SetBoneTransform(int32_t boneIndex, const glm::mat4& transform)
{
    // TODO: This function probably isn't working as intended. Please fix.
#if 1
    OCT_ASSERT(boneIndex >= 0 && boneIndex < int32_t(mBoneMatrices.size()));

    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    const std::vector<Bone>& bones = mesh->GetBones();
    mBoneMatrices[boneIndex] = bones[boneIndex].mInvOffsetMatrix * transform * bones[boneIndex].mOffsetMatrix;
#else
    // I attempted to recreate the way I am creating the bone transforms in SkeletalMesh::AnimateBones
    // but this just isn't working. Not sure what I'm doing wrong.
    SkeletalMesh* mesh = mSkeletalMesh.GetSkeletalMesh();
    if (mesh != nullptr)
    {
        OCT_ASSERT(boneIndex >= 0 && boneIndex < int32_t(mBoneMatrices.size()));
        mBoneMatrices[boneIndex] = transform;

        const std::vector<Bone>& bones = mesh->GetBones();

        int32_t parentIndex = bones[boneIndex].mParentIndex;
        if (parentIndex != -1)
        {
            mBoneMatrices[boneIndex] = mBoneMatrices[parentIndex] * mBoneMatrices[boneIndex];
        }

        mBoneMatrices[boneIndex] = mesh->GetInvRootTransform() * mBoneMatrices[boneIndex] * bones[boneIndex].mOffsetMatrix;
    }
#endif
}

void SkeletalMesh3D::SetBonePosition(int32_t boneIndex, glm::vec3 position)
{
    LogWarning("SkeletalMesh3D::SetBonePosition() not yet implemented");
}

void SkeletalMesh3D::SetBoneRotation(int32_t boneIndex, glm::vec3 rotation)
{
    LogWarning("SkeletalMesh3D::SetBoneRotation() not yet implemented");
}

void SkeletalMesh3D::SetBoneScale(int32_t boneIndex, glm::vec2 scale)
{
    LogWarning("SkeletalMesh3D::SetBoneScale() not yet implemented");
}

uint32_t SkeletalMesh3D::GetNumBones() const
{
    return uint32_t(mBoneMatrices.size());
}

BoneInfluenceMode SkeletalMesh3D::GetBoneInfluenceMode() const
{
    return mBoneInfluenceMode;
}

AnimationUpdateMode SkeletalMesh3D::GetAnimationUpdateMode() const
{
    return mAnimationUpdateMode;
}

void SkeletalMesh3D::SetAnimationUpdateMode(AnimationUpdateMode mode)
{
    mAnimationUpdateMode = mode;
}

Vertex* SkeletalMesh3D::GetSkinnedVertices()
{
    return mSkinnedVertices.data();
}

uint32_t SkeletalMesh3D::GetNumSkinnedVertices()
{
    return (uint32_t) mSkinnedVertices.size();
}

Material* SkeletalMesh3D::GetMaterial()
{
    Material* mat = mMaterialOverride.Get<Material>();

    if (!mat && mSkeletalMesh.Get())
    {
        mat = mSkeletalMesh.Get<SkeletalMesh>()->GetMaterial();
    }

    return mat;
}

void SkeletalMesh3D::Render()
{
    GFX_DrawSkeletalMeshComp(this);
}

Bounds SkeletalMesh3D::GetLocalBounds() const
{
    Bounds retBounds;
    if (mSkeletalMesh != nullptr)
    {
        retBounds = mSkeletalMesh.Get<SkeletalMesh>()->GetBounds();

    }
    else
    {
        retBounds = Mesh3D::GetLocalBounds();
    }

    if (mBoundsRadiusOverride > 0.0f)
    {
        retBounds.mRadius = mBoundsRadiusOverride;
    }

    return retBounds;
}

void SkeletalMesh3D::UpdateAnimation(float deltaTime, bool updateBones)
{
    if (mHasAnimatedThisFrame)
        return;

    static std::vector<DecompTransform> sDecompTransforms;
    static std::vector<AnimEvent> sAnimEvents;
    sAnimEvents.clear();
    sDecompTransforms.clear();

    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();

    bool inheritPose = mInheritPose && 
        updateBones &&
        mParent != nullptr && 
        mParent->GetType() == SkeletalMesh3D::GetStaticType();

    if (inheritPose &&
        mesh != nullptr)
    {
        // It's definitely possible to make this better by referencing the parent bone transforms
        // instead of copying them, but that would probably require some refactoring in the GFX layer.
        SkeletalMesh3D* parentMesh = mParent->As<SkeletalMesh3D>();

        if (!parentMesh->mHasAnimatedThisFrame)
        {
            parentMesh->UpdateAnimation(deltaTime, true);
        }

        uint32_t numBones = parentMesh->GetNumBones();

        // Inherited mesh should have the same skeleton.
        if (numBones == GetNumBones())
        {
            for (uint32_t i = 0; i < numBones; ++i)
            {
                mBoneMatrices[i] = parentMesh->GetBoneTransform((int32_t)i);
            }
        }
        else
        {
            LogError("Cannot inherit pose because number of bones differs between components.");
        }
    }

    if (mesh != nullptr &&
        !mAnimationPaused && 
        !inheritPose && 
        (mActiveAnimations.size() > 0 || mRevertToBindPose))
    {
        uint32_t numBones = GetNumBones();
        sDecompTransforms.resize(numBones);

        if (updateBones)
        {
            mesh->CopyBindPose(mBoneMatrices);
        }

        bool bonesUpdated = false;

        for (int32_t i = 0; i < (int32_t)mActiveAnimations.size(); ++i)
        {
            const Animation* anim = mesh->GetAnimation(mActiveAnimations[i].mName.c_str());
            bool animFinished = false;

            if (anim != nullptr)
            {
                float prevAnimTime = mActiveAnimations[i].mTime;
                float& animationTime = mActiveAnimations[i].mTime;
                float animationSpeed = mActiveAnimations[i].mSpeed * mAnimationSpeed;
                animationTime += (deltaTime * animationSpeed);

                float weight = mActiveAnimations[i].mWeight;
                weight = glm::clamp(weight, 0.0f, 1.0f);

                if (anim != nullptr)
                {
                    // Mod the animation time to fit within the animation duration
                    const float durationSeconds = anim->mDuration / anim->mTicksPerSecond;
                    const float prevTickTime = prevAnimTime * anim->mTicksPerSecond;

                    if (mActiveAnimations[i].mLoop)
                    {
                        if (animationSpeed > 0.0f &&
                            animationTime > durationSeconds)
                        {
                            animationTime = fmod(animationTime - durationSeconds, durationSeconds);
                        }
                        else if (animationSpeed < 0.0f &&
                                 animationTime < 0.0f)
                        {
                            animationTime = fmod(durationSeconds + animationTime, durationSeconds);
                        }
                    }
                    else
                    {
                        if (animationTime > durationSeconds)
                        {
                            animFinished = true;
                            animationTime = durationSeconds;
                        }
                    }

                    if (weight > 0.0f)
                    {
                        float tickTime = animationTime * anim->mTicksPerSecond;

                        if (updateBones)
                        {
                            // Go through all the channels, and update the relative transform 
                            // for each bone that exists in the animation.
                            for (uint32_t i = 0; i < anim->mChannels.size(); ++i)
                            {
                                int32_t boneIndex = anim->mChannels[i].mBoneIndex;
                                OCT_ASSERT(boneIndex != -1 &&
                                    boneIndex >= 0 &&
                                    boneIndex < (int32_t)mesh->GetBones().size());

                                if (boneIndex != -1)
                                {
                                    glm::vec3 scale = InterpolateScale(tickTime, anim->mChannels[i]);
                                    glm::quat rotation = InterpolateRotation(tickTime, anim->mChannels[i]);
                                    glm::vec3 position = InterpolatePosition(tickTime, anim->mChannels[i]);

                                    if (bonesUpdated)
                                    {
                                        sDecompTransforms[boneIndex].mPosition = glm::mix(sDecompTransforms[boneIndex].mPosition, position, weight);
                                        sDecompTransforms[boneIndex].mRotation = glm::slerp(sDecompTransforms[boneIndex].mRotation, rotation, weight);
                                        sDecompTransforms[boneIndex].mScale = glm::mix(sDecompTransforms[boneIndex].mScale, scale, weight);
                                    }
                                    else
                                    {
                                        // First animation doesn't need lerps.
                                        sDecompTransforms[boneIndex].mPosition = position;
                                        sDecompTransforms[boneIndex].mRotation = rotation;
                                        sDecompTransforms[boneIndex].mScale = scale;
                                    }

                                    sDecompTransforms[boneIndex].mValid = true;
                                }
                            }
                        }

                        if (anim->mEventTracks.size() > 0)
                        {
                            DetectTriggeredAnimEvents(*anim, prevTickTime, tickTime, animationSpeed, sAnimEvents);
                        }

                        bonesUpdated = true;
                    }
                }
            }
            else
            {
                LogWarning("Invalid animation name \"%s\" received in SkeletalMesh::AnimateBones()", mActiveAnimations[i].mName.c_str());
                animFinished = true;
            }

            if (animFinished)
            {
                const std::string& animName = mActiveAnimations[i].mName;

                for (int32_t q = int32_t(mQueuedAnimations.size()) - 1; q >= 0; --q)
                {
                    if (mQueuedAnimations[q].mDependentAnim == animName)
                    {
                        QueuedAnimation& queuedAnim = mQueuedAnimations[q];
                        PlayAnimation(
                            queuedAnim.mName.c_str(),
                            queuedAnim.mLoop,
                            queuedAnim.mSpeed,
                            queuedAnim.mWeight,
                            queuedAnim.mSlot);

                        mQueuedAnimations.erase(mQueuedAnimations.begin() + q);
                    }
                }

                mActiveAnimations.erase(mActiveAnimations.begin() + i);
                --i;
            }
        }

        if (updateBones)
        {
            // Unless no animations were computed, create bone matrices for all the bones
            // from the decomposed parts (position / rotation / scale);
            if (bonesUpdated)
            {
                // Create matrices from lerped pos/rot/scale
                for (uint32_t i = 0; i < numBones; ++i)
                {
                    if (sDecompTransforms[i].mValid)
                    {
                        glm::mat4& transform = mBoneMatrices[i];

                        transform = glm::mat4(1.0f);

                        transform = glm::translate(transform, sDecompTransforms[i].mPosition);
                        transform *= glm::toMat4(sDecompTransforms[i].mRotation);
                        transform = glm::scale(transform, sDecompTransforms[i].mScale);
                    }
                }
            }

            mesh->FinalizeBoneTransforms(mBoneMatrices);
            SimulateBoneDynamics(deltaTime);

            if (GFX_IsCpuSkinningRequired(this))
            {
                CpuSkinVertices();
            }
        }

        // Fire off any events that triggered.
        if (mAnimEventHandler.mFuncPointer != nullptr)
        {
            for (uint32_t i = 0; i < sAnimEvents.size(); ++i)
            {
                sAnimEvents[i].mNode = this;
                mAnimEventHandler.mFuncPointer(sAnimEvents[i]);
            }
        }
        if (mAnimEventHandler.mScriptFunc.IsValid())
        {
            for (uint32_t i = 0; i < sAnimEvents.size(); ++i)
            {
                sAnimEvents[i].mNode = this;

                Datum animTable;
                animTable.SetNodeField("node", ResolveWeakPtr<Node>(sAnimEvents[i].mNode));
                animTable.SetStringField("name", sAnimEvents[i].mName);
                animTable.SetStringField("animation", sAnimEvents[i].mAnimation);
                animTable.SetFloatField("time", sAnimEvents[i].mTime);
                animTable.SetVectorField("value", sAnimEvents[i].mValue);

                mAnimEventHandler.mScriptFunc.Call(1, &animTable);
            }
        }
    }

    // CPU skinned characters need to update their verts even if they are paused
    // because vertex data is double buffered for MAX_FRAMES.
    if (updateBones &&
        (inheritPose || mAnimationPaused) &&
        mesh != nullptr &&
        GFX_IsCpuSkinningRequired(this))
    {
        CpuSkinVertices();
    }

    if (updateBones)
    {
        UpdateAttachedChildren(deltaTime);
    }

    mHasAnimatedThisFrame = true;
}

void SkeletalMesh3D::UpdateAttachedChildren(float deltaTime)
{
    bool isAnimating = !mAnimationPaused &&
        (mAnimationSpeed != 0.0f) &&
        (mActiveAnimations.size() > 0 || mInheritPose);

    if (isAnimating)
    {
        for (uint32_t i = 0; i < mChildren.size(); ++i)
        {
            Node3D* child3D = mChildren[i]->IsNode3D() ? static_cast<Node3D*>(mChildren[i].Get()) : nullptr;

            if (child3D && child3D->GetParentBoneIndex() != -1)
            {
                child3D->MarkTransformDirty();
            }
        }
    }
}

#if PLATFORM_DOLPHIN && PROFILING_ENABLED
#include <ogc/lwp_watchdog.h>
// CPU skinning's share of the animation, for the console perf log's CULL line (Renderer.cpp reads and clears them each
// frame): the rest of "anim" is the pose -- the keys, the bone matrices, the hierarchy
uint64_t gOctSkinTicks = 0;
uint32_t gOctSkinVerts = 0;
#endif

void SkeletalMesh3D::CpuSkinVertices()
{
#if PLATFORM_DOLPHIN && PROFILING_ENABLED
    const uint64_t skinT0 = gettime();
    struct SkinTimer { uint64_t t0; ~SkinTimer() { gOctSkinTicks += gettime() - t0; } } skinTimer{ skinT0 };
#endif
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    if (mesh != nullptr)
    {
#if PLATFORM_DOLPHIN && PROFILING_ENABLED
        gOctSkinVerts += mesh->GetNumVertices();
#endif
#if API_GX && !EDITOR
        // The GX renderer draws these vertices straight from the array, and the GPU may still be drawing the last
        // frame with it. Two arrays, used in turn: the GPU only ever has the frame before in flight (GFX_BeginFrame
        // waits for the one before that), so the array it was given two frames ago is free to rewrite now. (One array
        // and a GxWaitGpu here stalled the CPU on the GPU's whole last frame, every frame, at the first skinned mesh:
        // 3-6 ms in a test level, and the frame's CPU and GPU no longer overlapped.)
        mSkinnedVertices.swap(mSkinnedVerticesBack);
#endif
        mSkinnedVertices.resize(mesh->GetNumVertices());
        const std::vector<VertexSkinned>& verts = mesh->GetVertices();

        if (mBoneInfluenceMode == BoneInfluenceMode::One)
        {
            for (uint32_t i = 0; i < mesh->GetNumVertices(); ++i)
            {
                const VertexSkinned& srcVert = verts[i];
                Vertex& dstVert = mSkinnedVertices[i];

                glm::mat4 transform = mBoneMatrices[srcVert.mBoneIndices[0]];

                dstVert.mPosition = transform * glm::vec4(srcVert.mPosition, 1.0f);
                dstVert.mNormal = transform * glm::vec4(srcVert.mNormal, 0.0f);
                dstVert.mTexcoord0 = srcVert.mTexcoord0;
                dstVert.mTexcoord1 = srcVert.mTexcoord1;
            }
        }
        else
        {
            for (uint32_t i = 0; i < mesh->GetNumVertices(); ++i)
            {
                const VertexSkinned& srcVert = verts[i];
                Vertex& dstVert = mSkinnedVertices[i];

                static_assert(MAX_BONE_INFLUENCES == 4, "Need to adjust this code or convert to loop.");
                // (most vertices have one or two bones: the rest, weighted 0, left out)
                glm::mat4 transform = mBoneMatrices[srcVert.mBoneIndices[0]] * srcVert.mBoneWeights[0];
                if (srcVert.mBoneWeights[1] != 0.0f) transform += mBoneMatrices[srcVert.mBoneIndices[1]] * srcVert.mBoneWeights[1];
                if (srcVert.mBoneWeights[2] != 0.0f) transform += mBoneMatrices[srcVert.mBoneIndices[2]] * srcVert.mBoneWeights[2];
                if (srcVert.mBoneWeights[3] != 0.0f) transform += mBoneMatrices[srcVert.mBoneIndices[3]] * srcVert.mBoneWeights[3];

                dstVert.mPosition = transform * glm::vec4(srcVert.mPosition, 1.0f);
                dstVert.mNormal = transform * glm::vec4(srcVert.mNormal, 0.0f);
                dstVert.mTexcoord0 = srcVert.mTexcoord0;
                dstVert.mTexcoord1 = srcVert.mTexcoord1;
            }
        }

        GFX_UpdateSkeletalMeshCompVertexBuffer(this, mSkinnedVertices);
    }
}
