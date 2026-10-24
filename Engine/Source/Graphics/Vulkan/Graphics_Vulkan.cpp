#if API_VULKAN

#include "Graphics/Graphics.h"
#include "Graphics/Vulkan/VulkanContext.h"
#include "Graphics/Vulkan/VulkanUtils.h"
#include "Graphics/Vulkan/Image.h"
#include "Graphics/Vulkan/DescriptorSet.h"
#include "Graphics/Vulkan/PipelineConfigs.h"
#include "Graphics/Vulkan/MultiBuffer.h"
#include "Renderer.h"
#include "World.h"
#include "Assets/StaticMesh.h"
#include "Assets/SkeletalMesh.h"
#include "Nodes/3D/StaticMesh3d.h"
#include "Nodes/3D/SkeletalMesh3d.h"
#include "Nodes/3D/Camera3d.h"

#include "Maths.h"
#include "Engine.h"

extern VulkanContext* gVulkanContext;

void GFX_Initialize()
{
    // On Android, it's possible that GFX_Initialize() was already called.
    if (gVulkanContext == nullptr)
    {
        CreateVulkanContext();
        gVulkanContext->Initialize();
    }
}

static Image* sProjShadowImageForShutdown();

void GFX_Shutdown()
{
    if (Image* img = sProjShadowImageForShutdown())
    {
        GetDestroyQueue()->Destroy(img);
    }
    gVulkanContext->Destroy();
    DestroyVulkanContext();
}

void GFX_BeginFrame()
{
    gVulkanContext->BeginFrame();
}

void GFX_EndFrame()
{
    gVulkanContext->EndFrame();
}

void GFX_BeginScreen(uint32_t screenIndex)
{

}

void GFX_BeginView(uint32_t viewIndex)
{

}

bool GFX_ShouldCullLights()
{
    return true;
}

void GFX_BeginRenderPass(RenderPassId renderPassId)
{
    gVulkanContext->BeginRenderPass(renderPassId);
}

void GFX_EndRenderPass()
{
    gVulkanContext->EndRenderPass();
}

void GFX_SetPipelineState(PipelineConfig pipelineConfig)
{
    BindPipelineConfig(pipelineConfig);
}

void GFX_SetViewport(int32_t x, int32_t y, int32_t width, int32_t height, bool handlePrerotation)
{
    gVulkanContext->SetViewport(x, y, width, height, handlePrerotation, false);
}

void GFX_SetScissor(int32_t x, int32_t y, int32_t width, int32_t height, bool handlePrerotation)
{
    gVulkanContext->SetScissor(x, y, width, height, handlePrerotation, false);
}

glm::mat4 GFX_MakePerspectiveMatrix(float fovyDegrees, float aspectRatio, float zNear, float zFar)
{
    VkSurfaceTransformFlagBitsKHR preTransformFlag = gVulkanContext->GetPreTransformFlag();

    glm::mat4 preRotateMat = glm::mat4(1.0f);
    glm::vec3 rotationAxis = glm::vec3(0.0f, 0.0f, 1.0f);

    if (preTransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
        preRotateMat = glm::rotate(preRotateMat, glm::radians(90.0f), rotationAxis);
    }

    else if (preTransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        preRotateMat = glm::rotate(preRotateMat, glm::radians(270.0f), rotationAxis);
    }

    else if (preTransformFlag & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
        preRotateMat = glm::rotate(preRotateMat, glm::radians(180.0f), rotationAxis);
    }

    glm::mat4 perspMat = glm::perspectiveFov(glm::radians(fovyDegrees), aspectRatio, 1.0f, zNear, zFar);
    perspMat[1][1] *= -1.0f;
    perspMat = preRotateMat * perspMat;

    return perspMat;
}

glm::mat4 GFX_MakeOrthographicMatrix(float left, float right, float bottom, float top, float zNear, float zFar)
{
    VkSurfaceTransformFlagBitsKHR preTransformFlag = gVulkanContext->GetPreTransformFlag();

    glm::mat4 preRotateMat = glm::mat4(1.0f);
    glm::vec3 rotationAxis = glm::vec3(0.0f, 0.0f, 1.0f);

    if (preTransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
        preRotateMat = glm::rotate(preRotateMat, glm::radians(90.0f), rotationAxis);
    }

    else if (preTransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        preRotateMat = glm::rotate(preRotateMat, glm::radians(270.0f), rotationAxis);
    }

    else if (preTransformFlag & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
        preRotateMat = glm::rotate(preRotateMat, glm::radians(180.0f), rotationAxis);
    }

    glm::mat4 orthoMat = glm::ortho(left, right, bottom, top, zNear, zFar);
    orthoMat[1][1] *= -1.0f;
    orthoMat = preRotateMat * orthoMat;

    return orthoMat;
}

