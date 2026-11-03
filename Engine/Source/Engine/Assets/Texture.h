#pragma once

#include <string>
#include "glm/glm.hpp"
#include "Asset.h"

#include "Graphics/GraphicsTypes.h"

class Texture : public Asset
{

public:

    DECLARE_ASSET(Texture, Asset);

    Texture();
    virtual ~Texture();

    TextureResource* GetResource();

    // GX, not the editor: the cooked texels LoadStream read straight into GPU memory for this texture (32-byte
    // aligned), handed over to GFX_CreateTextureResource -- null if there are none. The caller owns them after.
    void* TakeCookedData(uint32_t& outSize);

    // Console only (GX): fill THIS texture's existing buffer with another texture asset's texels,
    // read straight off the disc in small pieces, when the two are the same size and format.
    // Nothing big is allocated or freed -- see the .cpp. False (and nothing changed) otherwise.
    bool ReloadFrom(const std::string& assetName);
    // The same, a piece at a time: up to maxBytes from byte `at`. Returns where the next piece
    // starts (outTotal, the texel bytes, when done), or -1.
    int32_t ReloadPart(const std::string& assetName, uint32_t at, uint32_t maxBytes, uint32_t& outTotal);
    // ReloadFrom, read by a background thread: returns at once (false if it cannot: another size or
    // format, or no room to queue it). The texture is half one picture and half the other until
    // the read is in, so refill one that is not on the screen. IsReloading is true until then; the
    // call that finds it done puts the new texels up (and after that it is false).
    bool ReloadFromAsync(const std::string& assetName);
    bool IsReloading();
    // GameCube only: another texture's texels read in the background into ARAM, this texture's own
    // stash (the texture itself untouched, so it can be on the screen meanwhile); then, when wanted,
    // copied in from there in a few milliseconds. For a picture needed at a moment's notice that
    // the SD card takes seconds to give: a level's next sky. StashFrom is false when it cannot
    // (another size or format, no ARAM, a stash already being read); IsStashed(name) is true once
    // that asset's texels are all in; ReloadFromStash puts them up (false if there is no stash).
    bool StashFrom(const std::string& assetName);
    bool IsStashed(const std::string& assetName) const;
    bool IsStashing() const;            // a stash being read now
    // Where a texture file's texels start and how many there are, from its first bytes, if they
    // fit this texture (ctx): for the background reader (see the .cpp).
    static bool LocateTexels(const char* head, uint32_t headSize, void* ctx, uint32_t& offset, uint32_t& size);
    bool ReloadFromStash();
    void FreeStash();                   // its ARAM given back (waits for a read under way)
    // GameCube only: a texture kept in ARAM while it isn't wanted, as Nintendo's games keep their menus' art (a 2D
    // archive in ARAM, copied into main memory for the screen that uses it). Evict lets its main-memory block go once
    // its own stash is in (StashFrom(its name), IsStashed): false until then. Restore brings it back from ARAM in a few
    // milliseconds (false: not evicted, or no memory). Evicted, a quad drawn with it draws nothing -- for UI quads only
    // (a mesh's material would sample nothing).
    bool Evict();
    bool Restore();
    bool IsEvicted() const;

    // Asset Interface
    virtual void LoadStream(Stream& stream, Platform platform) override;
    virtual bool CanLoadWindowed() const override { return true; }
    virtual void SaveStream(Stream& stream, Platform platform) override;
    virtual void Create() override;
    virtual void Destroy() override;
    virtual bool Import(const std::string& path, ImportOptions* options) override;
    virtual void GatherProperties(std::vector<Property>& outProps) override;
    virtual glm::vec4 GetTypeColor() override;
    virtual const char* GetTypeName() override;
    virtual const char* GetTypeImportExt() override;

    void Init(uint32_t width, uint32_t height, uint8_t* data);

    // Dynamic textures hold RGBA8 pixels that can be replaced every frame with
    // UpdatePixels() (e.g. video). Call InitDynamic() before Create().
    void InitDynamic(uint32_t width, uint32_t height);
    bool IsDynamic() const;
    void UpdatePixels(const uint8_t* rgba8);

    // A dynamic texture made of Y, Cb and Cr planes (GameCube/Wii), converted to RGB
    // by the GPU when drawn by a widget. Its data is set with GFX_SetTextureResourceData().
    void InitDynamicYuv(uint32_t width, uint32_t height);
    bool IsYuv() const;

    void SetMipmapped(bool mipmapped);
    bool IsMipmapped() const;
    bool IsRenderTarget() const;
    bool IsSrgb() const;
    bool IsForcedHighQuality() const;

    uint32_t GetWidth() const;
    uint32_t GetHeight() const;
    uint32_t GetMipLevels() const;
    uint32_t GetLayers() const;
    PixelFormat GetFormat() const;
    FilterType GetFilterType() const;
    WrapMode GetWrapMode() const;
    int32_t GetLowQualityDownsampleFactor() const;

    void SetFormat(PixelFormat format);
    void SetFilterType(FilterType filterType);
    void SetWrapMode(WrapMode wrapMode);
    void SetForceHighQuality(bool forceHq);

    static bool HandlePropChange(class Datum* datum, uint32_t index, const void* newValue);

protected:

    std::string mReloadSource;          // ReloadPart: the asset being read in, and where its texels start
    uint32_t mReloadOffset = 0;
    volatile int32_t mAsyncReload = 0;  // ReloadFromAsync: 0 none, 1 reading, 2 read, 3 failed
    void FinishAsyncReload();           // wait for one to be in (before the buffer is refilled or freed)
    uint32_t mStashAram = 0;            // StashFrom: its ARAM block (kept for the next), the asset in it,
    std::string mStashSource;           // and 0 none, 1 reading, 2 in, 3 failed
    volatile int32_t mStashState = 0;
    bool mEvicted = false;              // Evict: its texels only in its stash, not in main memory

    uint32_t mWidth;
    uint32_t mHeight;
    uint32_t mMipLevels;
    uint32_t mLayers;
    PixelFormat mFormat;
    FilterType mFilterType;
    WrapMode mWrapMode;
    bool mMipmapped;
    bool mRenderTarget;
    bool mSrgb;
    bool mForceHighQuality;
    uint8_t mLowQualityDownsampleFactor;

    // Sampled this many levels smaller (a mip bias, as GX_InitTexObjLOD's), and mip levels of its own (RGBA8, level 1
    // on, one after another) laid over the made ones: hand-drawn mips, as GameCube games' water has
    float mLodBias = 0.0f;
    std::vector<uint8_t> mCustomMips;

public:
    float GetLodBias() const { return mLodBias; }
    const std::vector<uint8_t>& GetCustomMips() const { return mCustomMips; }

protected:
    bool mDynamic = false;
    bool mYuv = false;

    // This pixel array is used as an intermediate storage between LoadStream() and Create()
    // It is cleared and shrunk within Create() except when compiled for EDITOR
    std::vector<uint8_t> mPixels;

    // GX, not the editor: the cooked texels in place of mPixels, read straight into the block the GPU will use (see
    // TakeCookedData). Read into mPixels and copied into an aligned block, a 2 MB texture needed 4 MB at once, two
    // 2 MB holes -- one more than a 24 MB console back from a big scene had.
    void* mCookedData = nullptr;
    uint32_t mCookedSize = 0;

    // Graphics Resource
    TextureResource mResource;
};
