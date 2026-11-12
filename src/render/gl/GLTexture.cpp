#include "GLTexture.hpp"
#include "../Renderer.hpp"
#include "../OpenGL.hpp"
#include "../../Compositor.hpp"
#include "../../helpers/Format.hpp"
#include "../Texture.hpp"
#include <cstring>
#include <hyprgraphics/egl/Egl.hpp>

using namespace Hyprgraphics::Egl;
using namespace Render::GL;

static std::vector<CGLTexture*>& allTextures() {
    static std::vector<CGLTexture*> textures;
    return textures;
}

CGLTexture::SRegistration::SRegistration(CGLTexture* tex) : m_tex(tex) {
    allTextures().emplace_back(tex);
}

CGLTexture::SRegistration::~SRegistration() {
    std::erase(allTextures(), m_tex);
}

void CGLTexture::contextLostAll() {
    CFramebufferBindingGuard bindings{g_pHyprOpenGL};

    size_t                   kept = 0, bytes = 0;
    for (auto* tex : allTextures()) {
        tex->contextLost();
        if (!tex->m_lost.pixels.empty()) {
            kept++;
            bytes += tex->m_lost.pixels.size();
        }
    }

    LOG(Log::DEBUG, "GPU swap: {} textures lost, {} kept on the CPU ({} MiB)", allTextures().size(), kept, bytes / (1024 * 1024));
}

void CGLTexture::contextRestoredAll() {
    for (auto* tex : allTextures()) {
        tex->contextRestored();
    }
}

void CGLTexture::contextLost() {
    m_lost.active = m_texID != 0;
    m_lost.states = m_cachedStates;
    m_lost.pixels.clear();

    if (m_texID && m_target == GL_TEXTURE_2D && m_size.x >= 1 && m_size.y >= 1) {
        // read the texels back through a throwaway framebuffer
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        g_pHyprOpenGL->bindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texID, 0);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            m_lost.pixels.resize(sc<size_t>(m_size.x) * sc<size_t>(m_size.y) * 4);
            while (glGetError() != GL_NO_ERROR)
                ;
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, m_size.x, m_size.y, GL_RGBA, GL_UNSIGNED_BYTE, m_lost.pixels.data());
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            if (glGetError() != GL_NO_ERROR)
                m_lost.pixels.clear(); // not a format we can read as RGBA8 (float, depth): comes back empty
        }

        glDeleteFramebuffers(1, &fbo);
        g_pHyprOpenGL->onFramebufferDeleted(fbo);
    }

    if (m_texID)
        GLCALL(glDeleteTextures(1, &m_texID));
    m_texID = 0;

    if (m_eglImage)
        g_pHyprOpenGL->m_proc.eglDestroyImageKHR(g_pHyprOpenGL->m_eglDisplay, m_eglImage);
    m_eglImage = nullptr;

    m_cachedStates.fill(std::nullopt);
}

void CGLTexture::contextRestored() {
    if (!m_lost.active)
        return;

    m_lost.active = false;

    GLCALL(glGenTextures(1, &m_texID));
    bind();

    static constexpr std::array<GLenum, TEXTURE_PAR_LAST> PNAMES = {
        GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_MAG_FILTER, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G, GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A,
    };
    for (size_t i = 0; i < TEXTURE_PAR_LAST; ++i) {
        if (m_lost.states[i])
            setTexParameter(PNAMES[i], *m_lost.states[i]);
    }

    if (m_target == GL_TEXTURE_3D && !m_lutCopy.empty()) {
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
        GLCALL(glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, m_lutN, m_lutN, m_lutN, 0, GL_RGBA, GL_FLOAT, m_lutCopy.data()));
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
    } else if (!m_lost.pixels.empty()) {
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
        GLCALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_size.x, m_size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_lost.pixels.data()));
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
    }

    unbind();

    m_lost.pixels.clear();
    m_lost.pixels.shrink_to_fit();
}

void CGLTexture::rebindImage(void* image) {
    if (!image || !m_texID || m_target != GL_TEXTURE_2D)
        return;

    bind();
    GLCALL(g_pHyprOpenGL->m_proc.glEGLImageTargetTexture2DOES(m_target, image));
    unbind();

    m_eglImage = image;
}

