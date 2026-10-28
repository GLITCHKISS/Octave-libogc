#pragma once

#include <string>

#include "Assets/Material.h"
#include "Asset.h"
#include "Vertex.h"
#include "Constants.h"

#include "Graphics/Graphics.h"
#include "Graphics/GraphicsTypes.h"
#include "Graphics/GraphicsConstants.h"

#include "Maths.h"

#if EDITOR
#include <assimp/scene.h>
#endif

struct Bone
{
    std::string mName;
    int32_t mIndex = -1;
    int32_t mParentIndex = -1;
    glm::mat4 mOffsetMatrix = { };
    glm::mat4 mInvOffsetMatrix = { };
};

struct PositionKey
{
    float mTime = 0.0f;
    glm::vec3 mValue = {};
};

struct RotationKey
{
    float mTime = 0.0f;
    glm::quat mValue = {};
};

struct ScaleKey
{
    float mTime = 0.0f;
    glm::vec3 mValue = {};
};

struct AnimEventKey
{
    float mTime = 0.0f;
    glm::vec3 mValue = {};
};

struct AnimEventTrack
{
    std::string mName;
    std::vector<AnimEventKey> mEventKeys;
};

struct AnimEvent
{
    SkeletalMesh3D* mNode = nullptr;
    std::string mName;
    std::string mAnimation;
    float mTime = 0.0f;
    glm::vec3 mValue = {};
};

struct Channel
{
    int32_t mBoneIndex = -1;
    std::vector<PositionKey> mPositionKeys;
    std::vector<RotationKey> mRotationKeys;
    std::vector<ScaleKey> mScaleKeys;
};

struct Animation
{
    std::string mName;
    float mDuration = 0.0f;
    float mTicksPerSecond = 0.0f;
    std::vector<Channel> mChannels;
    std::vector<AnimEventTrack> mEventTracks;
};

// A morph target (a glTF one: Blender's shape keys): how far each vertex moves, at full weight, from the mesh as it is
// (its rest), before the bones -- blended in by SkeletalMesh3D::SetMorphWeight
struct MorphTarget
{
    std::string mName;
    std::vector<glm::vec3> mPositions;          // each vertex's move (one per vertex)
    std::vector<glm::vec3> mNormals;            // each normal's change (one per vertex, or none: all zero, dropped
                                                //  at load -- SkeletalMesh::LoadStream)
};

class SkeletalMesh : public Asset
{
public:

    DECLARE_ASSET(SkeletalMesh, Asset);

    SkeletalMesh();
    ~SkeletalMesh();

    SkeletalMeshResource* GetResource();

    // Asset Interface
    virtual void LoadStream(Stream& stream, Platform platform) override;
    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void Create() override;
    virtual void Destroy() override;
    virtual bool Import(const std::string& path, ImportOptions* options) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;
    virtual glm::vec4 GetTypeColor() override;
    virtual const char* GetTypeName() override;
    virtual const char* GetTypeImportExt() override;

    class Material* GetMaterial();
    void SetMaterial(class Material* newMaterial);

    uint32_t GetNumIndices();
    uint32_t GetNumFaces();
    uint32_t GetNumVertices();

    const IndexType* GetIndices() const;

    int32_t FindBoneIndex(const std::string& name) const;
    const std::vector<Bone>& GetBones() const;
    const Bone& GetBone(int32_t index) const;
    uint32_t GetNumBones() const;

    glm::mat4 GetInvRootTransform() const;

    const std::vector<Animation>& GetAnimations() const;
    const Animation* GetAnimation(const char* name);

    // Get length of animation in seconds
    float GetAnimationDuration(const char* name);

    const std::vector<VertexSkinned>& GetVertices() const;

    uint32_t GetNumMorphTargets() const;
    const MorphTarget& GetMorphTarget(uint32_t index) const;
    int32_t FindMorphTarget(const std::string& name) const;

    // The GameCube's morph targets: all of their moves (and normal changes), a vertex at a time, in one array --
    // mStride floats a vertex, target m's move at mPosOfs[m] in it and its normal change at mNrmOfs[m] (-1: none).
    // Built when the mesh loads on the console, the targets' own arrays then let go: skinning a parachute read its four
    // targets' moves and two normal changes as six streams beside the vertices, and the Gekko, with only a few cache
    // misses in flight at once, waited on them (~2.5 ms a glide). Empty elsewhere: the PC reads the targets as ever.
    struct MorphBlock
    {
        std::vector<float> mData;
        uint32_t mStride = 0;
        std::vector<int32_t> mPosOfs;
        std::vector<int32_t> mNrmOfs;
    };
    const MorphBlock& GetMorphBlock() const { return mMorphBlock; }

    void FinalizeBoneTransforms(std::vector<glm::mat4>& inoutTransforms);

    void CopyBindPose(std::vector<glm::mat4>& outTransforms);

    Bounds GetBounds() const;

    const glm::mat4 GetBindPoseMatrix(int32_t boneIndex) const;

    SkeletalMesh* GetAnimationLookupMesh();
    void SetAnimationLookupMesh(SkeletalMesh* lookupMesh);

    static bool HandlePropChange(Datum* datum, uint32_t index, const void* newValue);

private:

    void InitBindPose();
    void ComputeBounds();

    MaterialRef mMaterial;
    SkeletalMeshRef mAnimationLookupMesh;
    uint32_t mNumVertices;
    uint32_t mNumIndices;
    uint32_t mNumUvMaps;

    std::vector<Bone> mBones;
    std::vector<Animation> mAnimations;
    std::vector<VertexSkinned> mVertices;
    std::vector<MorphTarget> mMorphTargets;
    MorphBlock mMorphBlock;                     // (the console's: GetMorphBlock)

    glm::mat4 mInvRootTransform;
    std::vector<glm::mat4> mBindPoseMatrices;
    std::vector<IndexType> mIndices;

    Bounds mBounds;
    float mBoundsScale = 1.1f;

    // Graphics Resource
    SkeletalMeshResource mResource;

#if EDITOR
public:
    // The morph targets named as the file names them (a glTF mesh's extras.targetNames: assimp leaves them unnamed)
    void NameMorphTargets(const std::string& path);

    void Create(const aiScene& scene,
        const aiMesh& meshData,
        std::vector<Material>* materials = nullptr);

    void SetupBoneHierarchy(
        const aiNode& node,
        const aiMesh& meshData,
        std::vector<uint8_t>& boneIndices,
        std::vector<float>& boneWeights,
        int32_t parentBoneIndex);

    void SetupAnimations(const aiScene& scene);
    void SetupResource(const aiMesh& meshData,
        const std::vector<float>& boneWeights,
        const std::vector<uint8_t>& boneIndices);
#endif // EDITOR
};
