#pragma once

#include "Constants.h"
#include "Maths.h"

enum class VertexType
{
    Vertex,
    VertexInstanceColor,
    VertexColor,
    VertexColorInstanceColor,
    VertexUI,
    VertexLine,
    VertexSkinned,
    VertexParticle,
    Max
};

struct Vertex
{
    glm::vec3 mPosition;
    glm::vec2 mTexcoord0;
    glm::vec2 mTexcoord1;
    glm::vec3 mNormal;
};

struct VertexColor
{
    glm::vec3 mPosition;
    glm::vec2 mTexcoord0;
    glm::vec2 mTexcoord1;
    glm::vec3 mNormal;
    uint32_t mColor;
};

struct VertexUI
{
    glm::vec2 mPosition;
    glm::vec2 mTexcoord;
    uint32_t mColor;
};

struct VertexLine
{
    glm::vec3 mPosition;
    uint32_t mColor;
};

struct VertexSkinned
{
    glm::vec3 mPosition;
    glm::vec2 mTexcoord0;
    glm::vec2 mTexcoord1;
    glm::vec3 mNormal;
    uint8_t mBoneIndices[MAX_BONE_INFLUENCES];
    float mBoneWeights[MAX_BONE_INFLUENCES];
};

struct VertexParticle
{
    glm::vec3 mPosition;
    glm::vec2 mTexcoord;
    uint32_t mColor;
};

// What CPU skinning writes for each vertex: SkeletalMesh3D's two skinned arrays (GetSkinnedVertices). On GX only the
// position and the normal -- the texture coordinates never move with the bones, so the GPU reads them straight from
// the mesh's own vertices (SkeletalMesh::GetVertices, flushed once when it's created: GFX_CreateSkeletalMeshResource).
// 24 bytes a vertex, not 40: a character of 2468 vertices keeps two arrays of ~59 KB, not ~99 KB, and the skinning no
// longer copies 16 bytes a vertex that never change. Everywhere else (the editor, Vulkan, C3D) it's the whole Vertex,
// as the vertex buffers there take it.
#if API_GX && !EDITOR
struct VertexSkinOut
{
    glm::vec3 mPosition;
    glm::vec3 mNormal;
};
typedef VertexSkinOut CpuSkinnedVertex;
#else
typedef Vertex CpuSkinnedVertex;
#endif