CGLTexture::CGLTexture(bool opaque) {
    m_opaque = opaque;
}

CGLTexture::~CGLTexture() {
    if (!g_pCompositor || g_pCompositor->m_isShuttingDown || !g_pHyprRenderer || !g_pHyprOpenGL)
        return;

    g_pHyprOpenGL->makeEGLCurrent();
    if (m_texID) {
        GLCALL(glDeleteTextures(1, &m_texID));
        m_texID = 0;
    }

    if (m_eglImage)
        g_pHyprOpenGL->m_proc.eglDestroyImageKHR(g_pHyprOpenGL->m_eglDisplay, m_eglImage);
    m_eglImage = nullptr;
    m_cachedStates.fill(std::nullopt);
}

CGLTexture::CGLTexture(uint32_t drmFormat, uint8_t* pixels, uint32_t stride, const Vector2D& size_, bool keepDataCopy, bool opaque) :
    ITexture(drmFormat, pixels, stride, size_, keepDataCopy, opaque) {

    g_pHyprOpenGL->makeEGLCurrent();

    const auto format = getPixelFormatFromDRM(drmFormat);
    ASSERT(format);

    m_type          = format->withAlpha ? TEXTURE_RGBA : TEXTURE_RGBX;
    m_size          = size_;
    m_isSynchronous = true;
    m_target        = GL_TEXTURE_2D;
    allocate(size_);
    bind();
    setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if (format->swizzle.has_value())
        swizzle(format->swizzle.value());

    bool alignmentChanged = false;
    if (format->bytesPerBlock != 4) {
        const GLint alignment = (stride % 4 == 0) ? 4 : 1;
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, alignment));
        alignmentChanged = true;
    }

    GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / format->bytesPerBlock));
    GLCALL(glTexImage2D(GL_TEXTURE_2D, 0, format->glInternalFormat ? format->glInternalFormat : format->glFormat, size_.x, size_.y, 0, format->glFormat, format->glType, pixels));
    GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0));
    if (alignmentChanged)
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 4));

    unbind();
}

CGLTexture::CGLTexture(const Aquamarine::SDMABUFAttrs& attrs, void* image, bool opaque) {
    m_opaque = opaque;
    if (!g_pHyprOpenGL->m_proc.glEGLImageTargetTexture2DOES) {
        LOG(Log::ERR, "Cannot create a dmabuf texture: no glEGLImageTargetTexture2DOES");
        return;
    }

    m_opaque = isDrmFormatOpaque(attrs.format);

    // #TODO external only formats should be external as well.
    // also needs a separate color shader.
    /*if (NFormatUtils::isFormatYUV(attrs.format)) {
        m_target = GL_TEXTURE_EXTERNAL_OES;
        m_type   = TEXTURE_EXTERNAL;
    } else {*/
    m_target = GL_TEXTURE_2D;
    m_type   = isDrmFormatOpaque(attrs.format) ? TEXTURE_RGBX : TEXTURE_RGBA;
    //}

    allocate(attrs.size, attrs.format);
    m_eglImage = image;

    bind();
    setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLCALL(g_pHyprOpenGL->m_proc.glEGLImageTargetTexture2DOES(m_target, image));
    unbind();
}

CGLTexture::CGLTexture(std::span<const float> lut3D, size_t N) : ITexture(lut3D, N), m_target(GL_TEXTURE_3D) {
    allocate({});
    bind();

    GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
    setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    setTexParameter(GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    // Expand RGB->RGBA on upload (alpha=1)
    std::vector<float> rgba;
    rgba.resize(N * N * N * 4);
    for (size_t i = 0, j = 0; i < N * N * N; ++i, j += 3) {
        rgba[i * 4 + 0] = lut3D[j + 0];
        rgba[i * 4 + 1] = lut3D[j + 1];
        rgba[i * 4 + 2] = lut3D[j + 2];
        rgba[i * 4 + 3] = 1.F;
    }

    GLCALL(glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, N, N, N, 0, GL_RGBA, GL_FLOAT, rgba.data()));

    m_lutN    = N;
    m_lutCopy = std::move(rgba);

    unbind();
}