void GFX_SetFog(const FogSettings& fogSettings)
{

}

void GFX_Draw(void* data)
{
    //gVulkanContext->Draw();
}

void GFX_DrawLines(const std::vector<Line>& lines)
{
    gVulkanContext->DrawLines(lines);
}

void GFX_DrawFullscreen()
{
    gVulkanContext->DrawFullscreen();
}

void GFX_ResizeWindow()
{
    // Just wait until the out-of-date error when presenting or acquiring swapchain image?
#if 0
    if (gVulkanContext != nullptr)
    {
        gVulkanContext->RecreateSwapchain(false);
    }
#endif
}

void GFX_Reset()
{
    if (gVulkanContext != nullptr)
    {
        gVulkanContext->RecreateSwapchain(true);
    }
}

Node3D* GFX_ProcessHitCheck(World* world, int32_t x, int32_t y, uint32_t* outInstance)
{
#if EDITOR
    return gVulkanContext->ProcessHitCheck(world, x, y, outInstance);
#else
    return nullptr;
#endif
}

uint32_t GFX_GetNumViews()
{
    return 1;
}

void GFX_SetFrameRate(int32_t frameRate)
{

}

void GFX_PathTrace()
{
    if (gVulkanContext->IsRayTracingSupported())
    {
        gVulkanContext->GetRayTracer()->PathTraceWorld();
    }
}

void GFX_BeginLightBake()
{
    if (gVulkanContext->IsRayTracingSupported())
    {
        gVulkanContext->GetRayTracer()->BeginLightBake();
    }
    else
    {
        LogError("GPU cannot bake light because maxPerStageDescriptorSampledImages < %d", PATH_TRACE_MAX_TEXTURES);
    }
}

void GFX_UpdateLightBake()
{
    if (gVulkanContext->IsRayTracingSupported())
    {
        gVulkanContext->GetRayTracer()->UpdateLightBake();
    }
}

void GFX_EndLightBake()
{
    if (gVulkanContext->IsRayTracingSupported())
    {
        gVulkanContext->GetRayTracer()->EndLightBake();
    }
}

bool GFX_IsLightBakeInProgress()
{
    bool ret = false;

    if (gVulkanContext->IsRayTracingSupported())
    {
        ret = gVulkanContext->GetRayTracer()->IsLightBakeInProgress();
    }

    return ret;
}

float GFX_GetLightBakeProgress()
{
    float ret = 0.0f;

    if (gVulkanContext->IsRayTracingSupported())
    {
        ret = gVulkanContext->GetRayTracer()->GetLightBakeProgress();
    }
    
    return ret;
}

void GFX_EnableMaterials(bool enable)
{
    // This function is used to signal whether draws should bind their material pipelines.
    // Otherwise, just use the current pipeline.
    // This is kinda hacky.
    gVulkanContext->EnableMaterials(enable);
}

void GFX_BeginGpuTimestamp(const char* name)
{
    gVulkanContext->BeginGpuTimestamp(name);
}

void GFX_EndGpuTimestamp(const char* name)
{
    gVulkanContext->EndGpuTimestamp(name);
}

void GFX_CreateTextureResource(Texture* texture, std::vector<uint8_t>& data)
{
    if (IsHeadless()) return;
    CreateTextureResource(texture, data.data());
}

void GFX_DestroyTextureResource(Texture* texture)
{
    if (IsHeadless()) return;
    DestroyTextureResource(texture);
}

void GFX_UpdateTextureResourcePixels(Texture* texture, const uint8_t* rgba8)
{
    if (IsHeadless()) return;

    TextureResource* resource = texture->GetResource();
    if (resource->mImage != nullptr && rgba8 != nullptr)
    {
        resource->mImage->Update(rgba8);
    }
}

void GFX_SetTextureResourceData(Texture* texture, uint8_t* const* planes)
{
    // GameCube/Wii only; Vulkan textures are updated with GFX_UpdateTextureResourcePixels().
}

void GFX_CreateMaterialResource(Material* material)
{
    if (IsHeadless()) return;
    CreateMaterialResource(material);
}

