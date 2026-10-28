#pragma once

#if API_GX

class Texture;
class StaticMesh;
class SkeletalMesh;
class StaticMesh3D;
class SkeletalMesh3D;
class Widget;

#include "Assets/MaterialLite.h"

#include <gccore.h>

// NEVER libogc's paired-single guMtxConcat (ps_guMtxConcat): it is BROKEN in the libogc this is
// built with (3.0.4). It loads the 0 of its constant { 0, 1 } (Unit01) with a psq_l addressed off
// r13 by an R_PPC_SDAREL16 relocation, and psq_l has only 12 bits of offset: the linker writes
// all 16, the top ones land in the instruction's W and I fields, and it loads ONE float (the 1
// comes free) from 608 bytes past r13 -- whatever variable the link happens to put there. Every
// matrix it makes then has that float times the first matrix's translation in its third column.
// In one game the address was inside StaticMesh::StageColorsFrom's read buffer: once a
// stage read colours into it, every 3D object was drawn with a garbage matrix --
// "stripes to a single point", the frame rate gone -- for as long as the game ran. It was zero
// until then, so all was well; another build's layout puts another variable there. The C concat
// is correct, and the engine calls it by name: c_guMtxConcat. The name guMtxConcat is pointed at
// it too, so a new call written the usual way cannot bring the broken one back. (No other libogc
// routine has such a load: the rest of gu_psasm reaches its constants with lfs, which has the 16
// bits. Check with: objdump -d -M gekko on the .elf and grep for a psq_l or psq_st off r13 or r2.)
#undef guMtxConcat
#define guMtxConcat c_guMtxConcat

void SetupLights();
void SetupLightMask(ShadingModel shadingModel,  uint8_t lightingChannels, bool useBakedLight);
void SetupLightingChannels();

void PrepareForwardRendering();
void PrepareUiRendering();

bool IsCpuSkinningRequired(SkeletalMesh3D* component);

// hasNormals: the draw's vertices carry normals. Without them a UV_MAP_ENVIRONMENT slot uses UV 0.
void BindMaterial(MaterialLite* material, bool useVertexColor, bool useBakedLighting, bool hasNormals = true);
// After BindMaterial, with the draw's normal matrix (as given to GX_LoadNrmMtxImm): loads the
// sphere-map matrix (GX_TEXMTX2) when the bound material has a UV_MAP_ENVIRONMENT slot.
void LoadEnvTexMtx(const Mtx normalMtx);
// Turns off indirect texturing (a TevMode::Warp material's) if any is on: GX_SetTevDirect on the
// stages that used it and GX_SetNumIndStages(0). BindMaterial, the UI pass and Octave's own TEV
// setups call it; code that sets up its own TEV stages during the 3D pass must call it too.
void GxResetIndirect();
void BindStaticMesh(StaticMesh* staticMesh, uint32_t* instanceColors);
void BindSkeletalMesh(SkeletalMesh* skeletalMesh);

uint8_t ConfigTev(uint8_t tevStage, uint32_t textureSlot, TevMode mode, bool vertexColorBlend);

void ApplyWidgetRotation(Mtx& mtx, Widget* widget);

void* CreateMeshDisplayList(StaticMesh* staticMesh, bool useColor, uint32_t& outSize, bool compact = false);
void GFX_SetCompactUnlitMeshes(bool compact);
bool GFX_GetCompactUnlitMeshes();
void GFX_SetQuantizedMeshes(bool quantized);
bool GFX_GetQuantizedMeshes();
// On top of GFX_SetQuantizedMeshes: meshes with vertex colours, and with a second UV set of their
// own, quantized too (GxUtils.cpp). Off unless a game turns it on.
void GFX_SetQuantizedMeshesExtended(bool extended);
bool GFX_GetQuantizedMeshesExtended();
// Which meshes may be quantized as they load (nullptr: any that fit): a game whose code reads or
// rewrites some meshes' float vertices (GetVertices) answers false for those -- quantized, they have none.
typedef bool (*GxQuantizeFilter)(class StaticMesh* mesh);
void GFX_SetQuantizeFilter(GxQuantizeFilter filter);
bool GFX_QuantizeAllowed(class StaticMesh* mesh);
// Anisotropic filtering, texture by texture (GxUtils.cpp): a game answers 2 or 4 for the textures seen at a slant (a sea,
// a floor), 1 for the rest. Set it before the textures load. (nullptr, as ever: none.)
typedef uint8_t (*GxAnisotropyFilter)(class Texture* texture);
void GFX_SetTextureAnisotropy(GxAnisotropyFilter filter);
uint8_t GFX_TextureAnisotropy(class Texture* texture);
// Trilinear filtering, texture by texture (GxUtils.cpp): a mipmapped linear texture blends its two nearest mips
// (GX_LIN_MIP_LIN), which takes the GPU's texture unit twice the cycles of one (GX_LIN_MIP_NEAR). A game answers false
// for the textures that don't need it. Set it before the textures load. (nullptr, as ever: all trilinear.)
typedef bool (*GxTrilinearFilter)(class Texture* texture);
void GFX_SetTextureTrilinear(GxTrilinearFilter filter);
bool GFX_TextureTrilinear(class Texture* texture);
// A quantized vertex: int16 x, y, z; int8 normal x, y, z; a pad byte; int16 s, t (GxUtils.cpp)
static const uint32_t kQuantStride = 14;
bool GFX_MaterialAllowsCompact(class Material* material);
void CallMeshDisplayList(void* displayList, uint32_t size);
void DestroyMeshDisplayList(void* displayList);

// BATCH CULLING -- written and tested (Dolphin, 2026-09-24) but NOT HOOKED UP: nothing calls it.
// A mesh's display lists are batches with bounding spheres; this draws only the batches in view.
// To use it: call GxSetCullFrustum in GFX_BeginFrame with the camera's projection (tangents
// 1 / P[0][0] and 1 / P[1][1], near and far), and CallMeshDisplayListCulled in place of
// CallMeshDisplayList in GFX_DrawStaticMeshComp with the model-view matrix (before it is turned
// into the normal matrix). One game left out only ~8% of its batches: its camera looks
// along its course, so nearly all of it is in view.
void CallMeshDisplayListCulled(void* displayList, uint32_t size, const Mtx modelView);
void GxSetCullFrustum(bool enabled, float tanHalfX, float tanHalfY, float nearZ, float farZ);
// For a mesh whose vertices move after its lists are built: its batches are never left out.
void GxMeshListsNoCull(void* displayList);

// The GPU may still be drawing the last frame while the CPU works on the next (Graphics_GX.cpp).
// GxWaitGpu: wait until it has drawn everything queued so far -- before rewriting in place any
// memory it reads. GxDeferFree: free a block it may still be reading once it is done with it.
void GxWaitGpu();
void GxDeferFree(void* block);
void GxCountDraw(uint32_t verts, uint32_t tris);

// BindMaterial skips loading GX_TEXMTX0/1 when they already hold its UV offset and scale: code that loads either
// itself calls this after (the engine clears it each frame too).
void GxInvalidateTexMtxCache();

#endif