#include "Nodes/3D/SkeletalMesh3d.h"
#include "Nodes/3D/StaticMesh3d.h"
#include "Assets/SkeletalMesh.h"
#include "Renderer.h"
#include "AssetManager.h"
#include "Log.h"
#include "Maths.h"
#include "Utilities.h"
#include <algorithm>

#include "Graphics/Graphics.h"
#if API_GX
#include "Graphics/GX/GxUtils.h"
#endif

#if PLATFORM_DOLPHIN && PROFILING_ENABLED
#include <ogc/lwp_watchdog.h>
// The pose's parts, for the console perf log's SKIN line (Renderer.cpp reads and clears them): the keys and the
// layers' blend, the bone matrices, the hierarchy (FinalizeBoneTransforms), the dynamic bones, and UpdateAnimation
// whole -- what "anim" holds beyond it is the rest of the skeletal mesh's cull and update
uint64_t gOctPoseKeysTicks = 0, gOctPoseMatsTicks = 0, gOctPoseFinalTicks = 0, gOctPoseDynTicks = 0, gOctPoseAllTicks = 0;
uint64_t gOctDynStepTicks = 0, gOctDynIterTicks = 0, gOctDynPoseTicks = 0;   // (the dynamic bones' own parts)
#define OCT_POSE_MARK(var) const uint64_t var = gettime()
#else
#define OCT_POSE_MARK(var)
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

// (24: a game can layer more than 8 -- a character's 8 arm layers with its walk's 5 over them -- and more than 16: a
// character's moves, poses and the layers between them. Only what's playing is kept: a list, not 24 of each)
#define NUM_ANIMATION_SLOTS 24

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
        mSkinnedRigid = false;                                  // (the arrays are refilled whole: CpuSkinBlended)
        mSkinnedRigidBack = false;

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

    // (only a new one needs room: one already playing is only updated -- with every slot taken, the check here before
    // turned away every update too, each animation's weight held where it was)
    if (anim == nullptr && mActiveAnimations.size() >= NUM_ANIMATION_SLOTS)
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

void SkeletalMesh3D::SetBoneDynamicsInertia(float scale)
{
    mBoneInertiaScale = glm::max(scale, 0.0f);
}

bool SkeletalMesh3D::SetBoneChainGravity(const std::string& rootBone, glm::vec3 gravity)
{
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    int32_t root = mesh ? mesh->FindBoneIndex(rootBone) : -1;
    for (BoneChain& chain : mBoneChains)
    {
        if (!chain.mBones.empty() && chain.mBones[0] == root)
        {
            chain.mGravity = gravity;
            return true;
        }
    }
    return false;
}

uint32_t SkeletalMesh3D::GetNumMorphTargets() const
{
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    return mesh ? mesh->GetNumMorphTargets() : 0;
}

void SkeletalMesh3D::SetMorphWeight(int32_t index, float weight)
{
    if (index < 0 || index >= int32_t(GetNumMorphTargets()))
        return;
    if (mMorphWeights.size() < GetNumMorphTargets())
        mMorphWeights.resize(GetNumMorphTargets(), 0.0f);
    mMorphWeights[index] = weight;
}

bool SkeletalMesh3D::SetMorphWeight(const std::string& name, float weight)
{
    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    int32_t index = mesh ? mesh->FindMorphTarget(name) : -1;
    SetMorphWeight(index, weight);
    return index >= 0;
}

