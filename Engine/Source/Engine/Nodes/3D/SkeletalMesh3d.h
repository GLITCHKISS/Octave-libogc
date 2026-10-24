#pragma once

#include "Nodes/3D/Mesh3d.h"
#include "AssetRef.h"
#include "Vertex.h"

enum class BoneInfluenceMode
{
    One,
    Four,
    Num
};

enum class AnimationUpdateMode
{
    AlwaysUpdateTimeAndBones,
    AlwaysUpdateTime,
    OnlyUpdateWhenRendered,

    Count
};

class SkeletalMesh;
struct AnimEvent;
struct Channel;
struct Animation;

struct ActiveAnimation
{
    std::string mName;
    float mTime = 0.0f;
    float mSpeed = 1.0f;
    float mWeight = 0.0f;
    int32_t mSlot = 0;
    bool mLoop = false;
};

struct QueuedAnimation
{
    std::string mName;
    std::string mDependentAnim;
    float mTime = 0.0f;
    float mSpeed = 1.0f;;
    float mWeight = 0.0f;
    bool mLoop = false;
    int32_t mSlot = -1;
};

typedef void(*AnimEventHandlerFP)(const AnimEvent& animEvent);

// Dynamic bones: chains of bones simulated after the animation each frame (cloth, tails, hair). Each joint of a chain
// is a particle in world space that keeps its momentum (verlet), is damped, falls under gravity and is pulled back
// toward where the animation puts it (the stiffness); the chain keeps its lengths, its first joint stays where the
// animation has it, and each bone is then turned to point at the next particle. Colliders -- capsules fixed to bones,
// given in the skeleton's rest pose -- push the particles out of them.
struct BoneChain
{
    std::vector<int32_t> mBones;            // root first, each the next's parent
    std::vector<glm::vec3> mPos;            // the particles, world space: each bone's head, then the last's tip
    std::vector<glm::vec3> mPrev;
    glm::vec3 mTipLocal = {};               // the last bone's tip, in its own space (rest)
    float mStiffness = 0.2f;                // per 60th of a second: how much of the way back to the animation
    float mDamping = 0.1f;
    glm::vec3 mGravity = { 0.0f, -9.8f, 0.0f };
    float mRadius = 0.01f;                  // the particles' own thickness, kept off colliders
    bool mReset = true;
};

struct BoneCollider
{
    int32_t mBone = -1;
    glm::vec3 mA = {};                      // in the bone's own space (from the rest pose)
    glm::vec3 mB = {};
    float mRadius = 0.05f;
};

class SkeletalMesh3D : public Mesh3D
{
public:

    DECLARE_NODE(SkeletalMesh3D, Mesh3D);

    SkeletalMesh3D();
    ~SkeletalMesh3D();

    virtual const char* GetTypeName() const override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;

    virtual void Create() override;
    virtual void Destroy() override;
    SkeletalMeshCompResource* GetResource();

    virtual void Tick(float deltaTime) override;
    virtual void EditorTick(float deltaTime) override;

    virtual bool IsStaticMesh3D() const override;
    virtual bool IsSkeletalMesh3D() const override;

    void SetSkeletalMesh(SkeletalMesh* skeletalMesh);
    SkeletalMesh* GetSkeletalMesh();

    void PlayAnimation(const char* animName, bool loop, float speed = 1.0f, float weight = 1.0f, int32_t priority = -1);
    void QueueAnimation(const char* animName, bool loop, const char* targetAnim = nullptr, float speed = 1.0f, float weight = 1.0f, int32_t priority = -1);
    void StopAnimation(const char* animName, bool cancelQueued = false);
    void StopAllAnimations(bool cancelQueued = false);
    void CancelQueuedAnimation(const char* animName);
    void CancelAllQueuedAnimations();
    bool IsAnimationPlaying(const char* animName);
    void ResetAnimation();
    float GetAnimationSpeed() const;
    void SetAnimationSpeed(float speed);

    void SetAnimationPaused(bool paused);
    bool IsAnimationPaused() const;

    void SetInheritPose(bool inherit);
    bool IsInheritPoseEnabled() const;

    void SetBoundsRadiusOverride(float radius);
    float GetBoundsRadiusOverride() const;

    bool HasAnimatedThisFrame() const;

    ActiveAnimation* FindActiveAnimation(const char* animName);
    std::vector<ActiveAnimation>& GetActiveAnimations();

    QueuedAnimation* FindQueuedAnimation(const char* animName, const char* dependName = nullptr);
    std::vector<QueuedAnimation>& GetQueuedAnimations();

