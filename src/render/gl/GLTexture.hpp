#pragma once

#include "../Texture.hpp"
#include <aquamarine/buffer/Buffer.hpp>
#include <hyprutils/math/Misc.hpp>

namespace Render::GL {

    class CGLTexture : public ITexture {
      public:
        using ITexture::ITexture;

        CGLTexture(CGLTexture&)        = delete;
        CGLTexture(CGLTexture&&)       = delete;
        CGLTexture(const CGLTexture&&) = delete;
        CGLTexture(const CGLTexture&)  = delete;

        CGLTexture(bool opaque = false);
        CGLTexture(uint32_t drmFormat, uint8_t* pixels, uint32_t stride, const Vector2D& size, bool keepDataCopy = false, bool opaque = false);
        CGLTexture(const Aquamarine::SDMABUFAttrs&, void* image, bool opaque = false);
        CGLTexture(std::span<const float> lut3D, size_t N);
        ~CGLTexture();

        void allocate(const Vector2D& size, uint32_t drmFormat = 0) override;
        void update(uint32_t drmFormat, uint8_t* pixels, uint32_t stride, const CRegion& damage) override;
        void bind() override;
        void unbind() override;
        void setTexParameter(GLenum pname, GLint param) override;
        bool ok() override;
        bool isDMA() override;
        void refresh() override;

        // GPU swap: the EGL context is about to go (it is current) / a new one is current.
        // A lost texture keeps its pixels on the CPU and gets them back, under a new GL name.
        static void contextLostAll();
        static void contextRestoredAll();
        // a dmabuf texture gets its buffer back as a new EGLImage, of which it takes ownership
        void rebindImage(const Aquamarine::SDMABUFAttrs& attrs, void* image);

      private:
        void* m_eglImage = nullptr;

        // A dmabuf EGL only imports as GL_TEXTURE_EXTERNAL_OES (NVIDIA, a LINEAR buffer of another GPU) is bound
        // to this name, and m_texID holds a plain 2D copy of it, taken at every refresh().
        GLuint m_externalTexID = 0;
        void   attachImage(const Aquamarine::SDMABUFAttrs& attrs, void* image);

        enum eTextureParam : uint8_t {
            TEXTURE_PAR_WRAP_S = 0,
            TEXTURE_PAR_WRAP_T,
            TEXTURE_PAR_MAG_FILTER,
            TEXTURE_PAR_MIN_FILTER,
            TEXTURE_PAR_SWIZZLE_R,
            TEXTURE_PAR_SWIZZLE_G,
            TEXTURE_PAR_SWIZZLE_B,
            TEXTURE_PAR_SWIZZLE_A,
            TEXTURE_PAR_LAST,
        };

        GLenum                                             m_target = GL_TEXTURE_2D;

        void                                               swizzle(const std::array<GLint, 4>& colors);
        constexpr std::optional<size_t>                    getCacheStateIndex(GLenum pname);

        std::array<std::optional<GLint>, TEXTURE_PAR_LAST> m_cachedStates;

        void                                               contextLost();
        void                                               contextRestored();

        struct SLost {
            bool                                               active = false;
            std::vector<uint8_t>                               pixels; // RGBA8, the raw texels
            std::array<std::optional<GLint>, TEXTURE_PAR_LAST> states;
        } m_lost;

        std::vector<float> m_lutCopy; // RGBA, only for 3D LUTs
        size_t             m_lutN = 0;

        struct SRegistration {
            SRegistration(CGLTexture* tex);
            ~SRegistration();
            CGLTexture* m_tex = nullptr;
        } m_registration{this};
    };
}
