#include "LuaBindings/SkeletalMesh3d_Lua.h"
#include "LuaBindings/Mesh3d_Lua.h"
#include "LuaBindings/Asset_Lua.h"
#include "LuaBindings/SkeletalMesh_Lua.h"
#include "LuaBindings/Vector_Lua.h"

#include "Asset.h"
#include "AssetManager.h"
#include "Assets/SkeletalMesh.h"

int SkeletalMesh3D_Lua::SetSkeletalMesh(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    SkeletalMesh* skMesh = nullptr;
    if (!lua_isnil(L, 2))
    {
        skMesh = CHECK_SKELETAL_MESH(L, 2);
    }

    comp->SetSkeletalMesh(skMesh);

    return 0;
}

int SkeletalMesh3D_Lua::GetSkeletalMesh(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);

    SkeletalMesh* skMesh = comp->GetSkeletalMesh();

    Asset_Lua::Create(L, skMesh);
    return 1;
}

int SkeletalMesh3D_Lua::PlayAnimation(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* animName = CHECK_STRING(L, 2);
    bool loop = false;
    float speed = 1.0f;
    float weight = 1.0f;
    int32_t slot = -1;

    if (lua_isinteger(L, 3))
    {
        // New signature, slot is the next parameter
        if (!lua_isnone(L, 3)) { slot = CHECK_INTEGER(L, 3); }
        if (!lua_isnone(L, 4)) { loop = CHECK_BOOLEAN(L, 4); }
        if (!lua_isnone(L, 5)) { speed = CHECK_NUMBER(L, 5); }
        if (!lua_isnone(L, 6)) { weight = CHECK_NUMBER(L, 6); }
    }
    else if (lua_isboolean(L, 3))
    {
        // Old signature, kept for backwards compatibility
        if (!lua_isnone(L, 3)) { loop = CHECK_BOOLEAN(L, 3); }
        if (!lua_isnone(L, 4)) { speed = CHECK_NUMBER(L, 4); }
        if (!lua_isnone(L, 5)) { weight = CHECK_NUMBER(L, 5); }
        if (!lua_isnone(L, 6)) { slot = CHECK_INTEGER(L, 6); }
    }

    comp->PlayAnimation(animName, loop, speed, weight, slot);

    return 0;
}

int SkeletalMesh3D_Lua::StopAnimation(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* animName = CHECK_STRING(L, 2);
    bool cancelQueued = false;

    if (!lua_isnone(L, 3)) { cancelQueued = CHECK_BOOLEAN(L, 3); }

    comp->StopAnimation(animName, cancelQueued);

    return 0;
}

int SkeletalMesh3D_Lua::StopAllAnimations(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    bool cancelQueued = false;

    if (!lua_isnone(L, 2)) { cancelQueued = CHECK_BOOLEAN(L, 2); }

    comp->StopAllAnimations(cancelQueued);

    return 0;
}

int SkeletalMesh3D_Lua::IsAnimationPlaying(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* animName = CHECK_STRING(L, 2);

    bool ret = comp->IsAnimationPlaying(animName);

    lua_pushboolean(L, ret);
    return 1;
}

int SkeletalMesh3D_Lua::QueueAnimation(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* animName = CHECK_STRING(L, 2);
    bool loop = false;
    const char* dependentAnimName = nullptr;

    float speed = 1.0f;
    float weight = 1.0f;
    int32_t slot = -1;


    if (lua_isstring(L, 3))
    {
        // New signature.
        if (!lua_isnone(L, 3) && !lua_isnil(L, 3)) { dependentAnimName = CHECK_STRING(L, 3); }
        if (!lua_isnone(L, 4)) { slot = CHECK_INTEGER(L, 4); }
        if (!lua_isnone(L, 5)) { loop = CHECK_BOOLEAN(L, 5) };
        if (!lua_isnone(L, 6)) { speed = CHECK_NUMBER(L, 6); }
        if (!lua_isnone(L, 7)) { weight = CHECK_NUMBER(L, 7); }

    }
    else if (lua_isboolean(L, 3))
    {
        // Old signature, kept for backwards compatibility.
        if (!lua_isnone(L, 3)) { loop = CHECK_BOOLEAN(L, 3) };
        if (!lua_isnone(L, 4) && !lua_isnil(L, 4)) { dependentAnimName = CHECK_STRING(L, 4); }
        if (!lua_isnone(L, 5)) { speed = CHECK_NUMBER(L, 5); }
        if (!lua_isnone(L, 6)) { weight = CHECK_NUMBER(L, 6); }
        if (!lua_isnone(L, 7)) { slot = CHECK_INTEGER(L, 7); }
    }

    comp->QueueAnimation(animName, loop, dependentAnimName, speed, weight, slot);

    return 0;
}