void GFX_DestroyMaterialResource(Material* material)
{
    if (IsHeadless()) return;
    DestroyMaterialResource(material);
}

void GFX_CreateStaticMeshResource(StaticMesh* staticMesh, bool hasColor, uint32_t numVertices, void* vertices, uint32_t numIndices, IndexType* indices)
{
    if (IsHeadless()) return;
    CreateStaticMeshResource(staticMesh, hasColor, numVertices, vertices, numIndices, indices);
}

void GFX_DestroyStaticMeshResource(StaticMesh* staticMesh)
{
    if (IsHeadless()) return;
    DestroyStaticMeshResource(staticMesh);
}

void GFX_CreateSkeletalMeshResource(SkeletalMesh* skeletalMesh, uint32_t numVertices, VertexSkinned* vertices, uint32_t numIndices, uint32_t* indices)
{
    if (IsHeadless()) return;
    CreateSkeletalMeshResource(skeletalMesh, numVertices, vertices, numIndices, indices);
}

void GFX_DestroySkeletalMeshResource(SkeletalMesh* skeletalMesh)
{
    if (IsHeadless()) return;
    DestroySkeletalMeshResource(skeletalMesh);
}

void GFX_CreateStaticMeshCompResource(StaticMesh3D* staticMeshComp)
{
    if (IsHeadless()) return;
}

void GFX_DestroyStaticMeshCompResource(StaticMesh3D* staticMeshComp)
{
    if (IsHeadless()) return;
    DestroyStaticMeshCompResource(staticMeshComp);
}

void GFX_UpdateStaticMeshCompResourceColors(StaticMesh3D* staticMeshComp)
{
    if (IsHeadless()) return;
    UpdateStaticMeshCompResourceColors(staticMeshComp);
}

void GFX_DrawStaticMeshComp(StaticMesh3D* staticMeshComp, StaticMesh* meshOverride)
{
    DrawStaticMeshComp(staticMeshComp, meshOverride);
}

void GFX_CreateSkeletalMeshCompResource(SkeletalMesh3D* skeletalMeshComp)
{
    if (IsHeadless()) return;
}

void GFX_DestroySkeletalMeshCompResource(SkeletalMesh3D* skeletalMeshComp)
{
    if (IsHeadless()) return;
    DestroySkeletalMeshCompResource(skeletalMeshComp);
}

void GFX_ReallocateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* skeletalMeshComp, uint32_t numVertices)
{
    if (IsHeadless()) return;
    ReallocateSkeletalMeshCompVertexBuffer(skeletalMeshComp, numVertices);
}

void GFX_UpdateSkeletalMeshCompVertexBuffer(SkeletalMesh3D* skeletalMeshComp, const std::vector<Vertex>& skinnedVertices)
{
    if (IsHeadless()) return;
    UpdateSkeletalMeshCompVertexBuffer(skeletalMeshComp, skinnedVertices);
}

void GFX_DrawSkeletalMeshComp(SkeletalMesh3D* skeletalMeshComp)
{
    DrawSkeletalMeshComp(skeletalMeshComp);
}

bool GFX_IsCpuSkinningRequired(SkeletalMesh3D* skeletalMeshComp)
{
    return IsCpuSkinningRequired(skeletalMeshComp);
}

void GFX_DrawShadowMeshComp(ShadowMesh3D* shadowMeshComp)
{
    DrawShadowMeshComp(shadowMeshComp);
}

// Projected shadows, as the GX renderer's (its GFX_RenderProjectedShadowMap / GFX_DrawProjectedShadowReceivers are the
// reference): the casters' silhouette, one flat grey over white, from an orthographic camera straight above the first
// of them -- half the size of its bounds each way -- into a small texture before the forward pass; then each receiver
// under that box drawn again inside the forward pass with the texture laid straight down onto it (texture coordinates
// from its world position through the same camera, global.mShadowViewProj), multiplying what is there.
static const uint32_t kProjShadowRes = 128;
static Image* sProjShadowImage = nullptr;
static bool sProjShadowValid = false;
static float sProjShadowHalf = 1.0f;
static glm::vec3 sProjShadowCenter = { 0.0f, 0.0f, 0.0f };
static float sProjShadowTop = 0.0f;

static Image* sProjShadowImageForShutdown()
{
    Image* img = sProjShadowImage;
    sProjShadowImage = nullptr;
    sProjShadowValid = false;
    return img;
}