float SkeletalMesh3D::GetMorphWeight(int32_t index) const
{
    return (index >= 0 && index < int32_t(mMorphWeights.size())) ? mMorphWeights[index] : 0.0f;
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

// 1/sqrt(x), x > 0: on the GameCube the hardware's estimate (frsqrte) refined twice by Newton's method -- to about a
// millionth, far finer than a normal's 8 bits on the console -- with no software square root and no divide
static inline float OctRsqrt(float x)
{
#if PLATFORM_DOLPHIN
    double y;
    __asm__("frsqrte %0,%1" : "=f"(y) : "f"((double)x));
    const double h = 0.5 * x;
    y = y * (1.5 - h * y * y);                                      // (the estimate's 5 bits to 10,
    y = y * (1.5 - h * y * y);                                      //  then 20: a unit normal to a millionth)
    return (float)y;
#else
    return 1.0f / sqrtf(x);
#endif
}

// whether the segment p-q can come within reach of a capsule's line, its box lo-hi: the segment's box grown by reach
// not meeting the line's and it can't -- PushSegmentOut would find nothing, so it's skipped. Only comparisons: the
// GameCube has no square root instruction (newlib's sqrtf is software), and every chain segment against every collider,
// four passes a frame, was 2.6 ms of Mario's dynamic bones, almost all of them far apart
static inline bool NearCapsule(const glm::vec3& p, const glm::vec3& q, const glm::vec3& lo, const glm::vec3& hi, float reach)
{
    return glm::min(p.x, q.x) - reach <= hi.x && glm::max(p.x, q.x) + reach >= lo.x &&
           glm::min(p.y, q.y) - reach <= hi.y && glm::max(p.y, q.y) + reach >= lo.y &&
           glm::min(p.z, q.z) - reach <= hi.z && glm::max(p.z, q.z) + reach >= lo.z;
}

// whether a box lo-hi (some points') grown by reach meets a capsule line's box clo-chi: if not, no segment between those
// points can come within reach of it (each segment's own box is inside lo-hi), so NearCapsule would turn every one down
static inline bool BoxNearBox(const glm::vec3& lo, const glm::vec3& hi, const glm::vec3& clo, const glm::vec3& chi, float reach)
{
    return lo.x - reach <= chi.x && hi.x + reach >= clo.x &&
           lo.y - reach <= chi.y && hi.y + reach >= clo.y &&
           lo.z - reach <= chi.z && hi.z + reach >= clo.z;
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
    static std::vector<glm::vec3> sColA, sColB, sColLo, sColHi;
    sColA.resize(mBoneColliders.size());
    sColB.resize(mBoneColliders.size());
    sColLo.resize(mBoneColliders.size());
    sColHi.resize(mBoneColliders.size());
    for (uint32_t c = 0; c < mBoneColliders.size(); ++c)
    {
        const BoneCollider& col = mBoneColliders[c];
        glm::mat4 world = mTransform * mBoneMatrices[col.mBone] * bones[col.mBone].mInvOffsetMatrix;
        sColA[c] = glm::vec3(world * glm::vec4(col.mA, 1.0f));
        sColB[c] = glm::vec3(world * glm::vec4(col.mB, 1.0f));
        sColLo[c] = glm::min(sColA[c], sColB[c]);                 // (the line's box, for NearCapsule)
        sColHi[c] = glm::max(sColA[c], sColB[c]);
    }

    glm::mat4 invTransform = glm::inverse(mTransform);
    OCT_POSE_MARK(tDynStep0);

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
            chain.mLastVel.assign(numPoints, glm::vec3(0.0f));
            chain.mHasVel = false;
            chain.mReset = false;
        }

        if (dt > 0.0f)
        {
            const float keep = powf(1.0f - chain.mDamping, frames);
            const float pull = 1.0f - powf(1.0f - chain.mStiffness, frames);
            const glm::vec3 fall = chain.mGravity * dt * dt;

            for (uint32_t i = 0; i < numPoints; ++i)
            {
                // the animation's speed this frame, and how much it changed from the last (its acceleration: what
                // throws the chain -- none the first frame, nothing to compare with). Speeds, not the moves
                // themselves: a frame a millisecond longer moves a fast mesh further, and the difference between
                // two moves at a steady speed threw the whole chain about, frame to frame
                glm::vec3 vel = (target[i] - chain.mLastTarget[i]) / glm::max(deltaTime, 0.0001f);
                glm::vec3 accel = chain.mHasVel ? (vel - chain.mLastVel[i]) * dt : glm::vec3(0.0f);
                chain.mLastVel[i] = vel;
                chain.mLastTarget[i] = target[i];

                glm::vec3 offset = glm::vec3(0.0f);
                if (i > 0)
                {
                    offset = chain.mOffset[i] + (chain.mOffset[i] - chain.mPrevOffset[i]) * keep - accel * (chain.mInertia * mBoneInertiaScale) + fall;
                    offset *= (1.0f - pull);
                }
                chain.mPos[i] = target[i] + offset;
            }
            chain.mHasVel = true;
        }
        else
        {
            for (uint32_t i = 0; i < numPoints; ++i)
                chain.mPos[i] = target[i] + chain.mOffset[i];
        }
    }

    OCT_POSE_MARK(tDynIter0);
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
                if (numPoints < 2)
                    continue;
                // (the two lines' box, below the roots: a collider it can't reach skipped whole -- grown as pushes
                //  move their points, so it stays around them)
                glm::vec3 lo = glm::min(a.mPos[1], b.mPos[1]), hi = glm::max(a.mPos[1], b.mPos[1]);
                for (uint32_t i = 2; i < numPoints; ++i)
                {
                    lo = glm::min(lo, glm::min(a.mPos[i], b.mPos[i]));
                    hi = glm::max(hi, glm::max(a.mPos[i], b.mPos[i]));
                }
                for (uint32_t c = 0; c < mBoneColliders.size(); ++c)
                {
                    float reach = (mBoneColliders[c].mRadius + 0.5f * (a.mRadius + b.mRadius)) * scale;
                    if (!BoxNearBox(lo, hi, sColLo[c], sColHi[c], reach))
                        continue;
                    for (uint32_t i = 1; i < numPoints; ++i)
                    {
                        bool moved = false;
                        if (NearCapsule(a.mPos[i], b.mPos[i], sColLo[c], sColHi[c], reach))
                            moved |= PushSegmentOut(a.mPos[i], b.mPos[i], sColA[c], sColB[c], reach, true);
                        if (i + 1 < numPoints)
                        {
                            if (NearCapsule(a.mPos[i], b.mPos[i + 1], sColLo[c], sColHi[c], reach))
                                moved |= PushSegmentOut(a.mPos[i], b.mPos[i + 1], sColA[c], sColB[c], reach, true);
                            if (NearCapsule(b.mPos[i], a.mPos[i + 1], sColLo[c], sColHi[c], reach))
                                moved |= PushSegmentOut(b.mPos[i], a.mPos[i + 1], sColA[c], sColB[c], reach, true);
                        }
                        if (moved)
                        {
                            const uint32_t last = glm::min(i + 1, numPoints - 1);
                            for (uint32_t j = i; j <= last; ++j)
                            {
                                lo = glm::min(lo, glm::min(a.mPos[j], b.mPos[j]));
                                hi = glm::max(hi, glm::max(a.mPos[j], b.mPos[j]));
                            }
                        }
                    }
                }
            }

            for (BoneChain& chain : mBoneChains)
            {
                const uint32_t numPoints = (uint32_t)chain.mBones.size() + 1;

                // (the colliders this line's box -- all its points -- can reach, in their order: the others can't
                //  touch any of its segments, nor have its root inside them, and are skipped whole. A push moves its
                //  points: the box grown by them and the list made again, the colliders still taken in their order)
                glm::vec3 lo = chain.mPos[0], hi = chain.mPos[0];
                for (uint32_t i = 1; i < numPoints; ++i)
                {
                    lo = glm::min(lo, chain.mPos[i]);
                    hi = glm::max(hi, chain.mPos[i]);
                }
                static std::vector<uint32_t> sNear;
                auto listNear = [&]()
                {
                    sNear.clear();
                    for (uint32_t c = 0; c < mBoneColliders.size(); ++c)
                    {
                        if (BoxNearBox(lo, hi, sColLo[c], sColHi[c], (mBoneColliders[c].mRadius + chain.mRadius) * scale))
                            sNear.push_back(c);
                    }
                };
                listNear();

                // out of the colliders: the whole of each segment, not only its joints (a collider can come up
                // between two), its two ends moved by how near the contact is to each -- the root's end stays
                for (uint32_t i = 1; i < numPoints; ++i)
                {
                    for (uint32_t ni = 0; ni < sNear.size(); ++ni)
                    {
                        const uint32_t c = sNear[ni];
                        const glm::vec3 was0 = chain.mPos[i - 1], was1 = chain.mPos[i];
                        bool moved = false;
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
                                {
                                    chain.mPos[1] += away * ((reach - dist) / dist);
                                    moved = true;
                                }
                            }
                            else if (NearCapsule(chain.mPos[i - 1], chain.mPos[i], sColLo[c], sColHi[c], reach))
                                moved = PushSegmentOut(chain.mPos[i - 1], chain.mPos[i], sColA[c], sColB[c], reach, i > 1);
                        }
                        else if (NearCapsule(chain.mPos[i - 1], chain.mPos[i], sColLo[c], sColHi[c], reach))
                            moved = PushSegmentOut(chain.mPos[i - 1], chain.mPos[i], sColA[c], sColB[c], reach, i > 1);
                        if (moved && (chain.mPos[i - 1] != was0 || chain.mPos[i] != was1))
                        {
                            lo = glm::min(lo, glm::min(chain.mPos[i - 1], chain.mPos[i]));
                            hi = glm::max(hi, glm::max(chain.mPos[i - 1], chain.mPos[i]));
                            listNear();                             // (again, the colliders after this one taken
                            ni = 0;                                 //  from where it is in the new list)
                            while (ni < sNear.size() && sNear[ni] <= c) ++ni;
                            --ni;
                        }
                    }
                }
            }
        }
    }

    OCT_POSE_MARK(tDynPose0);
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
#if PLATFORM_DOLPHIN && PROFILING_ENABLED
    const uint64_t tDynEnd = gettime();
    gOctDynStepTicks += tDynIter0 - tDynStep0;
    gOctDynIterTicks += tDynPose0 - tDynIter0;
    gOctDynPoseTicks += tDynEnd - tDynPose0;
#endif
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