int SkeletalMesh3D_Lua::CancelQueuedAnimation(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* animName = CHECK_STRING(L, 2);

    comp->CancelQueuedAnimation(animName);

    return 0;
}

int SkeletalMesh3D_Lua::CancelAllQueuedAnimations(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);

    comp->CancelAllQueuedAnimations();

    return 0;
}

int SkeletalMesh3D_Lua::SetInheritPose(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    bool value = CHECK_BOOLEAN(L, 2);

    comp->SetInheritPose(value);

    return 0;
}

int SkeletalMesh3D_Lua::IsInheritPoseEnabled(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);

    bool ret = comp->IsInheritPoseEnabled();

    lua_pushboolean(L, ret);
    return 1;
}

int SkeletalMesh3D_Lua::ResetAnimation(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);

    comp->ResetAnimation();

    return 0;
}

int SkeletalMesh3D_Lua::GetAnimationSpeed(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);

    float ret = comp->GetAnimationSpeed();

    lua_pushnumber(L, ret);
    return 1;
}

int SkeletalMesh3D_Lua::SetAnimationSpeed(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    float speed = CHECK_NUMBER(L, 2);

    comp->SetAnimationSpeed(speed);

    return 0;
}

int SkeletalMesh3D_Lua::SetAnimationPaused(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    bool value = CHECK_BOOLEAN(L, 2);

    comp->SetAnimationPaused(value);

    return 0;
}

int SkeletalMesh3D_Lua::IsAnimationPaused(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);

    bool ret = comp->IsAnimationPaused();

    lua_pushboolean(L, ret);
    return 1;
}

int SkeletalMesh3D_Lua::GetBonePosition(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* boneName = CHECK_STRING(L, 2);

    glm::vec3 ret = comp->GetBonePosition(boneName);

    Vector_Lua::Create(L, ret);
    return 1;
}

int SkeletalMesh3D_Lua::GetBoneRotation(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* boneName = CHECK_STRING(L, 2);

    glm::vec3 ret = comp->GetBoneRotationEuler(boneName);

    Vector_Lua::Create(L, ret);
    return 1;
}

int SkeletalMesh3D_Lua::GetBoneScale(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* boneName = CHECK_STRING(L, 2);

    glm::vec3 ret = comp->GetBoneScale(boneName);

    Vector_Lua::Create(L, ret);
    return 1;
}

int SkeletalMesh3D_Lua::SetBonePosition(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* boneName = CHECK_STRING(L, 2);
    glm::vec3 value = CHECK_VECTOR(L, 3);

    int32_t boneIndex = comp->FindBoneIndex(boneName);
    comp->SetBonePosition(boneIndex, value);

    return 0;
}

int SkeletalMesh3D_Lua::SetBoneRotation(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* boneName = CHECK_STRING(L, 2);
    glm::vec3 value = CHECK_VECTOR(L, 3);

    int32_t boneIndex = comp->FindBoneIndex(boneName);
    comp->SetBoneRotation(boneIndex, value);

    return 0;
}

// AddBoneChain(rootBone, [stiffness 0.2], [damping 0.1], [gravity Vec(0,-9.8,0)], [radius 0.01], [inertia 1],
// [limit 0]) -> chain index or -1
int SkeletalMesh3D_Lua::AddBoneChain(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* boneName = CHECK_STRING(L, 2);
    float stiffness = 0.2f;
    float damping = 0.1f;
    glm::vec3 gravity = { 0.0f, -9.8f, 0.0f };
    float radius = 0.01f;
    if (!lua_isnone(L, 3)) { stiffness = CHECK_NUMBER(L, 3); }
    if (!lua_isnone(L, 4)) { damping = CHECK_NUMBER(L, 4); }
    if (!lua_isnone(L, 5)) { gravity = CHECK_VECTOR(L, 5); }
    if (!lua_isnone(L, 6)) { radius = CHECK_NUMBER(L, 6); }
    float inertia = 1.0f;
    float limit = 0.0f;
    if (!lua_isnone(L, 7)) { inertia = CHECK_NUMBER(L, 7); }
    if (!lua_isnone(L, 8)) { limit = CHECK_NUMBER(L, 8); }

    int32_t index = comp->AddBoneChain(boneName, stiffness, damping, gravity, radius, inertia, limit);

    lua_pushinteger(L, (int)index);
    return 1;
}

