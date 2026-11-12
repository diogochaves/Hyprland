#include "DMABuffer.hpp"
#include "WLBuffer.hpp"
#include "../../desktop/view/LayerSurface.hpp"
#include "../../render/Renderer.hpp"
#include "../../render/OpenGL.hpp"
#include "../../render/gl/GLTexture.hpp"
#include "../../helpers/Format.hpp"
#include "helpers/Drm.hpp"
#include <hyprgraphics/egl/Egl.hpp>

using namespace Hyprutils::OS;
using namespace Hyprgraphics::Egl;

static std::vector<CDMABuffer*>& allDMABuffers() {
    static std::vector<CDMABuffer*> buffers;
    return buffers;
}

CDMABuffer::SRegistration::SRegistration(CDMABuffer* buffer) : m_buffer(buffer) {
    allDMABuffers().emplace_back(buffer);
}

CDMABuffer::SRegistration::~SRegistration() {
    std::erase(allDMABuffers(), m_buffer);
}

void CDMABuffer::reimportAll() {
    size_t ok = 0, failed = 0, closed = 0;
    for (auto* b : allDMABuffers()) {
        if (!b->m_texture || !b->m_fds.at(0).isValid()) {
            closed++;
            continue;
        }

        if (b->reimport())
            ok++;
        else
            failed++;
    }

    LOG(Log::DEBUG, "GPU swap: dmabufs re-imported: {} ok, {} failed (they keep their last picture), {} already closed", ok, failed, closed);
}

bool CDMABuffer::reimport() {
    if (g_pHyprRenderer->type() != Render::IHyprRenderer::RT_GL)
        return false;

    const auto TEX = dynamic_cast<Render::GL::CGLTexture*>(m_texture.get());
    if (!TEX)
        return false;

    Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    auto image = Render::GL::g_pHyprOpenGL->createEGLImage(m_attrs);
    if (!image)
        return false;

    TEX->rebindImage(m_attrs, image);
    return true;
}

CDMABuffer::CDMABuffer(uint32_t id, wl_client* client, const Aquamarine::SDMABUFAttrs& attrs_, std::array<CFileDescriptor, 4> fds) : m_attrs(attrs_), m_fds(std::move(fds)) {
    for (size_t i = 0; i < m_fds.size(); ++i)
        m_attrs.fds[i] = m_fds[i].get();

    m_listeners.resourceDestroy = events.destroy.listen([this] {
        closeFDs();
        m_listeners.resourceDestroy.reset();
    });

    size       = m_attrs.size;
    m_resource = CWLBufferResource::create(makeShared<CWlBuffer>(client, 1, id));
    if UNLIKELY (!m_resource->good())
        return;

    m_opaque  = isDrmFormatOpaque(m_attrs.format);
    m_texture = g_pHyprRenderer->createTexture(m_attrs, m_opaque); // texture takes ownership of the eglImage

    if UNLIKELY (!m_texture) {
        LOG(Log::ERR, "CDMABuffer: failed to import EGLImage, retrying as implicit");
        m_attrs.modifier = DRM_FORMAT_MOD_INVALID;
        m_texture        = g_pHyprRenderer->createTexture(m_attrs, m_opaque);

        if UNLIKELY (!m_texture) {
            LOG(Log::ERR, "CDMABuffer: failed to import EGLImage");
            return;
        }
    }

    m_success = m_texture->ok();

    if UNLIKELY (!m_success)
        LOG(Log::ERR, "Failed to create a dmabuf: texture is null");
}

CDMABuffer::~CDMABuffer() {
    if (m_resource)
        m_resource->sendRelease();

    closeFDs();
}

Aquamarine::eBufferCapability CDMABuffer::caps() {
    return Aquamarine::eBufferCapability::BUFFER_CAPABILITY_DATAPTR;
}

Aquamarine::eBufferType CDMABuffer::type() {
    return Aquamarine::eBufferType::BUFFER_TYPE_DMABUF;
}

void CDMABuffer::update(const CRegion& damage) {
    // only does something for a texture that holds a copy (an external-only import)
    if (m_texture)
        m_texture->refresh();
}

bool CDMABuffer::isSynchronous() {
    return false;
}

Aquamarine::SDMABUFAttrs CDMABuffer::dmabuf() {
    return m_attrs;
}

std::tuple<uint8_t*, uint32_t, size_t> CDMABuffer::beginDataPtr(uint32_t flags) {
    // FIXME:
    return {nullptr, 0, 0};
}

void CDMABuffer::endDataPtr() {
    // FIXME:
}

bool CDMABuffer::good() {
    return m_success;
}

void CDMABuffer::closeFDs() {
    for (auto& fd : m_fds)
        fd.reset();

    m_attrs.fds.fill(-1);
    m_attrs.planes = 0;
}

std::vector<CFileDescriptor> CDMABuffer::exportSyncFiles() {
    if (!good())
        return {};

#ifndef __linux__
    return {};
#else
    std::vector<CFileDescriptor> syncFds;
    syncFds.reserve(m_attrs.fds.size());

    for (const auto& fd : m_attrs.fds) {
        if (fd == -1)
            continue;

        // buffer readability checks are rather slow on some Intel laptops
        // see https://gitlab.freedesktop.org/drm/intel/-/issues/9415
        if (g_pHyprRenderer && !g_pHyprRenderer->isIntel()) {
            if (CFileDescriptor::isReadable(fd))
                continue;
        }

        CFileDescriptor fence = DRM::exportFence(fd);
        if (fence.isValid())
            syncFds.emplace_back(std::move(fence));
    }

    return syncFds;
#endif
}
