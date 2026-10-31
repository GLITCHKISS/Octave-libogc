#pragma once

#include "Engine.h"
#include "WaveGrid.h"

#include "LuaBindings/LuaUtils.h"

#if LUA_ENABLED

#define WAVE_GRID_LUA_NAME "WaveGrid"
#define CHECK_WAVE_GRID(L, arg) CheckLuaType<WaveGrid_Lua>(L, arg, WAVE_GRID_LUA_NAME)->mGrid;

struct WaveGrid_Lua
{
    WaveGrid* mGrid = nullptr;

    static int Create(lua_State* L);
    static int Destroy(lua_State* L);

    static int AddWave(lua_State* L);
    static int AddLayer(lua_State* L);
    static int Ring(lua_State* L);
    static int HeightAt(lua_State* L);
    static int Update(lua_State* L);
    static int GetNumColumns(lua_State* L);
    static int GetNumRows(lua_State* L);

    static void Bind();
};

#endif