// AddBoneCollider(bone, a, b, radius): a capsule fixed to the bone; its ends each a Vec (rest pose, model space) or a
// bone's name (that bone's head in the rest pose)
int SkeletalMesh3D_Lua::AddBoneCollider(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* boneName = CHECK_STRING(L, 2);
    glm::vec3 ends[2];
    for (int i = 0; i < 2; ++i)
    {
        if (lua_type(L, 3 + i) == LUA_TSTRING)
            ends[i] = comp->GetBoneRestPosition(comp->FindBoneIndex(lua_tostring(L, 3 + i)));
        else
            ends[i] = CHECK_VECTOR(L, 3 + i);
    }
    float radius = CHECK_NUMBER(L, 5);

    lua_pushinteger(L, comp->AddBoneCollider(boneName, ends[0], ends[1], radius));   // (its index: SetBoneColliderRadius)
    return 1;
}

int SkeletalMesh3D_Lua::SetBoneDynamicsInertia(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    float scale = CHECK_NUMBER(L, 2);
    comp->SetBoneDynamicsInertia(scale);
    return 0;
}

// SetMorphWeight(target, weight): target a morph target's name or its index (from 0); false if it has none such
int SkeletalMesh3D_Lua::SetMorphWeight(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    float weight = CHECK_NUMBER(L, 3);
    bool found = false;
    if (lua_isinteger(L, 2))
    {
        int32_t index = (int32_t)lua_tointeger(L, 2);
        found = index >= 0 && index < int32_t(comp->GetNumMorphTargets());
        comp->SetMorphWeight(index, weight);
    }
    else
    {
        const char* name = CHECK_STRING(L, 2);
        found = comp->SetMorphWeight(std::string(name), weight);
    }
    lua_pushboolean(L, found);
    return 1;
}

int SkeletalMesh3D_Lua::GetMorphWeight(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    int32_t index = (int32_t)CHECK_INTEGER(L, 2);
    lua_pushnumber(L, comp->GetMorphWeight(index));
    return 1;
}

int SkeletalMesh3D_Lua::GetNumMorphTargets(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    lua_pushinteger(L, (lua_Integer)comp->GetNumMorphTargets());
    return 1;
}

int SkeletalMesh3D_Lua::SetBoneColliderRadius(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    int32_t index = (int32_t)CHECK_INTEGER(L, 2);
    float radius = CHECK_NUMBER(L, 3);
    comp->SetBoneColliderRadius(index, radius);
    return 0;
}

int SkeletalMesh3D_Lua::AddBoneChainLink(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* rootA = CHECK_STRING(L, 2);
    const char* rootB = CHECK_STRING(L, 3);
    lua_pushboolean(L, comp->AddBoneChainLink(rootA, rootB));
    return 1;
}

int SkeletalMesh3D_Lua::ResetBoneDynamics(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    comp->ResetBoneDynamics();
    return 0;
}

int SkeletalMesh3D_Lua::ClearBoneDynamics(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    comp->ClearBoneDynamics();
    return 0;
}

int SkeletalMesh3D_Lua::SetBoneScale(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    const char* boneName = CHECK_STRING(L, 2);
    glm::vec3 value = CHECK_VECTOR(L, 3);

    int32_t boneIndex = comp->FindBoneIndex(boneName);
    comp->SetBoneScale(boneIndex, value);

    return 0;
}

int SkeletalMesh3D_Lua::GetNumBones(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);

    uint32_t numBones = comp->GetNumBones();

    lua_pushinteger(L, (int)numBones);
    return 1;
}

int SkeletalMesh3D_Lua::SetAnimEventHandler(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    CHECK_FUNCTION(L, 2);
    ScriptFunc func(L, 2);

    comp->SetScriptAnimEventHandler(func);

    return 0;
}

