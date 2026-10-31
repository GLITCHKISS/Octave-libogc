#include "LuaBindings/WaveGrid_Lua.h"
#include "LuaBindings/StaticMesh3d_Lua.h"
#include "Log.h"

#if LUA_ENABLED

static float FieldOr(lua_State* L, int table, const char* key, float fallback)
{
    lua_getfield(L, table, key);
    const float value = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return value;
}

int WaveGrid_Lua::Create(lua_State* L)
{
    CHECK_TABLE(L, 1);
    WaveGridDesc desc;
    desc.mMinX = FieldOr(L, 1, "minX", desc.mMinX);
    desc.mMaxX = FieldOr(L, 1, "maxX", desc.mMaxX);
    desc.mMinZ = FieldOr(L, 1, "minZ", desc.mMinZ);
    desc.mMaxZ = FieldOr(L, 1, "maxZ", desc.mMaxZ);
    desc.mStep = FieldOr(L, 1, "step", desc.mStep);
    desc.mEdgeFade = FieldOr(L, 1, "edgeFade", desc.mEdgeFade);
    desc.mShadeGain = FieldOr(L, 1, "shadeGain", desc.mShadeGain);
    desc.mRingSpeed = FieldOr(L, 1, "ringSpeed", desc.mRingSpeed);
    desc.mRingWavelength = FieldOr(L, 1, "ringWavelength", desc.mRingWavelength);
    desc.mRingWidth = FieldOr(L, 1, "ringWidth", desc.mRingWidth);
    desc.mRingLife = FieldOr(L, 1, "ringLife", desc.mRingLife);
    desc.mRingHeight = FieldOr(L, 1, "ringHeight", desc.mRingHeight);
    desc.mRingCutoff = FieldOr(L, 1, "ringCutoff", desc.mRingCutoff);
    desc.mMaxRings = (uint32_t)FieldOr(L, 1, "maxRings", (float)desc.mMaxRings);

    WaveGrid_Lua* newObject = (WaveGrid_Lua*)lua_newuserdata(L, sizeof(WaveGrid_Lua));
    new (newObject) WaveGrid_Lua();
    newObject->mGrid = new WaveGrid(desc);
    luaL_getmetatable(L, WAVE_GRID_LUA_NAME);
    OCT_ASSERT(lua_istable(L, -1));
    lua_setmetatable(L, -2);
    return 1;
}

int WaveGrid_Lua::Destroy(lua_State* L)
{
    CHECK_WAVE_GRID(L, 1);
    WaveGrid_Lua* obj = (WaveGrid_Lua*)lua_touserdata(L, 1);
    delete obj->mGrid;
    obj->mGrid = nullptr;
    obj->~WaveGrid_Lua();
    return 0;
}

int WaveGrid_Lua::AddWave(lua_State* L)
{
    WaveGrid* grid = CHECK_WAVE_GRID(L, 1);
    float dirX = CHECK_NUMBER(L, 2);
    float dirZ = CHECK_NUMBER(L, 3);
    float wavelength = CHECK_NUMBER(L, 4);
    float height = CHECK_NUMBER(L, 5);
    float speed = lua_isnumber(L, 6) ? (float)lua_tonumber(L, 6) : 1.0f;
    float phase = lua_isnumber(L, 7) ? (float)lua_tonumber(L, 7) : 0.0f;
    grid->AddWave(dirX, dirZ, wavelength, height, speed, phase);
    return 0;
}

int WaveGrid_Lua::AddLayer(lua_State* L)
{
    WaveGrid* grid = CHECK_WAVE_GRID(L, 1);
    StaticMesh3D* nodeA = CHECK_STATIC_MESH_3D(L, 2);
    StaticMesh3D* nodeB = CHECK_STATIC_MESH_3D(L, 3);
    bool lit = lua_toboolean(L, 4) != 0;
    lua_pushboolean(L, grid->AddLayer(nodeA, nodeB, lit));
    return 1;
}

int WaveGrid_Lua::Ring(lua_State* L)
{
    WaveGrid* grid = CHECK_WAVE_GRID(L, 1);
    float x = CHECK_NUMBER(L, 2);
    float z = CHECK_NUMBER(L, 3);
    float strength = lua_isnumber(L, 4) ? (float)lua_tonumber(L, 4) : 1.0f;
    grid->Ring(x, z, strength);
    return 0;
}

int WaveGrid_Lua::HeightAt(lua_State* L)
{
    WaveGrid* grid = CHECK_WAVE_GRID(L, 1);
    float x = CHECK_NUMBER(L, 2);
    float z = CHECK_NUMBER(L, 3);
    lua_pushnumber(L, grid->HeightAt(x, z));
    return 1;
}

int WaveGrid_Lua::Update(lua_State* L)
{
    WaveGrid* grid = CHECK_WAVE_GRID(L, 1);
    float deltaTime = CHECK_NUMBER(L, 2);
    grid->Update(deltaTime);
    return 0;
}

int WaveGrid_Lua::GetNumColumns(lua_State* L)
{
    WaveGrid* grid = CHECK_WAVE_GRID(L, 1);
    lua_pushinteger(L, grid->GetNumColumns());
    return 1;
}

int WaveGrid_Lua::GetNumRows(lua_State* L)
{
    WaveGrid* grid = CHECK_WAVE_GRID(L, 1);
    lua_pushinteger(L, grid->GetNumRows());
    return 1;
}

void WaveGrid_Lua::Bind()
{
    lua_State* L = GetLua();
    OCT_ASSERT(lua_gettop(L) == 0);

    luaL_newmetatable(L, WAVE_GRID_LUA_NAME);
    int mtIndex = lua_gettop(L);

    REGISTER_TABLE_FUNC(L, mtIndex, Create);

    REGISTER_TABLE_FUNC_EX(L, mtIndex, Destroy, "__gc");

    REGISTER_TABLE_FUNC(L, mtIndex, AddWave);

    REGISTER_TABLE_FUNC(L, mtIndex, AddLayer);

    REGISTER_TABLE_FUNC(L, mtIndex, Ring);

    REGISTER_TABLE_FUNC(L, mtIndex, HeightAt);

    REGISTER_TABLE_FUNC(L, mtIndex, Update);

    REGISTER_TABLE_FUNC(L, mtIndex, GetNumColumns);

    REGISTER_TABLE_FUNC(L, mtIndex, GetNumRows);

    // Set the __index metamethod to itself
    lua_pushvalue(L, mtIndex);
    lua_setfield(L, mtIndex, "__index");

    lua_setglobal(L, WAVE_GRID_LUA_NAME);

    OCT_ASSERT(lua_gettop(L) == 0);
}

#endif