// Normalized lerp of two rotations, the shorter way round: no trig, where glm::slerp takes an acos and three sines. Between
// neighbouring keys (a few degrees apart at most) it's slerp to a fraction of a degree; blending animation layers, the
// ends are exact and only the speed through the middle differs. On the GameCube, Mario's pose (65 bones, several layers,
// two slerps a bone a layer) spent most of its 3.6 ms in sinf/acosf.
static inline glm::quat NLerpQuat(const glm::quat& a, const glm::quat& b, float t)
{
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float s = (d < 0.0f) ? -t : t;
    const float u = 1.0f - t;
    glm::quat r(u * a.w + s * b.w, u * a.x + s * b.x, u * a.y + s * b.y, u * a.z + s * b.z);
    const float len2 = r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w;
    if (len2 <= 1e-12f)
        return a;
    const float inv = 1.0f / sqrtf(len2);
    return glm::quat(r.w * inv, r.x * inv, r.y * inv, r.z * inv);
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
    return NLerpQuat(startQuat, endQuat, factor);
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
static uint32_t FindKeyIndexSearch(const std::vector<KeyT>& keys, float time)
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

// The same key, found by guessing first: keys baked a frame apart are evenly spaced, so where `time` falls between the
// first key and the last says which it is; a step or two either way confirms it (the binary search's own answer: the
// last g with every key from 1 to g at or before `time`, and the key after it past it). Keys unevenly spaced: after a
// few steps, the search. Mario's pose looked up ~1000 keys a frame (three a channel, every bone, every layer) at ~8
// probes each, scattered through each channel's keys -- 0.55 ms on the GameCube.
template<typename KeyT>
static uint32_t FindKeyIndex(const std::vector<KeyT>& keys, float time)
{
    const uint32_t n = uint32_t(keys.size());
    if (n < 2)
    {
        return 0;
    }
    const float t0 = keys[0].mTime, t1 = keys[n - 1].mTime;
    uint32_t g = 0;
    if (t1 > t0)
    {
        const float f = (time - t0) * (float(n - 1) / (t1 - t0));
        if (f >= float(n - 2)) g = n - 2;
        else if (f > 0.0f) g = uint32_t(f);
    }
    uint32_t steps = 0;
    while (g > 0 && time < keys[g].mTime)
    {
        --g;
        if (++steps > 4) return FindKeyIndexSearch(keys, time);
    }
    while (g < n - 2 && !(time < keys[g + 1].mTime))
    {
        ++g;
        if (++steps > 4) return FindKeyIndexSearch(keys, time);
    }
    return g;
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

void SkeletalMesh3D::SetBoneScale(int32_t boneIndex, glm::vec3 scale)
{
    if (boneIndex < 0)
    {
        return;
    }

    for (auto& over : mBoneScaleOverrides)
    {
        if (over.first == boneIndex)
        {
            over.second = scale;
            return;
        }
    }

    mBoneScaleOverrides.push_back({ boneIndex, scale });
}

void SkeletalMesh3D::ClearBoneScale(int32_t boneIndex)
{
    for (uint32_t i = 0; i < mBoneScaleOverrides.size(); ++i)
    {
        if (mBoneScaleOverrides[i].first == boneIndex)
        {
            mBoneScaleOverrides.erase(mBoneScaleOverrides.begin() + i);
            return;
        }
    }
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
    // (sharing its parent's this frame: the parent's array as it is now -- the one of its two it skinned into last)
    if (mSkinShare != nullptr && mSkinShare == mParent.Get())
        return mSkinShare->mSkinnedVertices.data();
    return mSkinnedVertices.data();
}

uint32_t SkeletalMesh3D::GetNumSkinnedVertices()
{
    if (mSkinShare != nullptr && mSkinShare == mParent.Get())
        return (uint32_t) mSkinShare->mSkinnedVertices.size();
    return (uint32_t) mSkinnedVertices.size();
}

bool SkeletalMesh3D::IsSkinnedRigidOnGpu()
{
    if (mSkinShare != nullptr && mSkinShare == mParent.Get())
        return mSkinShare->mSkinnedRigid;
    return mSkinnedRigid;
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
    mSkinShare = nullptr;                                       // (set again below while it still holds)
#if PLATFORM_DOLPHIN && PROFILING_ENABLED
    struct PoseAllTimer { uint64_t t0; ~PoseAllTimer() { gOctPoseAllTicks += gettime() - t0; } } poseAllTimer{ gettime() };
#endif

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
        OCT_POSE_MARK(tKeys0);

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
                                        sDecompTransforms[boneIndex].mRotation = NLerpQuat(sDecompTransforms[boneIndex].mRotation, rotation, weight);
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

        OCT_POSE_MARK(tKeys1);
#if PLATFORM_DOLPHIN && PROFILING_ENABLED
        gOctPoseKeysTicks += tKeys1 - tKeys0;
#endif
        if (updateBones)
        {
            // Unless no animations were computed, create bone matrices for all the bones
            // from the decomposed parts (position / rotation / scale);
            if (bonesUpdated)
            {
                // Scales held from script, over whatever the animations blended to
                for (const auto& over : mBoneScaleOverrides)
                {
                    if (over.first < (int32_t)numBones && sDecompTransforms[over.first].mValid)
                    {
                        sDecompTransforms[over.first].mScale = over.second;
                    }
                }

                // Create matrices from lerped pos/rot/scale
                for (uint32_t i = 0; i < numBones; ++i)
                {
                    if (sDecompTransforms[i].mValid)
                    {
                        // translate * rotate * scale, written out: the rotation's columns times the scale, the
                        // position the last column (the same matrix, without three 4x4 multiplies a bone)
                        const glm::mat3 r = glm::mat3_cast(sDecompTransforms[i].mRotation);
                        const glm::vec3& s = sDecompTransforms[i].mScale;
                        const glm::vec3& p = sDecompTransforms[i].mPosition;
                        mBoneMatrices[i] = glm::mat4(glm::vec4(r[0] * s.x, 0.0f), glm::vec4(r[1] * s.y, 0.0f),
                                                     glm::vec4(r[2] * s.z, 0.0f), glm::vec4(p, 1.0f));
                    }
                }
            }

            OCT_POSE_MARK(tMats1);
            mesh->FinalizeBoneTransforms(mBoneMatrices);
            OCT_POSE_MARK(tFinal1);
            SimulateBoneDynamics(deltaTime);
            OCT_POSE_MARK(tDyn1);
#if PLATFORM_DOLPHIN && PROFILING_ENABLED
            gOctPoseMatsTicks += tMats1 - tKeys1;
            gOctPoseFinalTicks += tFinal1 - tMats1;
            gOctPoseDynTicks += tDyn1 - tFinal1;
#endif

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
#if API_GX && !EDITOR
        // An inherited pose of the parent's own mesh, the same influences, no morphs on either: its skinned vertices
        // are the parent's exactly -- drawn from the parent's array (GetSkinnedVertices) instead of skinned again.
        // (A Starman's five afterimages, each Mario's mesh in Mario's pose: 2468 vertices each, ~0.9 ms each on the
        // GameCube.) The parent was updated first (above); its array is the one it skinned into this frame.
        SkeletalMesh3D* parentSkel = inheritPose ? mParent->As<SkeletalMesh3D>() : nullptr;
        bool share = parentSkel != nullptr &&
            parentSkel->mSkeletalMesh.Get<SkeletalMesh>() == mesh &&
            parentSkel->mSkinShare == nullptr &&
            parentSkel->mBoneInfluenceMode == mBoneInfluenceMode &&
            parentSkel->mSkinnedVertices.size() == mesh->GetNumVertices() &&
            parentSkel->GetNumBones() == GetNumBones() &&
            GFX_IsCpuSkinningRequired(parentSkel);
        for (uint32_t m = 0; share && m < (uint32_t)mMorphWeights.size(); ++m)
            share = (mMorphWeights[m] == 0.0f);
        for (uint32_t m = 0; share && m < (uint32_t)parentSkel->mMorphWeights.size(); ++m)
            share = (parentSkel->mMorphWeights[m] == 0.0f);
        if (share && parentSkel->mSkinnedRigid)
        {
            // (the parent's rigid vertices left to the GPU: only if this one's material lets it do the same)
            const uint16_t* blended = nullptr;
            const uint16_t* rigid = nullptr;
            uint32_t numBlended = 0, numRigid = 0;
            share = GxSkinRigidPlan(this, blended, numBlended, rigid, numRigid);
        }
        if (share)
            mSkinShare = parentSkel;
        else
#endif
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
// CPU skinning's share of the animation, for the console perf log's CULL line (Renderer.cpp reads and clears them each
// frame): the rest of "anim" is the pose -- the keys, the bone matrices, the hierarchy
uint64_t gOctSkinTicks = 0;
uint32_t gOctSkinVerts = 0;
#endif

#if PLATFORM_DOLPHIN && !EDITOR
// ---- CPU skinning on the Gekko, two floats an instruction: its paired singles ---------------------------------------
// Every FPR holds two singles, ps0 and ps1; a ps_ instruction works on both, and psq_l/psq_st load and store a pair
// (or, W=1, one: ps1 then 1.0) through a GQR, which can turn integers into floats as they load. GQR0 is plain floats
// (libogc's ps_guMtx functions, and the SDK, keep it 0); the morph block's int16s go through GQR7, set to signed 16-bit
// just for the loop and put back after (a thread switch keeps each thread's own GQRs: PPCContext.gqr).
//   The compiler doesn't know paired singles (GCC's -mpaired is gone): to it an FPR is a double, and it copies one with
// fmr -- ps0 only -- and spills it with stfd, ps0 only, whenever its register allocation likes (it did, in the blend
// and the morph sums, written as one asm an instruction). So all of a vertex's float work is one asm, registers named,
// as the SDK's own PSVEC/PSMTX functions are: no pair is ever in a C variable. The C around it does the integer work
// (the cache lines, the bones to blend); the asm's constants are scalars (ps0 only), in FPRs it leaves alone.
#define OCT_GQR_S16 919                                             // (GQR7, while the skinning runs: s16, scale 0)
#define OCT_GQR_S16_VALUE 0x00070007u
// Each line of skinned vertices written out to memory as soon as it's finished (dcbst), while the loop goes on: the
// flush after it (GFX_UpdateSkeletalMeshCompVertexBuffer) then finds the lines clean, instead of writing the whole
// array back (~100 KB for a parachute) a line after another with nothing else to do. 0: as before
#ifndef OCT_SKIN_STORE_LINES
#define OCT_SKIN_STORE_LINES 1
#endif

// The vertex's own (VertexSkinned, 60 bytes: position 0, texture coordinates 12 and 20, normal 28) into f1 P (x y),
// f2 PZ (z, ps0), f3 N, f4 NZ, f5 and f6 the texture coordinates
#define OCT_SKIN_ASM_SRC \
    "psq_l     1,0(%[src]),0,0\n\t" \
    "psq_l     2,8(%[src]),1,0\n\t" \
    "psq_l     3,28(%[src]),0,0\n\t" \
    "psq_l     4,36(%[src]),1,0\n\t" \
    "psq_l     5,12(%[src]),0,0\n\t" \
    "psq_l     6,20(%[src]),0,0\n\t"
// The pairs of morph slots of a vertex (at least one: SkeletalMesh::MorphBlock), each pair 6 int16s -- x y of its
// first slot, x y of its second, the two z's -- by its weight pair (each slot's weight times its scale): the x y of the
// first by its weight (ps_madds0) summed in A from the vertex's own (XY), the second's (ps_madds1) in B -- two chains,
// not one twice as long -- the z's (a pair, by both weights) in Z; TW T0 T1 T2 the loads. OCT_SKIN_ASM_SUM then puts
// the sums in XY and XZ (ps0) -- later, so the next sums' instructions go first and the FPU has work while it waits.
#define OCT_SKIN_ASM_MORPH(XY, M, W, N, L1, L2, TW, T0, T1, T2, A, B, Z) \
    "psq_l     " TW ",0(%[" W "]),0,0\n\t" \
    "psq_l     " T0 ",0(%[" M "]),0,7\n\t" \
    "psq_l     " T1 ",4(%[" M "]),0,7\n\t" \
    "psq_l     " T2 ",8(%[" M "]),0,7\n\t" \
    "addic.    %[" N "],%[" N "],-1\n\t" \
    "ps_madds0 " A "," T0 "," TW "," XY "\n\t" \
    "ps_muls1  " B "," T1 "," TW "\n\t" \
    "ps_mul    " Z "," T2 "," TW "\n\t" \
    "beq       " L2 "f\n" \
    L1 ":\n\t" \
    "psq_lu    " TW ",8(%[" W "]),0,0\n\t" \
    "psq_lu    " T0 ",12(%[" M "]),0,7\n\t" \
    "psq_l     " T1 ",4(%[" M "]),0,7\n\t" \
    "psq_l     " T2 ",8(%[" M "]),0,7\n\t" \
    "addic.    %[" N "],%[" N "],-1\n\t" \
    "ps_madds0 " A "," T0 "," TW "," A "\n\t" \
    "ps_madds1 " B "," T1 "," TW "," B "\n\t" \
    "ps_madd   " Z "," T2 "," TW "," Z "\n\t" \
    "bne       " L1 "b\n" \
    L2 ":\n\t"
#define OCT_SKIN_ASM_SUM(XY, XZ, A, B, Z) \
    "ps_add    " XY "," A "," B "\n\t" \
    "ps_sum0   " Z "," Z "," Z "," Z "\n\t" \
    "fadds     " XZ "," Z "," XZ "\n\t"
// The moves' sums in f11 f12 f13, the normal changes' in f17 f18 f19 (OCT_SKIN_ASM_MORPH); their tails side by side
#define OCT_SKIN_ASM_SUM2 \
    "ps_add    1,11,12\n\t" \
    "ps_add    3,17,18\n\t" \
    "ps_sum0   13,13,13,13\n\t" \
    "ps_sum0   19,19,19,19\n\t" \
    "fadds     2,13,2\n\t" \
    "fadds     4,19,4\n\t"
// The bones' blended 3x4 -- a glm::mat4's columns 0..3 (cols), each two pairs, x y and z w (w unused), f14..f21 --
// times the position and the normal, and the vertex out (Vertex, 40 bytes: position 0, texture coordinates 12 and
// 20, normal 28). Four chains of three side by side: the FPU has one reservation station, and a chain alone waits out
// each step's three cycles
#define OCT_SKIN_ASM_OUT \
    "psq_l     14,0(%[cols]),0,0\n\t" \
    "psq_l     15,8(%[cols]),0,0\n\t" \
    "psq_l     20,48(%[cols]),0,0\n\t" \
    "psq_l     21,56(%[cols]),0,0\n\t" \
    "psq_l     16,16(%[cols]),0,0\n\t" \
    "psq_l     17,24(%[cols]),0,0\n\t" \
    "ps_madds0 22,14,1,20\n\t" \
    "ps_madds0 23,15,1,21\n\t" \
    "psq_l     18,32(%[cols]),0,0\n\t" \
    "ps_muls0  24,14,3\n\t" \
    "psq_l     19,40(%[cols]),0,0\n\t" \
    "ps_muls0  25,15,3\n\t" \
    "ps_madds1 22,16,1,22\n\t" \
    "ps_madds1 23,17,1,23\n\t" \
    "ps_madds1 24,16,3,24\n\t" \
    "ps_madds1 25,17,3,25\n\t" \
    "ps_madds0 22,18,2,22\n\t" \
    "ps_madds0 23,19,2,23\n\t" \
    "ps_madds0 24,18,4,24\n\t" \
    "ps_madds0 25,19,4,25\n\t" \
    "psq_st    5,12(%[dst]),0,0\n\t" \
    "psq_st    6,20(%[dst]),0,0\n\t" \
    "psq_st    22,0(%[dst]),0,0\n\t" \
    "psq_st    23,8(%[dst]),1,0\n\t" \
    "psq_st    24,28(%[dst]),0,0\n\t" \
    "psq_st    25,36(%[dst]),1,0"
// As OCT_SKIN_ASM_OUT, with the normal made unit again: 1/sqrt(x*x + y*y + z*z) -- the estimate (frsqrte) and two
// Newton steps (the SDK's PSVECNormalize takes one, trusting the Gekko's estimate to 1/4096; the second, a few
// cycles, holds to a millionth even from the 1/32 the PowerPC promises); left as it is when nearly 0 (as the PC: a
// length squared under 1e-8). The transform is linear, so the normal goes through it as it is and its result is
// scaled: the chain of steps, each waiting on the last, runs alongside the transform instead of before it
#define OCT_SKIN_ASM_OUT_RENORM \
    "ps_mul    7,3,3\n\t" \
    "psq_l     14,0(%[cols]),0,0\n\t" \
    "psq_l     15,8(%[cols]),0,0\n\t" \
    "psq_l     20,48(%[cols]),0,0\n\t" \
    "ps_madd   8,4,4,7\n\t" \
    "psq_l     21,56(%[cols]),0,0\n\t" \
    "psq_l     16,16(%[cols]),0,0\n\t" \
    "psq_l     17,24(%[cols]),0,0\n\t" \
    "ps_sum0   8,8,4,7\n\t" \
    "ps_madds0 22,14,1,20\n\t" \
    "psq_l     18,32(%[cols]),0,0\n\t" \
    "ps_madds0 23,15,1,21\n\t" \
    "frsqrte   9,8\n\t" \
    "psq_l     19,40(%[cols]),0,0\n\t" \
    "ps_muls0  24,14,3\n\t" \
    "ps_muls0  25,15,3\n\t" \
    "fsubs     10,8,%[tiny]\n\t" \
    "ps_madds1 22,16,1,22\n\t" \
    "fmuls     11,9,9\n\t" \
    "ps_madds1 23,17,1,23\n\t" \
    "fmuls     12,9,%[half]\n\t" \
    "ps_madds1 24,16,3,24\n\t" \
    "fnmsubs   11,11,8,%[three]\n\t" \
    "ps_madds1 25,17,3,25\n\t" \
    "ps_madds0 22,18,2,22\n\t" \
    "ps_madds0 23,19,2,23\n\t" \
    "fmuls     11,11,12\n\t" \
    "ps_madds0 24,18,4,24\n\t" \
    "fmuls     12,11,11\n\t" \
    "ps_madds0 25,19,4,25\n\t" \
    "fmuls     13,11,%[half]\n\t" \
    "psq_st    22,0(%[dst]),0,0\n\t" \
    "psq_st    23,8(%[dst]),1,0\n\t" \
    "fnmsubs   12,12,8,%[three]\n\t" \
    "psq_st    5,12(%[dst]),0,0\n\t" \
    "psq_st    6,20(%[dst]),0,0\n\t" \
    "fmuls     11,12,13\n\t" \
    "fsel      11,10,11,%[one]\n\t" \
    "ps_muls0  24,24,11\n\t" \
    "ps_muls0  25,25,11\n\t" \
    "psq_st    24,28(%[dst]),0,0\n\t" \
    "psq_st    25,36(%[dst]),1,0"
#define OCT_SKIN_ASM_CLOBBERS \
    "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12", "fr13", \
    "fr14", "fr15", "fr16", "fr17", "fr18", "fr19", "fr20", "fr21", "fr22", "fr23", "fr24", "fr25", \
    "cr0", "xer", "memory"

// The bones' blended 3x4 for a vertex of more than one bone, into out (16 floats, 8-aligned): bone b0 by its weight
// (w0, a float), then each other bone weighted (n, from list: a bone's matrix, its weight), in order -- the PC's sums
struct OctSkinBone { const char* mMat; const float* mWeight; };
static inline __attribute__((always_inline)) void OctSkinBlend(float* out, const char* b0, const float* w0,
                                                              const OctSkinBone* list, uint32_t n)
{
    const char* b;
    const float* w;
    __asm__ volatile(
        "psq_l     0,0(%[w0]),1,0\n\t"
        "psq_l     1,0(%[b0]),0,0\n\t"
        "psq_l     2,8(%[b0]),0,0\n\t"
        "psq_l     3,16(%[b0]),0,0\n\t"
        "psq_l     4,24(%[b0]),0,0\n\t"
        "ps_muls0  1,1,0\n\t"
        "psq_l     5,32(%[b0]),0,0\n\t"
        "ps_muls0  2,2,0\n\t"
        "psq_l     6,40(%[b0]),0,0\n\t"
        "ps_muls0  3,3,0\n\t"
        "psq_l     7,48(%[b0]),0,0\n\t"
        "ps_muls0  4,4,0\n\t"
        "psq_l     8,56(%[b0]),0,0\n\t"
        "ps_muls0  5,5,0\n\t"
        "cmpwi     %[n],0\n\t"
        "ps_muls0  6,6,0\n\t"
        "ps_muls0  7,7,0\n\t"
        "ps_muls0  8,8,0\n\t"
        "beq       2f\n\t"
        "mtctr     %[n]\n"
        "1:\n\t"
        "lwz       %[b],0(%[list])\n\t"
        "lwz       %[w],4(%[list])\n\t"
        "addi      %[list],%[list],8\n\t"
        "psq_l     0,0(%[w]),1,0\n\t"
        "psq_l     9,0(%[b]),0,0\n\t"
        "psq_l     10,8(%[b]),0,0\n\t"
        "psq_l     11,16(%[b]),0,0\n\t"
        "psq_l     12,24(%[b]),0,0\n\t"
        "ps_madds0 1,9,0,1\n\t"
        "psq_l     9,32(%[b]),0,0\n\t"
        "ps_madds0 2,10,0,2\n\t"
        "psq_l     10,40(%[b]),0,0\n\t"
        "ps_madds0 3,11,0,3\n\t"
        "psq_l     11,48(%[b]),0,0\n\t"
        "ps_madds0 4,12,0,4\n\t"
        "psq_l     12,56(%[b]),0,0\n\t"
        "ps_madds0 5,9,0,5\n\t"
        "ps_madds0 6,10,0,6\n\t"
        "ps_madds0 7,11,0,7\n\t"
        "ps_madds0 8,12,0,8\n\t"
        "bdnz      1b\n"
        "2:\n\t"
        "psq_st    1,0(%[out]),0,0\n\t"
        "psq_st    2,8(%[out]),0,0\n\t"
        "psq_st    3,16(%[out]),0,0\n\t"
        "psq_st    4,24(%[out]),0,0\n\t"
        "psq_st    5,32(%[out]),0,0\n\t"
        "psq_st    6,40(%[out]),0,0\n\t"
        "psq_st    7,48(%[out]),0,0\n\t"
        "psq_st    8,56(%[out]),0,0"
        : [b] "=&b"(b), [w] "=&b"(w), [list] "+b"(list)
        : [out] "b"(out), [b0] "b"(b0), [w0] "b"(w0), [n] "r"(n)
        : "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12",
          "cr0", "ctr", "memory");
}

// What CpuSkinVertices hands the loop: the vertices, and with the morph targets on (MORPH) the block's first vertex
// and a weight pair for each pair of slots -- each slot's target's weight times the slot's scale, so the int16s go
// straight to a move (0 for a target off)
struct OctSkinJob
{
    const VertexSkinned* mSrc;
    Vertex* mDst;
    uint32_t mNumVerts;
    const glm::mat4* mBones;
    bool mOneBone;
    const int16_t* mMorph;
    uint32_t mMorphStride;      // (bytes a vertex)
    const float* mPosW;         // (2 floats a pair of move slots)
    uint32_t mNumPos;           // (pairs of move slots read: at least 1)
    const float* mNrmW;
    uint32_t mNrmOfs;           // (bytes from a vertex's first pair to its first normal pair)
    uint32_t mNumNrm;           // (pairs of normal slots read: at least 1 with RENORM)
};

// CpuSkinVertices' loop, on the console: the same sums as the PC's -- the morph targets each by its weight added to
// the vertex's own, the normal made unit again when a target turns it, the bones' blended transform -- in pairs.
// MORPH: a morph target on; RENORM: one of those turns normals.
template<bool MORPH, bool RENORM>
static void OctSkinPairs(const OctSkinJob& job)
{
    const VertexSkinned* src = job.mSrc;
    Vertex* dst = job.mDst;
    const uint32_t numVerts = job.mNumVerts;
    const glm::mat4* bones = job.mBones;
    const bool oneBone = job.mOneBone;
    const char* morph = (const char*)job.mMorph;
    const uint32_t morphStride = job.mMorphStride;
    const float* posW = job.mPosW;
    const uint32_t numPos = job.mNumPos;
    const float* nrmW = job.mNrmW;
    const uint32_t nrmOfs = job.mNrmOfs;
    const uint32_t numNrm = job.mNumNrm;
    static_assert(sizeof(VertexSkinned) == 60 && sizeof(Vertex) == 40 && sizeof(glm::mat4) == 64,
                  "OctSkinPairs' asm reads and writes them at these offsets");

    // Each 32-byte cache line of the array zeroed just before it's written (dcbz) -- every byte of a Vertex is
    // written below, so nothing the zeroing clears is kept -- not read in from memory first, as a store to a line
    // not in the cache otherwise is; the lines only partly in the array (its ends) left alone. And the source a few
    // vertices ahead asked for (dcbt), a cache line at a time, PF_AHEAD vertices ahead: each line once, not a dcbt
    // for every vertex whether its line was asked for already or not. (Past the arrays' ends too, for the last few:
    // a dcbt only reads, and one that can't is nothing.)
    static_assert(sizeof(Vertex) == 2 * sizeof(glm::vec3) + 2 * sizeof(glm::vec2), "dcbz: a Vertex has no padding");
    uintptr_t nextLine = ((uintptr_t)dst + 31) & ~(uintptr_t)31;
    const uintptr_t dstEnd = (uintptr_t)(dst + numVerts);
#if OCT_SKIN_STORE_LINES
    uintptr_t doneLine = (uintptr_t)dst & ~(uintptr_t)31;
#endif
    const uint32_t PF_AHEAD = 6;
    uintptr_t srcPf = (uintptr_t)src & ~(uintptr_t)31;
    uintptr_t morphPf = (uintptr_t)morph & ~(uintptr_t)31;

    // (the Newton step's constants, and the normal too short to make unit: scalars. Made opaque, so the compiler
    //  keeps them in four FPRs through the loop -- the asm leaves f26-f31 alone -- not loads them again each vertex)
    double kHalf = 0.5, kThree = 3.0, kOne = 1.0, kTiny = 1e-8;
    __asm__("" : "+f"(kHalf), "+f"(kThree), "+f"(kOne), "+f"(kTiny));
    float blended[16] __attribute__((aligned(32)));            // (a vertex of more than one bone: its 3x4)

    uint32_t oldGqr = 0;
    if (MORPH)
    {
        __asm__ volatile("mfspr %0,%1" : "=r"(oldGqr) : "i"(OCT_GQR_S16));
        __asm__ volatile("mtspr %0,%1\n\tisync" : : "i"(OCT_GQR_S16), "r"(OCT_GQR_S16_VALUE) : "memory");
    }

    // (the first PF_AHEAD vertices asked for before the loop; then each vertex asks for the lines one more brings)
    while (srcPf < (uintptr_t)(src + PF_AHEAD))
    {
        __asm__ volatile("dcbt 0,%0" : : "r"(srcPf));
        srcPf += 32;
    }
    while (MORPH && morphPf < (uintptr_t)(morph + PF_AHEAD * morphStride))
    {
        __asm__ volatile("dcbt 0,%0" : : "r"(morphPf));
        morphPf += 32;
    }

    const VertexSkinned* sv = src;
    Vertex* dv = dst;
    const char* mv = morph;
    for (uint32_t i = 0; i < numVerts; ++i, ++sv, ++dv, mv += morphStride)
    {
        // (a vertex, 40 bytes, starts at most two lines, and one 60 bytes on brings at most two more to ask for)
        {
            const uintptr_t vertEnd = (uintptr_t)(dv + 1);
            if (nextLine < vertEnd && nextLine + 32 <= dstEnd)
            {
                __asm__ volatile("dcbz 0,%0" : : "r"(nextLine) : "memory");
                nextLine += 32;
                if (nextLine < vertEnd && nextLine + 32 <= dstEnd)
                {
                    __asm__ volatile("dcbz 0,%0" : : "r"(nextLine) : "memory");
                    nextLine += 32;
                }
            }
        }
        {
            const uintptr_t want = (uintptr_t)(sv + PF_AHEAD);
            if (srcPf < want)
            {
                __asm__ volatile("dcbt 0,%0" : : "r"(srcPf));
                srcPf += 32;
                if (srcPf < want)
                {
                    __asm__ volatile("dcbt 0,%0" : : "r"(srcPf));
                    srcPf += 32;
                }
            }
        }
        if (MORPH)
        {
            const uintptr_t want = (uintptr_t)(mv + PF_AHEAD * morphStride);
#pragma GCC unroll 1
            while (morphPf < want)
            {
                __asm__ volatile("dcbt 0,%0" : : "r"(morphPf));
                morphPf += 32;
            }
        }

        // the bones' blended transform, its top three rows only; the rest, weighted 0, left out (most vertices have
        // one or two bones). One bone of weight 1 (the most common: rigid parts): its matrix's columns as they are
        // -- times 1, the same numbers. (The weights' bits compared, not floats: -0 is 0 as ever.)
        uint32_t wb0, wb1, wb2, wb3;
        __builtin_memcpy(&wb0, &sv->mBoneWeights[0], 4);
        __builtin_memcpy(&wb1, &sv->mBoneWeights[1], 4);
        __builtin_memcpy(&wb2, &sv->mBoneWeights[2], 4);
        __builtin_memcpy(&wb3, &sv->mBoneWeights[3], 4);
        const char* cols = (const char*)(bones + sv->mBoneIndices[0]);
        if (!oneBone && !(wb0 == 0x3f800000u && ((wb1 | wb2 | wb3) << 1) == 0))
        {
            OctSkinBone list[3];
            uint32_t n = 0;
            if ((wb1 << 1) != 0) { list[n].mMat = (const char*)(bones + sv->mBoneIndices[1]); list[n].mWeight = &sv->mBoneWeights[1]; ++n; }
            if ((wb2 << 1) != 0) { list[n].mMat = (const char*)(bones + sv->mBoneIndices[2]); list[n].mWeight = &sv->mBoneWeights[2]; ++n; }
            if ((wb3 << 1) != 0) { list[n].mMat = (const char*)(bones + sv->mBoneIndices[3]); list[n].mWeight = &sv->mBoneWeights[3]; ++n; }
            OctSkinBlend(blended, cols, &sv->mBoneWeights[0], list, n);
            cols = (const char*)blended;
        }

        // the vertex, its morph targets each by its weight added to its own, through the bones
        const char* m = mv;
        const char* mn = mv + nrmOfs;
        const float* pw = posW;
        const float* nw = nrmW;
        uint32_t np = numPos, nn = numNrm;
#define OCT_SKIN_ASM_OPERANDS \
        : [m] "+b"(m), [mn] "+b"(mn), [pw] "+b"(pw), [nw] "+b"(nw), [np] "+r"(np), [nn] "+r"(nn) \
        : [src] "b"(sv), [dst] "b"(dv), [cols] "b"(cols), \
          [half] "f"(kHalf), [three] "f"(kThree), [one] "f"(kOne), [tiny] "f"(kTiny) \
        : OCT_SKIN_ASM_CLOBBERS
        if (MORPH && RENORM)
        {
            __asm__ volatile(OCT_SKIN_ASM_SRC
                             OCT_SKIN_ASM_MORPH("1", "m", "pw", "np", "1", "2", "7", "8", "9", "10", "11", "12", "13")
                             OCT_SKIN_ASM_MORPH("3", "mn", "nw", "nn", "3", "4", "0", "14", "15", "16", "17", "18", "19")
                             OCT_SKIN_ASM_SUM2
                             OCT_SKIN_ASM_OUT_RENORM
                             OCT_SKIN_ASM_OPERANDS);
        }
        else if (MORPH)
        {
            __asm__ volatile(OCT_SKIN_ASM_SRC
                             OCT_SKIN_ASM_MORPH("1", "m", "pw", "np", "1", "2", "7", "8", "9", "10", "11", "12", "13")
                             OCT_SKIN_ASM_SUM("1", "2", "11", "12", "13")
                             OCT_SKIN_ASM_OUT
                             OCT_SKIN_ASM_OPERANDS);
        }
        else
        {
            __asm__ volatile(OCT_SKIN_ASM_SRC
                             OCT_SKIN_ASM_OUT
                             OCT_SKIN_ASM_OPERANDS);
        }
#undef OCT_SKIN_ASM_OPERANDS

#if OCT_SKIN_STORE_LINES
        {
            const uintptr_t vertEnd = (uintptr_t)(dv + 1);         // (a vertex finishes at most two lines)
            if (doneLine + 32 <= vertEnd)
            {
                __asm__ volatile("dcbst 0,%0" : : "r"(doneLine) : "memory");
                doneLine += 32;
                if (doneLine + 32 <= vertEnd)
                {
                    __asm__ volatile("dcbst 0,%0" : : "r"(doneLine) : "memory");
                    doneLine += 32;
                }
            }
        }
#endif
    }

    if (MORPH)
        __asm__ volatile("mtspr %0,%1" : : "i"(OCT_GQR_S16), "r"(oldGqr) : "memory");
}
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
        std::swap(mSkinnedRigid, mSkinnedRigidBack);
#endif
        mSkinnedVertices.resize(mesh->GetNumVertices());
#if API_GX && !EDITOR
        if (CpuSkinBlended())                                   // (the rigid vertices left to the GPU)
            return;
        mSkinnedRigid = false;                                  // (every vertex skinned below)
#endif
        const std::vector<VertexSkinned>& verts = mesh->GetVertices();
        const uint32_t numVerts = mesh->GetNumVertices();
        const VertexSkinned* src = verts.data();
        Vertex* dst = mSkinnedVertices.data();
        const glm::mat4* boneMats = mBoneMatrices.data();

        // The morph targets, each by its weight, added to the vertex's own as the vertices go by: a target's moves read
        // straight alongside the vertex, in the same order as ever (target by target, from the vertex's own). The
        // parachute's four targets each move every one of its 2542 vertices, so a list of the vertices each moves, and
        // its sums kept aside then read back, only pulled more memory through the cache -- it was ~3.9 ms of a glide
        // on the GameCube, nearly all of it waiting on memory: eight streams a vertex (the mesh's vertex, four targets'
        // moves, two targets' normal changes, the scratch sums), none asked for ahead.
        const uint32_t numMorphs = glm::min<uint32_t>(mesh->GetNumMorphTargets(), uint32_t(mMorphWeights.size()));
        const bool oneBone = (mBoneInfluenceMode == BoneInfluenceMode::One);
#if PLATFORM_DOLPHIN && !EDITOR
        // (the console: OctSkinPairs, two floats an instruction, the targets from the mesh's block -- GetMorphBlock:
        //  every target's moves for a vertex side by side, in 16-bit fixed point -- by a weight for each slot, its
        //  target's times the slot's scale; the pairs of slots past the last one on not read, and none when all are off)
        const SkeletalMesh::MorphBlock& block = mesh->GetMorphBlock();
        static std::vector<float> sMorphW;
        const uint32_t numPosW = 2 * block.mNumPosPairs;
        sMorphW.assign(numPosW + 2 * block.mNumNrmPairs, 0.0f);
        uint32_t usedPos = 0, usedNrm = 0;
        for (uint32_t m = 0; m < numMorphs && m < block.mPosSlot.size(); ++m)
        {
            const float w = mMorphWeights[m];
            if (w == 0.0f)
                continue;
            const int32_t posSlot = block.mPosSlot[m];
            const int32_t nrmSlot = block.mNrmSlot[m];
            if (posSlot >= 0)
            {
                sMorphW[posSlot] = w * block.mPosScale[posSlot];
                usedPos = glm::max<uint32_t>(usedPos, uint32_t(posSlot) / 2 + 1);
            }
            if (nrmSlot >= 0)
            {
                sMorphW[numPosW + nrmSlot] = w * block.mNrmScale[nrmSlot];
                usedNrm = glm::max<uint32_t>(usedNrm, uint32_t(nrmSlot) / 2 + 1);
            }
        }
        const bool morphOn = (usedPos > 0 || usedNrm > 0) && block.mStride > 0 &&
                             block.mData.size() >= size_t(numVerts) * block.mStride;

        OctSkinJob job;
        job.mSrc = src;
        job.mDst = dst;
        job.mNumVerts = numVerts;
        job.mBones = boneMats;
        job.mOneBone = oneBone;
        job.mMorph = block.mData.data();
        job.mMorphStride = block.mStride * uint32_t(sizeof(int16_t));
        job.mPosW = sMorphW.data();
        job.mNumPos = glm::max<uint32_t>(usedPos, 1u);         // (the block has at least one pair of move slots)
        job.mNrmW = sMorphW.data() + numPosW;
        job.mNrmOfs = 12 * block.mNumPosPairs;
        job.mNumNrm = usedNrm;
        if (!morphOn)
            OctSkinPairs<false, false>(job);
        else if (usedNrm == 0)
            OctSkinPairs<true, false>(job);
        else
            OctSkinPairs<true, true>(job);                      // (a normal turned: made unit again)
#else
        bool morphNormals = false;
        struct ActiveMorph { const glm::vec3* mPos; const glm::vec3* mNrm; uint32_t mNumPos; uint32_t mNumNrm; float mWeight; };
        static std::vector<ActiveMorph> sMorphs;
        sMorphs.clear();
        for (uint32_t m = 0; m < numMorphs; ++m)
        {
            const float w = mMorphWeights[m];
            if (w == 0.0f)
                continue;
            const MorphTarget& morph = mesh->GetMorphTarget(m);
            const uint32_t numPos = glm::min<uint32_t>(uint32_t(morph.mPositions.size()), numVerts);
            const uint32_t numNrm = glm::min<uint32_t>(uint32_t(morph.mNormals.size()), numVerts);
            if (numPos == 0 && numNrm == 0)
                continue;
            sMorphs.push_back({ morph.mPositions.data(), morph.mNormals.data(), numPos, numNrm, w });
            morphNormals = morphNormals || (numNrm > 0);
        }
        const uint32_t numActive = uint32_t(sMorphs.size());
        const ActiveMorph* active = sMorphs.data();

        static_assert(MAX_BONE_INFLUENCES == 4, "Need to adjust this code or convert to loop.");
        for (uint32_t i = 0; i < numVerts; ++i)
        {
            const VertexSkinned& srcVert = src[i];
            Vertex& dstVert = dst[i];

            glm::vec3 pos = srcVert.mPosition;
            glm::vec3 nrm = srcVert.mNormal;
            if (numActive > 0)
            {
                for (uint32_t m = 0; m < numActive; ++m)
                {
                    const ActiveMorph& am = active[m];
                    if (i < am.mNumPos) pos += am.mPos[i] * am.mWeight;
                    if (i < am.mNumNrm) nrm += am.mNrm[i] * am.mWeight;
                }
                if (morphNormals)                                   // (the normal turned: made unit again)
                {
                    const float len2 = nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z;
                    if (len2 > 1e-8f) nrm *= OctRsqrt(len2);
                }
            }

            // the bones' blended transform, its top three rows only (columns c0..c2 the rotation-scale, c3 the move);
            // the rest, weighted 0, left out (most vertices have one or two bones). One bone of weight 1 (the most
            // common: rigid parts): its matrix's columns as they are -- times 1, the same numbers
            const glm::mat4& b0 = boneMats[srcVert.mBoneIndices[0]];
            const float w0 = oneBone ? 1.0f : srcVert.mBoneWeights[0];
            glm::vec3 c0, c1, c2, c3;
            if (oneBone || (w0 == 1.0f && srcVert.mBoneWeights[1] == 0.0f && srcVert.mBoneWeights[2] == 0.0f &&
                            srcVert.mBoneWeights[3] == 0.0f))
            {
                c0 = glm::vec3(b0[0]); c1 = glm::vec3(b0[1]); c2 = glm::vec3(b0[2]); c3 = glm::vec3(b0[3]);
            }
            else
            {
                c0 = glm::vec3(b0[0]) * w0; c1 = glm::vec3(b0[1]) * w0; c2 = glm::vec3(b0[2]) * w0; c3 = glm::vec3(b0[3]) * w0;
                for (uint32_t k = 1; k < 4; ++k)
                {
                    const float w = srcVert.mBoneWeights[k];
                    if (w == 0.0f) continue;
                    const glm::mat4& b = boneMats[srcVert.mBoneIndices[k]];
                    c0 += glm::vec3(b[0]) * w; c1 += glm::vec3(b[1]) * w; c2 += glm::vec3(b[2]) * w; c3 += glm::vec3(b[3]) * w;
                }
            }

            dstVert.mPosition = c0 * pos.x + c1 * pos.y + c2 * pos.z + c3;
            dstVert.mTexcoord0 = srcVert.mTexcoord0;
            dstVert.mTexcoord1 = srcVert.mTexcoord1;
            dstVert.mNormal = c0 * nrm.x + c1 * nrm.y + c2 * nrm.z;
        }
#endif

        GFX_UpdateSkeletalMeshCompVertexBuffer(this, mSkinnedVertices);
    }
}

#if API_GX && !EDITOR
// GX: the vertices that follow one bone at weight 1 -- most of a character's -- are put through their bone by the GPU
// (Graphics_GX.cpp, GxSkinLists): the array keeps them as the mesh has them, copied in once (each of the two arrays,
// then left: nothing else writes them), and only the rest are skinned here, as CpuSkinVertices would (the plan is
// only for a mesh with no morph targets). False when the GPU can't take them (GxSkinRigidPlan): CpuSkinVertices
// skins every vertex. (Called from it: the array to write is mSkinnedVertices, swapped and sized.)
bool SkeletalMesh3D::CpuSkinBlended()
{
    const uint16_t* blended = nullptr;
    const uint16_t* rigid = nullptr;
    uint32_t numBlended = 0, numRigid = 0;
    if (!GxSkinRigidPlan(this, blended, numBlended, rigid, numRigid))
        return false;

    SkeletalMesh* mesh = mSkeletalMesh.Get<SkeletalMesh>();
    const VertexSkinned* src = mesh->GetVertices().data();
    Vertex* dst = mSkinnedVertices.data();
#if PLATFORM_DOLPHIN && PROFILING_ENABLED
    gOctSkinVerts -= numRigid;                                  // (CpuSkinVertices counted them all)
#endif

    const bool fill = !mSkinnedRigid;
    if (fill)
    {
        for (uint32_t k = 0; k < numRigid; ++k)
        {
            const VertexSkinned& s = src[rigid[k]];
            Vertex& d = dst[rigid[k]];
            d.mPosition = s.mPosition;
            d.mTexcoord0 = s.mTexcoord0;
            d.mTexcoord1 = s.mTexcoord1;
            d.mNormal = s.mNormal;
        }
        mSkinnedRigid = true;
    }

    const glm::mat4* boneMats = mBoneMatrices.data();
    const bool oneBone = (mBoneInfluenceMode == BoneInfluenceMode::One);
    for (uint32_t k = 0; k < numBlended; ++k)
    {
        const VertexSkinned& srcVert = src[blended[k]];
        Vertex& dstVert = dst[blended[k]];
        const glm::mat4& b0 = boneMats[srcVert.mBoneIndices[0]];
        const float w0 = oneBone ? 1.0f : srcVert.mBoneWeights[0];
        glm::vec3 c0 = glm::vec3(b0[0]) * w0, c1 = glm::vec3(b0[1]) * w0, c2 = glm::vec3(b0[2]) * w0, c3 = glm::vec3(b0[3]) * w0;
        for (uint32_t i = 1; i < 4 && !oneBone; ++i)
        {
            const float w = srcVert.mBoneWeights[i];
            if (w == 0.0f) continue;
            const glm::mat4& b = boneMats[srcVert.mBoneIndices[i]];
            c0 += glm::vec3(b[0]) * w; c1 += glm::vec3(b[1]) * w; c2 += glm::vec3(b[2]) * w; c3 += glm::vec3(b[3]) * w;
        }
        const glm::vec3 pos = srcVert.mPosition;
        const glm::vec3 nrm = srcVert.mNormal;
        dstVert.mPosition = c0 * pos.x + c1 * pos.y + c2 * pos.z + c3;
        dstVert.mTexcoord0 = srcVert.mTexcoord0;
        dstVert.mTexcoord1 = srcVert.mTexcoord1;
        dstVert.mNormal = c0 * nrm.x + c1 * nrm.y + c2 * nrm.z;
    }

    // out of the data cache: the whole array when the rigid vertices were copied in, else just what was skinned
    if (fill)
        GFX_UpdateSkeletalMeshCompVertexBuffer(this, mSkinnedVertices);
    else
        GxFlushSkinnedVertices(dst, blended, numBlended);
    return true;
}
#endif