void GFX_RenderProjectedShadowMap(const std::vector<SkeletalMesh3D*>& casters)
{
    sProjShadowValid = false;
    World* world = Renderer::Get()->GetCurrentWorld();
    if (casters.empty() || world == nullptr || world->GetActiveCamera() == nullptr)
        return;

    if (sProjShadowImage == nullptr)
    {
        ImageDesc desc;
        desc.mWidth = kProjShadowRes;
        desc.mHeight = kProjShadowRes;
        desc.mFormat = VK_FORMAT_R8G8B8A8_UNORM;
        desc.mUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        SamplerDesc sampler;
        sampler.mAddressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler.mBorderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;      // (outside the box: no shadow)
        sProjShadowImage = new Image(desc, sampler, "Projected Shadow");
        sProjShadowImage->Transition(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // The camera: over the first caster (the rest ride with it), straight down.
    SkeletalMesh3D* lead = casters[0];
    Bounds b = lead->GetBounds();
    float radius = (lead->GetBoundsRadiusOverride() > 0.0f) ? lead->GetBoundsRadiusOverride() : b.mRadius;
    sProjShadowHalf = glm::max(0.25f, radius * 0.5f);
    sProjShadowCenter = lead->GetWorldPosition();
    sProjShadowTop = sProjShadowCenter.y + radius;

    glm::mat4 view = glm::lookAt(sProjShadowCenter + glm::vec3(0.0f, 20.0f, 0.0f), sProjShadowCenter, glm::vec3(0.0f, 0.0f, -1.0f));
    glm::mat4 proj = glm::ortho(-sProjShadowHalf, sProjShadowHalf, -sProjShadowHalf, sProjShadowHalf, 0.1f, 200.0f);
    proj[1][1] *= -1.0f;
    gVulkanContext->GetGlobalUniformData().mShadowViewProj = proj * view;
    gVulkanContext->UpdateGlobalDescriptorSet();

    RenderPassSetup rp;
    rp.mColorImages[0] = sProjShadowImage;
    rp.mLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    rp.mStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    rp.mDebugName = "Projected Shadow";
    gVulkanContext->BeginVkRenderPass(rp, true, glm::vec4(1.0f));    // (white: no shadow)

    // its border kept white (the texture clamps: nothing smears off its edge)
    gVulkanContext->SetViewport(0, 0, kProjShadowRes, kProjShadowRes, false, false);
    gVulkanContext->SetScissor(2, 2, kProjShadowRes - 4, kProjShadowRes - 4, false, false);

    VkCommandBuffer cb = GetCommandBuffer();
    for (SkeletalMesh3D* caster : casters)
    {
        SkeletalMesh* mesh = caster->GetSkeletalMesh();
        if (mesh == nullptr)
            continue;

        BindPipelineConfig(PipelineConfig::ProjShadowCaster);
        if (IsCpuSkinningRequired(caster))
        {
            VkDeviceSize offset = 0;
            VkBuffer vertexBuffer = caster->GetResource()->mVertexBuffer->Get();
            vkCmdBindVertexBuffers(cb, 0, 1, &vertexBuffer, &offset);
            BindSkeletalMeshResourceIndices(mesh);
            gVulkanContext->SetVertexType(VertexType::Vertex);
            gVulkanContext->SetVertexShader("Shadow.vert");
        }
        else
        {
            BindSkeletalMeshResource(mesh);
            gVulkanContext->SetVertexType(VertexType::VertexSkinned);
            gVulkanContext->SetVertexShader("ShadowSkinned.vert");
        }
        gVulkanContext->CommitPipeline();
        BindGeometryDescriptorSet(caster);
        vkCmdDrawIndexed(cb, mesh->GetNumIndices(), 1, 0, 0, 0);
    }

    gVulkanContext->EndVkRenderPass();
    sProjShadowValid = true;
}

void GFX_DrawProjectedShadowReceivers(const std::vector<StaticMesh3D*>& receivers)
{
    if (!sProjShadowValid || sProjShadowImage == nullptr)
        return;

    VkCommandBuffer cb = GetCommandBuffer();
    const float reach = sProjShadowHalf * 1.42f;                     // (the box's corner)
    for (StaticMesh3D* comp : receivers)
    {
        StaticMesh* mesh = comp->GetStaticMesh();
        if (mesh == nullptr)
            continue;

        // only what lies under the box, and not above the caster's top
        Bounds b = comp->GetBounds();
        float dx = b.mCenter.x - sProjShadowCenter.x, dz = b.mCenter.z - sProjShadowCenter.z;
        float r = b.mRadius + reach;
        if (dx * dx + dz * dz > r * r || b.mCenter.y - b.mRadius > sProjShadowTop)
            continue;

        BindPipelineConfig(PipelineConfig::ProjShadowReceiver);
        BindStaticMeshResource(mesh);
        bool color = mesh->HasVertexColor();
        gVulkanContext->SetVertexType(color ? VertexType::VertexColor : VertexType::Vertex);
        gVulkanContext->SetVertexShader(color ? "ForwardColor.vert" : "Forward.vert");
        gVulkanContext->CommitPipeline();
        BindGeometryDescriptorSet(comp);
        DescriptorSet::Begin("Projected Shadow").WriteImage(0, sProjShadowImage).Build().Bind(cb, 2);
        vkCmdDrawIndexed(cb, mesh->GetNumIndices(), 1, 0, 0, 0);
    }

    BindPipelineConfig(PipelineConfig::Forward);
}

void GFX_DrawInstancedMeshComp(InstancedMesh3D* instancedMeshComp)
{
    DrawInstancedMeshComp(instancedMeshComp);
}

void GFX_CreateTextMeshCompResource(TextMesh3D* textMeshComp)
{
    if (IsHeadless()) return;
}

void GFX_DestroyTextMeshCompResource(TextMesh3D* textMeshComp)
{
    if (IsHeadless()) return;
    DestroyTextMeshCompResource(textMeshComp);
}

void GFX_UpdateTextMeshCompVertexBuffer(TextMesh3D* textMeshComp, const std::vector<Vertex>& vertices)
{
    if (IsHeadless()) return;
    UpdateTextMeshCompVertexBuffer(textMeshComp, vertices);
}

void GFX_DrawTextMeshComp(TextMesh3D* textMeshComp)
{
    DrawTextMeshComp(textMeshComp);
}

void GFX_CreateParticleCompResource(Particle3D* particleComp)
{
    if (IsHeadless()) return;
}

void GFX_DestroyParticleCompResource(Particle3D* particleComp)
{
    if (IsHeadless()) return;
    DestroyParticleCompResource(particleComp);
}

void GFX_UpdateParticleCompVertexBuffer(Particle3D* particleComp, const std::vector<VertexParticle>& vertices)
{
    if (IsHeadless()) return;
    UpdateParticleCompVertexBuffer(particleComp, vertices);
}

void GFX_DrawParticleComp(Particle3D* particleComp)
{
    DrawParticleComp(particleComp);
}

void GFX_CreateQuadResource(Quad* quad)
{
    if (IsHeadless()) return;
    CreateQuadResource(quad);
}

void GFX_DestroyQuadResource(Quad* quad)
{
    if (IsHeadless()) return;
    DestroyQuadResource(quad);
}

void GFX_UpdateQuadResourceVertexData(Quad* quad)
{
    if (IsHeadless()) return;
    UpdateQuadResourceVertexData(quad);
}

void GFX_DrawQuad(Quad* quad)
{
    DrawQuad(quad);
}

void GFX_CreateTextResource(Text* text)
{
    if (IsHeadless()) return;
    CreateTextResource(text);
}

void GFX_DestroyTextResource(Text* text)
{
    if (IsHeadless()) return;
    DestroyTextResource(text);
}

void GFX_UpdateTextResourceVertexData(Text* text)
{
    if (IsHeadless()) return;
    UpdateTextResourceVertexData(text);
}

void GFX_DrawText(Text* text)
{
    DrawTextWidget(text);
}

void GFX_CreatePolyResource(Poly* poly)
{
    if (IsHeadless()) return;
    CreatePolyResource(poly);
}

void GFX_DestroyPolyResource(Poly* poly)
{
    if (IsHeadless()) return;
    DestroyPolyResource(poly);
}

void GFX_UpdatePolyResourceVertexData(Poly* poly)
{
    if (IsHeadless()) return;
    UpdatePolyResourceVertexData(poly);
}

void GFX_DrawPoly(Poly* poly)
{
    DrawPoly(poly);
}

void GFX_DrawStaticMesh(StaticMesh* mesh, Material* material, const glm::mat4& transform, glm::vec4 color)
{
    DrawStaticMesh(mesh, material, transform, color);
}

void GFX_RenderPostProcessPasses()
{
    gVulkanContext->RenderPostProcessChain();
}

#endif