#pragma once

#include "EngineTypes.h"
#include "Log.h"
#include "Engine.h"

#include "Nodes/3D/SkeletalMesh3d.h"

#include "LuaBindings/Node_Lua.h"
#include "LuaBindings/LuaUtils.h"

#if LUA_ENABLED

#define SKELETAL_MESH_3D_LUA_NAME "SkeletalMesh3D"
#define SKELETAL_MESH_3D_LUA_FLAG "cfSkeletalMesh3D"
#define CHECK_SKELETAL_MESH_3D(L, arg) static_cast<SkeletalMesh3D*>(CheckNodeLuaType(L, arg, SKELETAL_MESH_3D_LUA_NAME, SKELETAL_MESH_3D_LUA_FLAG));

struct SkeletalMesh3D_Lua
{
    static int SetSkeletalMesh(lua_State* L);
    static int GetSkeletalMesh(lua_State* L);
    static int PlayAnimation(lua_State* L);
    static int StopAnimation(lua_State* L);
    static int StopAllAnimations(lua_State* L);
    static int IsAnimationPlaying(lua_State* L);
    static int QueueAnimation(lua_State* L);
    static int CancelQueuedAnimation(lua_State* L);
    static int CancelAllQueuedAnimations(lua_State* L);
    static int SetInheritPose(lua_State* L);
    static int IsInheritPoseEnabled(lua_State* L);
    static int ResetAnimation(lua_State* L);
    static int GetAnimationSpeed(lua_State* L);
    static int SetAnimationSpeed(lua_State* L);
    static int SetAnimationPaused(lua_State* L);
    static int IsAnimationPaused(lua_State* L);
    static int GetBonePosition(lua_State* L);
    static int GetBoneRotation(lua_State* L);
    static int GetBoneScale(lua_State* L);
    static int SetBonePosition(lua_State* L);
    static int SetBoneRotation(lua_State* L);
    static int AddBoneChain(lua_State* L);
    static int AddBoneCollider(lua_State* L);
    static int AddBoneChainLink(lua_State* L);
    static int SetBoneColliderRadius(lua_State* L);
    static int SetBoneDynamicsInertia(lua_State* L);
    static int SetBoneChainGravity(lua_State* L);
    static int SetMorphWeight(lua_State* L);
    static int GetMorphWeight(lua_State* L);
    static int GetNumMorphTargets(lua_State* L);
    static int ResetBoneDynamics(lua_State* L);
    static int ClearBoneDynamics(lua_State* L);
    static int SetBoneScale(lua_State* L);
    static int ClearBoneScale(lua_State* L);
    static int GetNumBones(lua_State* L);
    static int SetAnimEventHandler(lua_State* L);
    static int SetBoundsRadiusOverride(lua_State* L);
    static int GetBoundsRadiusOverride(lua_State* L);

    static void Bind();
};

#endif