int SkeletalMesh3D_Lua::SetBoundsRadiusOverride(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);
    float radius = CHECK_NUMBER(L, 2);

    comp->SetBoundsRadiusOverride(radius);

    return 0;
}

int SkeletalMesh3D_Lua::GetBoundsRadiusOverride(lua_State* L)
{
    SkeletalMesh3D* comp = CHECK_SKELETAL_MESH_3D(L, 1);

    float radius = comp->GetBoundsRadiusOverride();

    lua_pushnumber(L, radius);
    return 1;
}

void SkeletalMesh3D_Lua::Bind()
{
    lua_State* L = GetLua();
    int mtIndex = CreateClassMetatable(
        SKELETAL_MESH_3D_LUA_NAME,
        SKELETAL_MESH_3D_LUA_FLAG,
        MESH_3D_LUA_NAME);

    Node_Lua::BindCommon(L, mtIndex);

    REGISTER_TABLE_FUNC(L, mtIndex, SetSkeletalMesh);

    REGISTER_TABLE_FUNC(L, mtIndex, GetSkeletalMesh);

    REGISTER_TABLE_FUNC(L, mtIndex, PlayAnimation);

    REGISTER_TABLE_FUNC(L, mtIndex, StopAnimation);

    REGISTER_TABLE_FUNC(L, mtIndex, StopAllAnimations);

    REGISTER_TABLE_FUNC(L, mtIndex, IsAnimationPlaying);

    REGISTER_TABLE_FUNC(L, mtIndex, QueueAnimation);

    REGISTER_TABLE_FUNC(L, mtIndex, CancelQueuedAnimation);

    REGISTER_TABLE_FUNC(L, mtIndex, CancelAllQueuedAnimations);

    REGISTER_TABLE_FUNC(L, mtIndex, SetInheritPose);

    REGISTER_TABLE_FUNC(L, mtIndex, IsInheritPoseEnabled);

    REGISTER_TABLE_FUNC(L, mtIndex, ResetAnimation);

    REGISTER_TABLE_FUNC(L, mtIndex, GetAnimationSpeed);

    REGISTER_TABLE_FUNC(L, mtIndex, SetAnimationSpeed);

    REGISTER_TABLE_FUNC(L, mtIndex, SetAnimationPaused);

    REGISTER_TABLE_FUNC(L, mtIndex, IsAnimationPaused);

    REGISTER_TABLE_FUNC(L, mtIndex, GetBonePosition);

    REGISTER_TABLE_FUNC(L, mtIndex, GetBoneRotation);

    REGISTER_TABLE_FUNC(L, mtIndex, GetBoneScale);

    REGISTER_TABLE_FUNC(L, mtIndex, SetBonePosition);

    REGISTER_TABLE_FUNC(L, mtIndex, SetBoneRotation);
    REGISTER_TABLE_FUNC(L, mtIndex, AddBoneChain);
    REGISTER_TABLE_FUNC(L, mtIndex, AddBoneCollider);
    REGISTER_TABLE_FUNC(L, mtIndex, AddBoneChainLink);
    REGISTER_TABLE_FUNC(L, mtIndex, SetBoneColliderRadius);
    REGISTER_TABLE_FUNC(L, mtIndex, SetBoneDynamicsInertia);
    REGISTER_TABLE_FUNC(L, mtIndex, SetMorphWeight);
    REGISTER_TABLE_FUNC(L, mtIndex, GetMorphWeight);
    REGISTER_TABLE_FUNC(L, mtIndex, GetNumMorphTargets);
    REGISTER_TABLE_FUNC(L, mtIndex, ResetBoneDynamics);
    REGISTER_TABLE_FUNC(L, mtIndex, ClearBoneDynamics);

    REGISTER_TABLE_FUNC(L, mtIndex, SetBoneScale);

    REGISTER_TABLE_FUNC(L, mtIndex, GetNumBones);

    REGISTER_TABLE_FUNC(L, mtIndex, SetAnimEventHandler);

    REGISTER_TABLE_FUNC(L, mtIndex, SetBoundsRadiusOverride);

    REGISTER_TABLE_FUNC(L, mtIndex, GetBoundsRadiusOverride);

    lua_pop(L, 1);
    OCT_ASSERT(lua_gettop(L) == 0);
}