    glm::mat4 GetBoneTransform(const std::string& name) const;
    glm::vec3 GetBonePosition(const std::string& name) const;
    glm::quat GetBoneRotationQuat(const std::string& name) const;
    glm::vec3 GetBoneRotationEuler(const std::string& name) const;
    glm::vec3 GetBoneScale(const std::string& name) const;

    glm::mat4 GetBoneTransform(int32_t index) const;
    glm::vec3 GetBonePosition(int32_t boneIndex) const;
    glm::quat GetBoneRotationQuat(int32_t boneIndex) const;
    glm::vec3 GetBoneRotationEuler(int32_t boneIndex) const;
    glm::vec3 GetBoneScale(int32_t boneIndex) const;

    void SetBoneTransform(int32_t boneIndex, const glm::mat4& transform);
    void SetBonePosition(int32_t boneIndex, glm::vec3 position);
    void SetBoneRotation(int32_t boneIndex, glm::vec3 rotation);
    void SetBoneScale(int32_t boneIndex, glm::vec2 scale);

    uint32_t GetNumBones() const;
    BoneInfluenceMode GetBoneInfluenceMode() const;

    AnimationUpdateMode GetAnimationUpdateMode() const;
    void SetAnimationUpdateMode(AnimationUpdateMode mode);

    Vertex* GetSkinnedVertices();
    uint32_t GetNumSkinnedVertices();

    virtual Material* GetMaterial() override;
    virtual void Render() override;

    void UpdateAnimation(float deltaTime, bool updateBones);

    virtual Bounds GetLocalBounds() const override;

    int32_t FindBoneIndex(const std::string& name) const;

    // Dynamic bones (see BoneChain). AddBoneChain: the bone and its descendants down a single line; returns the chain's
    // index, or -1. AddBoneCollider: a capsule fixed to a bone, its ends given in the rest pose's model space (a sphere
    // when they're the same point). ResetBoneDynamics: the particles back on the animation (after a teleport).
    int32_t AddBoneChain(const std::string& rootBone, float stiffness, float damping, glm::vec3 gravity, float radius);
    void AddBoneCollider(const std::string& bone, glm::vec3 restA, glm::vec3 restB, float radius);
    void ResetBoneDynamics();
    void ClearBoneDynamics();
    glm::vec3 GetBoneRestPosition(int32_t boneIndex) const;     // its head in the rest pose, model space

    void SetAnimEventHandler(AnimEventHandlerFP handler);
    AnimEventHandlerFP GetAnimEventHandler();
    void SetScriptAnimEventHandler(const ScriptFunc& func);

protected:

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

    void TickCommon(float deltaTime);

    glm::vec3 InterpolateScale(float time, const Channel& channel);
    glm::quat InterpolateRotation(float time, const Channel& channel);
    glm::vec3 InterpolatePosition(float time, const Channel& channel);
    void DetectTriggeredAnimEvents(
        const Animation& animation,
        float prevTickTime,
        float tickTime,
        float animationSpeed,
        std::vector<AnimEvent>& outEvents);

    uint32_t FindScaleIndex(float time, const Channel& channel);
    uint32_t FindRotationIndex(float time, const Channel& channel);
    uint32_t FindPositionIndex(float time, const Channel& channel);

    void UpdateAttachedChildren(float deltaTime);
    void CpuSkinVertices();
    void SimulateBoneDynamics(float deltaTime);

    SkeletalMeshRef mSkeletalMesh;
    std::vector<glm::mat4> mBoneMatrices;
    std::vector<Vertex> mSkinnedVertices; // Used by CPU skinning only.
    std::vector<Vertex> mSkinnedVerticesBack; // (GX: the other of the two, in turn -- CpuSkinVertices)

    ScriptableFP<AnimEventHandlerFP> mAnimEventHandler;
    std::string mDefaultAnimation;
    float mAnimationSpeed = 1.0f;
    std::vector<ActiveAnimation> mActiveAnimations;
    std::vector<QueuedAnimation> mQueuedAnimations;
    float mBoundsRadiusOverride = 0.0f;
    std::vector<BoneChain> mBoneChains;
    std::vector<BoneCollider> mBoneColliders;
    bool mAnimationPaused;
    bool mRevertToBindPose;
    bool mInheritPose;
    bool mHasAnimatedThisFrame;

    BoneInfluenceMode mBoneInfluenceMode;
    AnimationUpdateMode mAnimationUpdateMode;

    // Graphics Resource
    SkeletalMeshCompResource mResource;
};