void CGLTexture::update(uint32_t drmFormat, uint8_t* pixels, uint32_t stride, const CRegion& damage) {
    if (damage.empty())
        return;

    g_pHyprOpenGL->makeEGLCurrent();

    const auto format = getPixelFormatFromDRM(drmFormat);
    ASSERT(format);

    bind();

    if (format->swizzle.has_value())
        swizzle(format->swizzle.value());

    bool alignmentChanged = false;
    if (format->bytesPerBlock != 4) {
        const GLint alignment = (stride % 4 == 0) ? 4 : 1;
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, alignment));
        alignmentChanged = true;
    }

    GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / format->bytesPerBlock));

    damage.copy().intersect(CBox{{}, m_size}).forEachRect([&format, &pixels](const auto& rect) {
        GLCALL(glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, rect.x1));
        GLCALL(glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, rect.y1));

        int width  = rect.x2 - rect.x1;
        int height = rect.y2 - rect.y1;
        GLCALL(glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x1, rect.y1, width, height, format->glFormat, format->glType, pixels));
    });

    if (alignmentChanged)
        GLCALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 4));

    GLCALL(glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0));
    GLCALL(glPixelStorei(GL_UNPACK_SKIP_PIXELS_EXT, 0));
    GLCALL(glPixelStorei(GL_UNPACK_SKIP_ROWS_EXT, 0));

    unbind();

    if (m_keepDataCopy) {
        m_dataCopy.resize(stride * m_size.y);
        memcpy(m_dataCopy.data(), pixels, stride * m_size.y);
    }
}

void CGLTexture::allocate(const Vector2D& size, uint32_t drmFormat) {
    if (!m_texID)
        GLCALL(glGenTextures(1, &m_texID));
    m_size      = size;
    m_drmFormat = drmFormat;
}

void CGLTexture::bind() {
    GLCALL(glBindTexture(m_target, m_texID));
}

void CGLTexture::unbind() {
    GLCALL(glBindTexture(m_target, 0));
}

bool CGLTexture::ok() {
    return m_texID > 0;
}

bool CGLTexture::isDMA() {
    return m_eglImage;
}

constexpr std::optional<size_t> CGLTexture::getCacheStateIndex(GLenum pname) {
    switch (pname) {
        case GL_TEXTURE_WRAP_S: return TEXTURE_PAR_WRAP_S;
        case GL_TEXTURE_WRAP_T: return TEXTURE_PAR_WRAP_T;
        case GL_TEXTURE_MAG_FILTER: return TEXTURE_PAR_MAG_FILTER;
        case GL_TEXTURE_MIN_FILTER: return TEXTURE_PAR_MIN_FILTER;
        case GL_TEXTURE_SWIZZLE_R: return TEXTURE_PAR_SWIZZLE_R;
        case GL_TEXTURE_SWIZZLE_G: return TEXTURE_PAR_SWIZZLE_G;
        case GL_TEXTURE_SWIZZLE_B: return TEXTURE_PAR_SWIZZLE_B;
        case GL_TEXTURE_SWIZZLE_A: return TEXTURE_PAR_SWIZZLE_A;
        default: return std::nullopt;
    }
}

void CGLTexture::setTexParameter(GLenum pname, GLint param) {
    const auto cacheIndex = getCacheStateIndex(pname);

    if (!cacheIndex) {
        GLCALL(glTexParameteri(m_target, pname, param));
        return;
    }

    const auto idx = cacheIndex.value();

    if (m_cachedStates[idx] == param)
        return;

    m_cachedStates[idx] = param;
    GLCALL(glTexParameteri(m_target, pname, param));
}

void CGLTexture::swizzle(const std::array<GLint, 4>& colors) {
    setTexParameter(GL_TEXTURE_SWIZZLE_R, colors.at(0));
    setTexParameter(GL_TEXTURE_SWIZZLE_G, colors.at(1));
    setTexParameter(GL_TEXTURE_SWIZZLE_B, colors.at(2));
    setTexParameter(GL_TEXTURE_SWIZZLE_A, colors.at(3));
}
