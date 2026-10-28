#include "render/globe_renderer.hpp"

#if defined(__ANDROID__)
#include <android/log.h>
#endif
#if defined(SOKOL_GLES3)
#include <GLES3/gl3.h>
#define OTK_GL_MIPS 1
#elif defined(SOKOL_GLCORE) && !defined(_WIN32) && !defined(__APPLE__)
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#define OTK_GL_MIPS 1
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(SOKOL_D3D11)
#include <d3d11.h>
#include <dxgi.h>
#endif
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#else
#include <unistd.h>
#endif

#include "feature/grid.hpp"
#include "net/http.hpp"

#include <stb_image.h>

#include "globe.glsl.h"
#include "mip.glsl.h"
#include "vector.glsl.h"
#include "sky.glsl.h"
#include "surface.glsl.h"
#include "snapshot.glsl.h"

#include "core/astro.hpp"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>
#if !defined(_WIN32)
#include <time.h>
#endif

#if defined(OTK_EMBED_BACKDROP)
namespace otk::embedded {
extern const unsigned char world_backdrop[];
extern const std::size_t world_backdrop_size;
}
#endif

namespace otk {

namespace {

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
constexpr double budgetMs = 6.0;
constexpr double prefetchBudgetMs = 2.0;
#else
constexpr double budgetMs = 8.0;
constexpr double prefetchBudgetMs = 3.0;
#endif
constexpr int kGrid = 32;
constexpr int kMaxUniformDrop = 3;
constexpr int kPosts = kGrid + 1;
constexpr int kVertexCount = kPosts * kPosts + 4 * kPosts;
constexpr int kStride = 7;
constexpr int kIndexCount = kGrid * kGrid * 6 + 4 * kGrid * 6;
constexpr int kSurfaceSize = 256;
constexpr int kMinLevel = 2;
constexpr int kMaxLevel = 22;
constexpr int kKeepLevel = 3;
constexpr int kSurfaceRendersPerFrame = 32;
constexpr int kTerrainBuildsPerFrame = 32;
constexpr size_t kUploadsPerFrame = 48;
constexpr size_t kSurfaceBufferBytes = 4 * 1024 * 1024;
constexpr double kRetrySeconds = 30.0;
constexpr int kBaseZoom = 3;
constexpr double kSkirtMax = 20000.0;
constexpr double kUnknownMaxHeight = 4500.0;
constexpr double kFadeSeconds = 0.35;

double spanDeg(int level) { return 180.0 / static_cast<double>(1 << level); }

GeoRect viewFootprint(const Camera& cam) {
    const Vec3 eye = cam.eye();
    const Mat4& inv = cam.invViewProjRel();
    const double k = kWgs84A / kWgs84B;
    const Vec3 es{eye.x, eye.y, eye.z * k};
    const double ee = dot(es, es) - kWgs84A * kWgs84A;
    constexpr int kSteps = 12;
    std::vector<double> lons;
    lons.reserve((kSteps + 1) * (kSteps + 1));
    double south = 90.0, north = -90.0;
    for (int j = 0; j <= kSteps; ++j)
        for (int i = 0; i <= kSteps; ++i) {
            const double x = -1.0 + 2.0 * i / kSteps, y = -1.0 + 2.0 * j / kSteps;
            const Vec4 a = inv * Vec4{x, y, 0.0, 1.0};
            const Vec4 b = inv * Vec4{x, y, 1.0, 1.0};
            const Vec3 dir = normalize(Vec3{b.x / b.w - a.x / a.w, b.y / b.w - a.y / a.w, b.z / b.w - a.z / a.w});
            const Vec3 ds{dir.x, dir.y, dir.z * k};
            const double qa = dot(ds, ds), qb = dot(es, ds);
            const double disc = qb * qb - qa * ee;
            Vec3 p;
            if (disc >= 0.0 && -qb - std::sqrt(disc) > 0.0) {
                p = eye + dir * ((-qb - std::sqrt(disc)) / qa);
            } else {
                const Vec3 c = normalize(es + ds * std::max(-qb / qa, 0.0)) * kWgs84A;
                p = Vec3{c.x, c.y, c.z / k};
            }
            const GeoPoint g = ecefToGeodetic(p);
            lons.push_back(g.lon);
            south = std::min(south, g.lat);
            north = std::max(north, g.lat);
        }
    bool full = false;
    for (int pole = -1; pole <= 1; pole += 2) {
        const Vec3 at{0.0, 0.0, pole * kWgs84B};
        if (dot(Vec3{0.0, 0.0, static_cast<double>(pole)}, eye - at) <= 0.0) continue;
        const Vec4 c = cam.viewProjRel() * Vec4{at.x - eye.x, at.y - eye.y, at.z - eye.z, 1.0};
        if (c.w <= 0.0 || std::abs(c.x) > c.w || std::abs(c.y) > c.w) continue;
        if (pole > 0) north = 90.0;
        else south = -90.0;
        full = true;
    }
    std::sort(lons.begin(), lons.end());
    double gap = lons.front() + 360.0 - lons.back(), west = lons.front(), east = lons.back();
    for (size_t i = 1; i < lons.size(); ++i)
        if (lons[i] - lons[i - 1] > gap) {
            gap = lons[i] - lons[i - 1];
            west = lons[i];
            east = lons[i - 1] + 360.0;
        }
    const double latPad = std::max(0.05, (north - south) * 0.05), lonPad = std::max(0.05, (east - west) * 0.05);
    south = std::max(-90.0, south - latPad);
    north = std::min(90.0, north + latPad);
    if (full || gap < 30.0 || east - west + 2.0 * lonPad >= 360.0) return {-180.0, south, 180.0, north};
    return {west - lonPad, south, east + lonPad, north};
}

void clipToView(const GeoRect& r, const GeoRect& view, const Camera& cam, const GeoPoint& eyeGeo, std::vector<ViewRect>& out) {
    const double s = std::max(r.south, view.south), n = std::min(r.north, view.north);
    if (s >= n) return;
    for (double shift : {-360.0, 0.0, 360.0}) {
        const double w = std::max(r.west, view.west + shift), e = std::min(r.east, view.east + shift);
        if (w >= e) continue;
        const double mid = (w + e) * 0.5;
        const double lon = std::clamp(mid + std::remainder(eyeGeo.lon - mid, 360.0), w, e);
        const double lat = std::clamp(eyeGeo.lat, s, n);
        const double dist = length(geodeticToEcef(lat, lon, 0.0) - cam.eye());
        out.push_back({GeoRect{w, s, e, n}, cam.pixelSizeAt(std::max(dist, 1.0))});
    }
}

}

GlobeRenderer::GlobeRenderer() = default;

GlobeRenderer::~GlobeRenderer() { shutdown(); }

double GlobeRenderer::now() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

GlobeRenderer::Bounds GlobeRenderer::bounds(const QuadKey& k) {
    double s = spanDeg(k.level);
    double west = -180.0 + k.x * s;
    double north = 90.0 - k.y * s;
    return {west, north - s, west + s, north};
}

bool GlobeRenderer::init() {
    if (ready_) return true;
    if (!budgetSet_) {
        double ram = physicalMemoryMb();
        double vram = videoMemoryMb();
#if defined(__EMSCRIPTEN__)
        (void)ram;
        (void)vram;
        setMemoryBudget(512);
#elif defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
        if (ram > 0) setMemoryBudget(static_cast<int>(std::clamp(ram * 0.16, 256.0, 1024.0)));
#else
        if (vram > 0) setMemoryBudget(static_cast<int>(std::clamp(vram * 0.35, 256.0, 1536.0)));
        else if (ram > 0) setMemoryBudget(static_cast<int>(std::clamp(ram * 0.03, 256.0, 768.0)));
#endif
    }
    fitPools();
    sg_enable_stats();
    sg_backend backend = sg_query_backend();
    holeMarker_ = std::getenv("OTK_HOLES") != nullptr;
    if (const char* v = std::getenv("OTK_LOD_BLEND")) lodBlend_ = std::atoi(v) != 0;
    if (const char* v = std::getenv("OTK_UPGRADE_FADE")) fadeUpgrades_ = std::atoi(v) != 0;
    busyLog_ = std::getenv("OTK_BUSY_LOG") != nullptr;
    if (const char* v = std::getenv("OTK_VISUAL")) visualStats_ = std::atoi(v) != 0;
    if (const char* v = std::getenv("OTK_VISUAL_LOG")) visualLog_ = std::fopen(v, "w");
    if (const char* v = std::getenv("OTK_RERENDER_GAP")) rerenderGap_ = std::max(0.0, std::atof(v));
    if (const char* v = std::getenv("OTK_MERGE_FADE")) mergeSeconds_ = std::max(0.0, std::atof(v));
    if (const char* v = std::getenv("OTK_MERGE_GAP")) mergeGap_ = std::max(0.0, std::atof(v));
    if (const char* v = std::getenv("OTK_UNIFORM_SHARE")) uniformShare_ = std::clamp(std::atof(v), 0.0, 1.0);
    if (const char* v = std::getenv("OTK_REVEAL_HOLD")) revealHold_ = std::max(0.0, std::atof(v));
    if (const char* v = std::getenv("OTK_SETTLE")) settleFreeze_ = std::atoi(v) != 0;
    if (const char* v = std::getenv("OTK_SETTLE_WAIT")) settleWait_ = std::max(0.0, std::atof(v));
    if (const char* v = std::getenv("OTK_UNIFORM_LOD")) uniformLod_ = std::atoi(v) != 0;
    if (const char* v = std::getenv("OTK_LINES3D")) lines3d_ = std::atoi(v) != 0;
    if (const char* v = std::getenv("OTK_REVEAL_GAP")) revealGap_ = std::max(0.0, std::atof(v));
    if (const char* v = std::getenv("OTK_MOTION_HOLD")) motionHold_ = std::max(0.0, std::atof(v));
    if (const char* v = std::getenv("OTK_MOTION_SPEED")) motionSpeed_ = std::max(0.0, std::atof(v));
    if (const char* v = std::getenv("OTK_MAX_DIVE")) maxDive_ = std::atoi(v);
    if (const char* v = std::getenv("OTK_LOD_MORPH")) lodMorph_ = std::clamp(std::atof(v), 0.0, 1.0);
    if (const char* v = std::getenv("OTK_MARGIN")) marginPrefetch_ = std::max(1.0, std::atof(v));
    if (std::getenv("OTK_DIAG")) diagnostics_ = true;
    if (const char* pp = std::getenv("OTK_PROFILE")) {
        profiling_ = true;
        profilePath_ = pp;
    }
#if defined(OTK_GL_MIPS)
    glMips_ = backend == SG_BACKEND_GLES3 || backend == SG_BACKEND_GLCORE;
    if (const char* v = std::getenv("OTK_BOX_MIPS")) glMips_ = glMips_ && std::atoi(v) == 0;
    if (backend == SG_BACKEND_GLES3 || backend == SG_BACKEND_GLCORE) {
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (renderer && (std::strstr(renderer, "llvmpipe") || std::strstr(renderer, "softpipe") || std::strstr(renderer, "SwiftShader"))) {
            softwareRaster_ = true;
            lodMorph_ = 0.0;
        }
    }
#endif
    flipSurface_ = backend == SG_BACKEND_D3D11 || backend == SG_BACKEND_METAL_IOS ||
                   backend == SG_BACKEND_METAL_MACOS || backend == SG_BACKEND_METAL_SIMULATOR ||
                   backend == SG_BACKEND_WGPU;

    std::vector<std::uint16_t> idx;
    idx.reserve(kIndexCount);
    for (int j = 0; j < kGrid; ++j)
        for (int i = 0; i < kGrid; ++i) {
            auto a = static_cast<std::uint16_t>(j * kPosts + i);
            auto b = static_cast<std::uint16_t>(a + 1);
            auto c = static_cast<std::uint16_t>(a + kPosts);
            auto d = static_cast<std::uint16_t>(c + 1);
            idx.insert(idx.end(), {a, b, d, a, d, c});
        }
    const int skirtBase = kPosts * kPosts;
    auto edge = [&](int e, int i) -> int {
        switch (e) {
        case 0: return i;
        case 1: return kGrid * kPosts + i;
        case 2: return i * kPosts;
        default: return i * kPosts + kGrid;
        }
    };
    for (int e = 0; e < 4; ++e)
        for (int i = 0; i < kGrid; ++i) {
            auto a = static_cast<std::uint16_t>(edge(e, i));
            auto b = static_cast<std::uint16_t>(edge(e, i + 1));
            auto as = static_cast<std::uint16_t>(skirtBase + e * kPosts + i);
            auto bs = static_cast<std::uint16_t>(skirtBase + e * kPosts + i + 1);
            idx.insert(idx.end(), {a, b, bs, a, bs, as});
        }
    sg_buffer_desc ib{};
    ib.usage.index_buffer = true;
    ib.data = {idx.data(), idx.size() * sizeof(std::uint16_t)};
    indexBuffer_ = sg_make_buffer(&ib);

    const float tri[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    sg_buffer_desc sb{};
    sb.usage.vertex_buffer = true;
    sb.data = SG_RANGE(tri);
    skyBuffer_ = sg_make_buffer(&sb);

    sg_buffer_desc vb{};
    vb.usage.vertex_buffer = true;
    vb.usage.write_transient = true;
    vb.size = kSurfaceBufferBytes;
    surfaceBuffer_ = sg_make_buffer(&vb);
    featureBuffer_ = sg_make_buffer(&vb);
    features_.init();
    labels_.init();
    models_.init();
    points_.init();

    const std::uint8_t ocean[4] = {10, 30, 62, 255};
    sg_image_desc fd{};
    fd.width = 1;
    fd.height = 1;
    fd.pixel_format = SG_PIXELFORMAT_RGBA8;
    fd.data.mip_levels[0] = SG_RANGE(ocean);
    fallbackImage_ = sg_make_image(&fd);
    sg_view_desc fv{};
    fv.texture.image = fallbackImage_;
    fallbackView_ = sg_make_view(&fv);

    sg_sampler_desc smp{};
    smp.min_filter = SG_FILTER_LINEAR;
    smp.mag_filter = SG_FILTER_LINEAR;
    smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    imagerySampler_ = sg_make_sampler(&smp);
    mipSampler_ = sg_make_sampler(&smp);
    smp.mipmap_filter = SG_FILTER_LINEAR;
    smp.max_anisotropy = 8;
    sampler_ = sg_make_sampler(&smp);

    globeShader_ = sg_make_shader(globe_shader_desc(backend));
    surfaceShader_ = sg_make_shader(surface_shader_desc(backend));
    skyShader_ = sg_make_shader(sky_shader_desc(backend));

    sg_pipeline_desc gp{};
    gp.shader = globeShader_;
    gp.layout.attrs[ATTR_globe_position].format = SG_VERTEXFORMAT_FLOAT3;
    gp.layout.attrs[ATTR_globe_texcoord0].format = SG_VERTEXFORMAT_FLOAT2;
    gp.layout.attrs[ATTR_globe_shade].format = SG_VERTEXFORMAT_FLOAT;
    gp.layout.attrs[ATTR_globe_height].format = SG_VERTEXFORMAT_FLOAT;
    gp.index_type = SG_INDEXTYPE_UINT16;
    gp.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    gp.depth.write_enabled = true;
    gp.cull_mode = SG_CULLMODE_NONE;
    globePipeline_ = sg_make_pipeline(&gp);
    gp.depth.write_enabled = false;
    gp.colors[0].blend.enabled = true;
    gp.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    gp.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    globeBlendPipeline_ = sg_make_pipeline(&gp);

    sg_pipeline_desc sp{};
    sp.shader = surfaceShader_;
    sp.layout.attrs[ATTR_surface_position].format = SG_VERTEXFORMAT_FLOAT2;
    sp.layout.attrs[ATTR_surface_texcoord0].format = SG_VERTEXFORMAT_FLOAT2;
    sp.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
    sp.colors[0].blend.enabled = true;
    sp.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    sp.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    sp.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    sp.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    sp.depth.pixel_format = SG_PIXELFORMAT_NONE;
    sp.sample_count = 1;
    surfacePipeline_ = sg_make_pipeline(&sp);

    sg_pipeline_desc kp{};
    kp.shader = skyShader_;
    kp.layout.attrs[ATTR_sky_position].format = SG_VERTEXFORMAT_FLOAT2;
    kp.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
    kp.depth.write_enabled = false;
    skyPipeline_ = sg_make_pipeline(&kp);

    vfillShader_ = sg_make_shader(vfill_shader_desc(backend));
    vlineShader_ = sg_make_shader(vline_shader_desc(backend));
    auto vecPipe = [&](sg_shader shd, bool line) {
        sg_pipeline_desc d{};
        d.shader = shd;
        if (line) {
            d.layout.attrs[ATTR_vline_position].format = SG_VERTEXFORMAT_FLOAT2;
            d.layout.attrs[ATTR_vline_other].format = SG_VERTEXFORMAT_FLOAT2;
            d.layout.attrs[ATTR_vline_extra].format = SG_VERTEXFORMAT_FLOAT2;
            d.layout.attrs[ATTR_vline_color0].format = SG_VERTEXFORMAT_FLOAT4;
        } else {
            d.layout.attrs[ATTR_vfill_position].format = SG_VERTEXFORMAT_FLOAT2;
            d.layout.attrs[ATTR_vfill_color0].format = SG_VERTEXFORMAT_FLOAT4;
        }
        d.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        d.colors[0].blend.enabled = true;
        d.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        d.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        d.depth.pixel_format = SG_PIXELFORMAT_NONE;
        d.sample_count = 1;
        return sg_make_pipeline(&d);
    };
    vfillPipeline_ = vecPipe(vfillShader_, false);
    vlinePipeline_ = vecPipe(vlineShader_, true);

    mipShader_ = sg_make_shader(mip_shader_desc(backend));
    sg_pipeline_desc mp{};
    mp.shader = mipShader_;
    mp.layout.attrs[ATTR_mip_position].format = SG_VERTEXFORMAT_FLOAT2;
    mp.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
    mp.depth.pixel_format = SG_PIXELFORMAT_NONE;
    mp.sample_count = 1;
    mipPipeline_ = sg_make_pipeline(&mp);
    snapshotShader_ = sg_make_shader(snapshot_shader_desc(backend));
    sg_pipeline_desc np{};
    np.shader = snapshotShader_;
    np.layout.attrs[ATTR_snapshot_position].format = SG_VERTEXFORMAT_FLOAT2;
    np.colors[0].blend.enabled = true;
    np.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
    np.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    np.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    np.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    np.depth.compare = SG_COMPAREFUNC_ALWAYS;
    np.depth.write_enabled = false;
    snapshotPipeline_ = sg_make_pipeline(&np);
    sg_image_desc sd{};
    sd.usage.color_attachment = true;
    sd.width = kSurfaceSize;
    sd.height = kSurfaceSize;
    sd.pixel_format = SG_PIXELFORMAT_RGBA8;
    sd.sample_count = 1;
    scratch_ = sg_make_image(&sd);
    sg_view_desc sa{};
    sa.color_attachment.image = scratch_;
    scratchAttachment_ = sg_make_view(&sa);
    sg_view_desc st{};
    st.texture.image = scratch_;
    scratchTexture_ = sg_make_view(&st);

    ready_ = true;
    return true;
}

void GlobeRenderer::shutdown() {
    if (!ready_) return;
    buildings_.shutdown();
    if (sg_isvalid()) {
        for (auto& [k, t] : terrain_) sg_destroy_buffer(t.vbuf);
        for (auto& [k, s] : surface_) dropSurface(s);
        for (auto& s : freeSurfaces_) dropSurface(s);
        for (auto& [k, list] : retiring_)
            for (auto& r : list) dropSurface(r.surface);
        retiring_.clear();
        retiredCount_ = 0;
        for (auto& [k, t] : imagery_) dropImagery(t);
        sg_destroy_pipeline(globePipeline_);
        sg_destroy_pipeline(globeBlendPipeline_);
        sg_destroy_pipeline(surfacePipeline_);
        sg_destroy_pipeline(skyPipeline_);
        sg_destroy_pipeline(mipPipeline_);
        sg_destroy_pipeline(snapshotPipeline_);
        sg_destroy_shader(snapshotShader_);
        dropSnapshotTarget();
        dropIdleTarget();
        sg_destroy_pipeline(vfillPipeline_);
        sg_destroy_pipeline(vlinePipeline_);
        sg_destroy_shader(vfillShader_);
        sg_destroy_shader(vlineShader_);
        sg_destroy_shader(mipShader_);
        sg_destroy_sampler(mipSampler_);
        sg_destroy_view(scratchAttachment_);
        sg_destroy_view(scratchTexture_);
        sg_destroy_image(scratch_);
        sg_destroy_shader(globeShader_);
        sg_destroy_shader(surfaceShader_);
        sg_destroy_shader(skyShader_);
        sg_destroy_sampler(sampler_);
        sg_destroy_sampler(imagerySampler_);
        sg_destroy_view(fallbackView_);
        sg_destroy_image(fallbackImage_);
        if (backdropImage_.id) {
            sg_destroy_view(backdropView_);
            sg_destroy_image(backdropImage_);
            backdropImage_ = {};
            backdropView_ = {};
        }
        backdropTried_ = false;
        sg_destroy_buffer(indexBuffer_);
        sg_destroy_buffer(skyBuffer_);
        sg_destroy_buffer(surfaceBuffer_);
        sg_destroy_buffer(featureBuffer_);
        for (auto& [id, o] : overlays_) {
            sg_destroy_view(o.view);
            sg_destroy_image(o.image);
        }
        overlays_.clear();
        features_.shutdown();
        labels_.shutdown();
        tilesets_.clear();
        models_.shutdown();
        points_.shutdown();
    }
    terrain_.clear();
    surface_.clear();
    freeSurfaces_.clear();
    imagery_.clear();
    ready_ = false;
}

bool GlobeRenderer::reachabilityChanged() const { return net::reachabilityEpoch() != netEpoch_; }

void GlobeRenderer::setCacheDirectory(const std::string& dir) {
    std::string path = dir;
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += '/';
    path += "tiles.sqlite";
    cacheDir_ = dir;
    if (cache_.open(path)) {
        loader_.setCache(&cache_);
        if (!layers_.empty()) applyLayers();
        if (!elevation_.sources().empty()) setElevation(std::vector<ElevationLayer>(elevation_.sources()));
    }
}

void GlobeRenderer::setMemoryBudget(int megabytes) {
    budgetSet_ = true;
    double mb = std::clamp(megabytes, 96, 8192);
    surfaceCapacity_ = static_cast<size_t>(mb * 0.60 / 0.35);
    budgetMb_ = mb;
    imageryCapacity_ = static_cast<size_t>(mb * 0.20 / 0.262);
    terrainCapacity_ = static_cast<size_t>(mb * 0.20 / 0.034);
    freeSurfacePool_ = std::max<size_t>(16, surfaceCapacity_ / 16);
    fitPools();
}

void GlobeRenderer::fitPools() {
    if (!sg_isvalid()) return;
    sg_desc d = sg_query_desc();
    const size_t buffers = static_cast<size_t>(std::max(d.buffer_pool_size, 128));
    const size_t images = static_cast<size_t>(std::max(d.image_pool_size, 128));
    const size_t views = static_cast<size_t>(std::max(d.view_pool_size, 128));
    const size_t perSurface = 2 * kMips + 1;
    size_t imageRoom = images > 1024 ? images - 512 : images / 2;
    size_t viewRoom = views > 4096 ? views - 2048 : views / 2;
    auto surfaceSlots = [&](size_t cap) { return cap + std::max<size_t>(16, cap / 16); };
    double f = 1.0;
    double imagesNeeded = static_cast<double>(surfaceSlots(surfaceCapacity_) + imageryCapacity_);
    double viewsNeeded = static_cast<double>(surfaceSlots(surfaceCapacity_) * perSurface + imageryCapacity_);
    if (imagesNeeded > imageRoom) f = std::min(f, imageRoom / imagesNeeded);
    if (viewsNeeded > viewRoom) f = std::min(f, viewRoom / viewsNeeded);
    if (f < 1.0) {
        surfaceCapacity_ = std::max<size_t>(64, static_cast<size_t>(surfaceCapacity_ * f));
        imageryCapacity_ = std::max<size_t>(48, static_cast<size_t>(imageryCapacity_ * f));
    }
    freeSurfacePool_ = std::max<size_t>(16, surfaceCapacity_ / 16);
    size_t slots = surfaceSlots(surfaceCapacity_);
    size_t byImages = imageRoom > slots ? imageRoom - slots : 0;
    size_t byViews = viewRoom > slots * perSurface ? viewRoom - slots * perSurface : 0;
    imageryPoolMax_ = std::max(imageryCapacity_, std::min({byImages, byViews, imageryCapacity_ * 4}));
    size_t bufferRoom = buffers > 2048 + 2 * imageryPoolMax_ ? buffers - 1024 - 2 * imageryPoolMax_ : buffers / 2;
    terrainCapacity_ = std::min(terrainCapacity_, std::max<size_t>(256, bufferRoom));
}

size_t GlobeRenderer::imageryLimit() const {
    if (budgetMb_ <= 0 || imageryPoolMax_ <= imageryCapacity_) return imageryCapacity_;
    const double surfaceMb = kSurfaceSize * kSurfaceSize * 4.0 * 4.0 / 3.0 / 1048576.0;
    const double imageryMb = 256.0 * 256.0 * 4.0 / 1048576.0;
    const double terrainMb = kVertexCount * kStride * 4.0 / 1048576.0;
    double room = budgetMb_ - static_cast<double>(surface_.size() + freeSurfaces_.size()) * surfaceMb - static_cast<double>(terrain_.size()) * terrainMb;
    size_t fit = room > 0 ? static_cast<size_t>(room / imageryMb) : 0;
    return std::clamp(fit, imageryCapacity_, imageryPoolMax_);
}

void GlobeRenderer::poolPressure(const char* what) {
    ++invalidResources_;
    size_t terrainBefore = terrainCapacity_, surfaceBefore = surfaceCapacity_, imageryBefore = imageryCapacity_;
    if (std::strcmp(what, "buffer") == 0) terrainCapacity_ = std::max<size_t>(256, std::min(terrainCapacity_, terrain_.size()) * 9 / 10);
    else {
        surfaceCapacity_ = std::max<size_t>(64, std::min(surfaceCapacity_, surface_.size()) * 9 / 10);
        imageryCapacity_ = std::max<size_t>(48, std::min(imageryCapacity_, imagery_.size()) * 9 / 10);
        imageryPoolMax_ = std::max(imageryCapacity_, std::min(imageryPoolMax_, imagery_.size() * 9 / 10));
        freeSurfacePool_ = std::max<size_t>(16, surfaceCapacity_ / 16);
        while (freeSurfaces_.size() > freeSurfacePool_) {
            dropSurface(freeSurfaces_.back());
            freeSurfaces_.pop_back();
        }
    }
    diag("pool pressure frame %llu: %s creation failed; terrain cap %zu -> %zu surface cap %zu -> %zu imagery cap %zu -> %zu",
         static_cast<unsigned long long>(frame_), what, terrainBefore, terrainCapacity_, surfaceBefore, surfaceCapacity_, imageryBefore,
         imageryCapacity_);
}

std::vector<std::string> GlobeRenderer::credits() const {
    std::vector<std::pair<std::string, int>> all;
    for (const auto& [id, t] : tilesets_)
        for (const auto& c : t->credits()) {
            bool merged = false;
            for (auto& a : all)
                if (a.first == c.first) {
                    a.second += c.second;
                    merged = true;
                }
            if (!merged) all.push_back(c);
        }
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::string> out;
    for (const auto& l : userLayers_)
        if (l.visible && !l.attribution.empty() && std::find(out.begin(), out.end(), l.attribution) == out.end()) out.push_back(l.attribution);
    for (const auto& a : all) out.push_back(a.first);
    if (labelsOn_) {
        out.push_back("Boundaries: geoBoundaries (CC BY 4.0)");
        out.push_back("Places: GeoNames (CC BY 4.0), Natural Earth");
    }
    if (overlayLayer_ >= 0)
        out.push_back(std::string(buildingsOn_ ? "Roads, buildings and places" : "Roads and places") +
                      ": OpenFreeMap \xC2\xA9 OpenMapTiles \xC2\xA9 OpenStreetMap contributors");
    // if (buildingsOn_) out.push_back("Trees: Meta and World Resources Institute canopy height (CC BY 4.0)");
    return out;
}

void GlobeRenderer::setSunLighting(bool enabled, double unixSeconds) {
    sunOn_ = enabled;
    sunDir_ = sunDirectionEcef(unixSeconds);
    models_.setSun(enabled, sunDir_);
}

std::uint64_t GlobeRenderer::addTileset(const std::string& uri, const TilesetOptions& options, Vec3* center, double* radius) {
    auto t = std::make_unique<Tileset3D>(uri, options, cache_.isOpen() ? &cache_ : nullptr, &offline_);
    if (!t->valid()) return 0;
    if (center) *center = t->center();
    if (radius) *radius = t->radius();
    std::uint64_t id = nextTileset_++;
    tilesets_[id] = std::move(t);
    busy_ = true;
    return id;
}

void GlobeRenderer::setOffline(bool offline) {
    bool reconnect = offline_ && !offline;
    offline_ = offline;
    loader_.setOffline(offline);
    canopy_.setOffline(offline);
    if (reconnect) retryUnavailable();
}

sg_view GlobeRenderer::backdrop() {
    if (backdropTried_ || !backdropEnabled_) return backdropView_;
    backdropTried_ = true;
#if defined(OTK_EMBED_BACKDROP)
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(embedded::world_backdrop, static_cast<int>(embedded::world_backdrop_size), &w, &h, &n, 4);
    if (!px) return backdropView_;
    std::vector<std::vector<std::uint8_t>> mips;
    mips.emplace_back(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
    int mw = w, mh = h;
    while ((mw > 1 || mh > 1) && static_cast<int>(mips.size()) < SG_MAX_MIPMAPS) {
        const int nw = std::max(1, mw / 2), nh = std::max(1, mh / 2);
        const std::vector<std::uint8_t>& src = mips.back();
        std::vector<std::uint8_t> dst(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    const int x0 = std::min(x * 2, mw - 1), x1 = std::min(x * 2 + 1, mw - 1);
                    const int y0 = std::min(y * 2, mh - 1), y1 = std::min(y * 2 + 1, mh - 1);
                    const int sum = src[(static_cast<size_t>(y0) * mw + x0) * 4 + c] + src[(static_cast<size_t>(y0) * mw + x1) * 4 + c] +
                                    src[(static_cast<size_t>(y1) * mw + x0) * 4 + c] + src[(static_cast<size_t>(y1) * mw + x1) * 4 + c];
                    dst[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<std::uint8_t>((sum + 2) / 4);
                }
        mips.push_back(std::move(dst));
        mw = nw;
        mh = nh;
    }
    sg_image_desc d{};
    d.width = w;
    d.height = h;
    d.pixel_format = SG_PIXELFORMAT_RGBA8;
    d.num_mipmaps = static_cast<int>(mips.size());
    for (size_t i = 0; i < mips.size(); ++i) d.data.mip_levels[i] = {mips[i].data(), mips[i].size()};
    backdropImage_ = sg_make_image(&d);
    sg_view_desc v{};
    v.texture.image = backdropImage_;
    backdropView_ = sg_make_view(&v);
#endif
    return backdropView_;
}

bool GlobeRenderer::backdropUv(const QuadKey& key, sg_view& view, float uv[4]) {
    if (!backdropEnabled_ || !backdrop().id || sg_query_view_state(backdropView_) != SG_RESOURCESTATE_VALID) return false;
    const Bounds b = bounds(key);
    const double span = b.north - b.south;
    view = backdropView_;
    uv[0] = static_cast<float>(span / 360.0);
    uv[1] = static_cast<float>(flipSurface_ ? span / 180.0 : -span / 180.0);
    uv[2] = static_cast<float>((b.west + 180.0) / 360.0);
    uv[3] = static_cast<float>(flipSurface_ ? (90.0 + b.south) / 180.0 : (90.0 - b.south) / 180.0);
    return true;
}

void GlobeRenderer::retryUnavailable() {
    buildings_.retry();
    failed_.clear();
    failCount_.clear();
    for (auto& [k, s] : surface_)
        if (!s.complete || s.degraded) {
            s.complete = false;
            s.missing.clear();
        }
    elevation_.clearFailures();
    busy_ = true;
}

void GlobeRenderer::retryLayer(int layer) {
    buildings_.retry();
    for (auto it = failed_.begin(); it != failed_.end();) it = it->first.layer == layer ? failed_.erase(it) : std::next(it);
    for (auto it = failCount_.begin(); it != failCount_.end();) it = it->first.layer == layer ? failCount_.erase(it) : std::next(it);
    busy_ = true;
}

void GlobeRenderer::setLayers(const std::vector<ImageryLayer>& layers) {
    userLayers_ = layers;
    applyLayers();
}

void GlobeRenderer::setLabelsSource(const std::string& source) {
    if (labelsSource_ == source) return;
    labelsSource_ = source;
    buildings_.clear();
    applyLayers();
}

void GlobeRenderer::applyLayers() {
    std::vector<ImageryLayer> all = userLayers_;
    overlayLayer_ = -1;
    if ((labelsOn_ || buildingsOn_) && !labelsSource_.empty()) {
        ImageryLayer l;
        l.name = "Labels";
        l.labels = true;
        l.stream = false;
        l.maxRequests = 4;
        l.minZoom = 0;
        l.maxZoom = 14;
        std::string lower = labelsSource_;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto ends = [&](const char* suffix) {
            const size_t n = std::strlen(suffix);
            return lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0;
        };
        if (ends(".mbtiles")) {
            l.type = SourceType::MBTiles;
            l.path = labelsSource_;
        } else if (ends(".pmtiles")) {
            l.type = SourceType::PMTiles;
            l.path = labelsSource_;
        } else {
            l.type = SourceType::Mvt;
            l.url = labelsSource_;
        }
        overlayLayer_ = static_cast<int>(all.size());
        all.push_back(std::move(l));
    }
    setLayerList(all);
}

void GlobeRenderer::setLayerList(const std::vector<ImageryLayer>& layers) {
    std::vector<int> from(layers.size(), -1);
    std::vector<bool> taken(layers_.size(), false);
    std::vector<std::string> oldIds;
    for (const auto& l : layers_) oldIds.push_back(layerIdentity(l));
    bool same = layers.size() == layers_.size();
    for (size_t i = 0; i < layers.size(); ++i) {
        std::string id = layerIdentity(layers[i]);
        for (size_t j = 0; j < layers_.size(); ++j)
            if (!taken[j] && oldIds[j] == id) {
                from[i] = static_cast<int>(j);
                taken[j] = true;
                break;
            }
        same = same && from[i] == static_cast<int>(i);
    }
    if (visualLog_)
        std::fprintf(visualLog_, "layers frame %llu t %.3f same %d count %zu -> %zu labels %d buildings %d\n", static_cast<unsigned long long>(frame_), now(),
                     same ? 1 : 0, layers_.size(), layers.size(), labelsOn_ ? 1 : 0, buildingsOn_ ? 1 : 0);
    layers_ = layers;
    std::vector<float> opacity(layers.size(), -1.0f);
    for (size_t i = 0; i < layers.size(); ++i)
        if (from[i] >= 0 && from[i] < static_cast<int>(layerOpacity_.size())) opacity[i] = layerOpacity_[from[i]];
    layerOpacity_ = std::move(opacity);
    if (!same) {
        loader_.setLayers(layers, from);
        matrices_ = loader_.matrices();
        vectorLayers_ = loader_.vectorLayers();
        std::vector<int> to(taken.size(), -1);
        for (size_t i = 0; i < from.size(); ++i)
            if (from[i] >= 0) to[from[i]] = static_cast<int>(i);
        std::unordered_map<TileId, ImageryTexture, TileIdHash> kept;
        for (auto& [k, t] : imagery_) {
            int n = k.layer >= 0 && k.layer < static_cast<int>(to.size()) ? to[k.layer] : -1;
            if (n < 0) {
                dropImagery(t);
                continue;
            }
            TileId id = k;
            id.layer = n;
            kept.emplace(id, std::move(t));
        }
        imagery_ = std::move(kept);
        std::unordered_map<TileId, double, TileIdHash> failed;
        for (auto& [k, v] : failed_) {
            int n = k.layer >= 0 && k.layer < static_cast<int>(to.size()) ? to[k.layer] : -1;
            if (n < 0) continue;
            TileId id = k;
            id.layer = n;
            failed.emplace(id, v);
        }
        failed_ = std::move(failed);
        failCount_.clear();
    }
    for (auto& [k, s] : surface_) {
        s.complete = false;
        s.missing.clear();
    }
    baseLayer_ = -1;
    for (int l = 0; l < static_cast<int>(layers_.size()); ++l)
        if (shown(l) && !layers_[l].polarFill && !layers_[l].labels) {
            baseLayer_ = l;
            break;
        }
    const bool photo = baseLayer_ < 0 || layers_[baseLayer_].photo;
    if (photo != basePhoto_) {
        basePhoto_ = photo;
        buildings_.clear();
    }
    usefulLevel_ = kMaxLevel;
    int best = -1;
    bool vectorOnly = true;
    for (size_t l = 0; l < matrices_.size() && l < layers_.size(); ++l) {
        const TileMatrix& m = matrices_[l];
        if (m.levels.empty() || !shown(static_cast<int>(l))) continue;
        if (l < vectorLayers_.size() && vectorLayers_[l]) continue;
        vectorOnly = false;
        double deg = m.levels.back().resolutionX / m.unitsPerDegree();
        if (deg <= 0) continue;
        best = std::max(best, static_cast<int>(std::ceil(std::log2(180.0 / kSurfaceSize / deg))) + 1);
    }
    if (!vectorOnly && best > 0) usefulLevel_ = std::clamp(best, kMinLevel + 1, kMaxLevel);
    ++imageryVersion_;
    if (!same) {
        switchVersion_ = imageryVersion_;
        switchAt_ = now();
        if (snapshotShown_ && settleSnapshot_) {
            snapshotStart_ = now();
            snapshotFade_ = -1.0;
            settleQuiet_ = 0;
            switchHold_ = true;
        } else if (fadeUpgrades_ && settleFreeze_) {
            switchWanted_ = true;
        }
    }
    busy_ = true;
}

double GlobeRenderer::gridStep(const Camera& cam, int type) const {
    double s = cam.gsd * 70.0 * dpiScale_;
    if (type == 0) return s <= 1000.0 ? 1000.0 : s <= 10000.0 ? 10000.0 : s <= 100000.0 ? 100000.0 : 0.0;
    return graticuleStep(s / 111320.0, type == 2);
}

int GlobeRenderer::maxRefine() const {
    bool needsDetail = !features_.empty() || anyGrid() || !overlays_.empty() || labelsOn_ || contours_;
    return needsDetail ? kMaxLevel : usefulLevel_;
}

void GlobeRenderer::setElevation(const std::vector<ElevationLayer>& layers) {
    elevation_.setSources(layers, cache_.isOpen() ? &cache_ : nullptr, &offline_);
    invalidateTerrain();
}

void GlobeRenderer::setTerrainEnabled(bool enabled) {
    if (terrainEnabled_ == enabled) return;
    terrainEnabled_ = enabled;
    invalidateTerrain();
}

void GlobeRenderer::setTerrainExaggeration(double factor) {
    if (!(factor > 0) || factor == exaggeration_) return;
    exaggeration_ = factor;
    invalidateTerrain();
}

void GlobeRenderer::markDirty(const GeoRect& rect) {
    for (auto& [k, s] : surface_) {
        Bounds b = bounds(k);
        if (rect.intersects({b.west, b.south, b.east, b.north})) {
            s.complete = false;
            s.missing.clear();
        }
    }
    busy_ = true;
}

void GlobeRenderer::setContours(std::shared_ptr<const ContourSet> set) {
    if (set == contours_) return;
    if (contours_) markDirty(contours_->bounds);
    contours_ = std::move(set);
    labels_.setContours(contours_);
    if (contours_) markDirty(contours_->bounds);
}

BuildingLayer::ImageryFn GlobeRenderer::roofImagery() {
    if (baseLayer_ < 0 || baseLayer_ >= static_cast<int>(matrices_.size())) return {};
    if (baseLayer_ < static_cast<int>(layers_.size()) && !layers_[baseLayer_].photo) return {};
    const int layer = baseLayer_;
    return [this, layer](double w, double s, double e, double n, int level, bool request, std::vector<BuildingLayer::Piece>& out) {
        out.clear();
        const TileMatrix& bm = matrices_[layer];
        const double maxLat = maxLatitude(bm.srid);
        if (s < -maxLat || n > maxLat || !(e > w) || !(n > s) || bm.levels.empty()) return;
        int lz = std::min({level, bm.maxZoom, 19});
        while (lz > bm.minZoom && !bm.hasLevel(lz)) --lz;
        if (lz < 0 || !bm.hasLevel(lz)) return;
        const double X0 = projectX(bm.srid, w), X1 = projectX(bm.srid, e), Y0 = projectY(bm.srid, s), Y1 = projectY(bm.srid, n);
        const double sx = bm.spanX(lz), sy = bm.spanY(lz);
        const int tx0 = static_cast<int>(std::floor((X0 - bm.originX) / sx)), tx1 = static_cast<int>(std::floor((X1 - bm.originX) / sx));
        const int ty0 = static_cast<int>(std::floor((bm.originY - Y1) / sy)), ty1 = static_cast<int>(std::floor((bm.originY - Y0) / sy));
        if ((tx1 - tx0 + 1) * (ty1 - ty0 + 1) > 16) return;
        const double t = now();
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                const double px0 = std::max(X0, bm.originX + tx * sx), px1 = std::min(X1, bm.originX + (tx + 1) * sx);
                const double py1 = std::min(Y1, bm.originY - ty * sy), py0 = std::max(Y0, bm.originY - (ty + 1) * sy);
                if (px1 <= px0 || py1 <= py0) continue;
                BuildingLayer::Piece p;
                p.fx0 = static_cast<float>((px0 - X0) / (X1 - X0));
                p.fx1 = static_cast<float>((px1 - X0) / (X1 - X0));
                p.fy0 = static_cast<float>((Y1 - py1) / (Y1 - Y0));
                p.fy1 = static_cast<float>((Y1 - py0) / (Y1 - Y0));
                const TileId target{layer, lz, tx, ty};
                if (request && imagery_.find(target) == imagery_.end()) {
                    auto f = failed_.find(target);
                    if (f == failed_.end() || f->second <= t) want(target, 0.5);
                }
                const double cx = 0.5 * (px0 + px1), cy = 0.5 * (py0 + py1);
                for (int z = lz; z >= bm.minZoom && z >= 0; --z) {
                    if (!bm.hasLevel(z)) continue;
                    const double zx = bm.spanX(z), zy = bm.spanY(z);
                    const int ax = static_cast<int>(std::floor((cx - bm.originX) / zx)), ay = static_cast<int>(std::floor((bm.originY - cy) / zy));
                    auto it = imagery_.find(TileId{layer, z, ax, ay});
                    if (it == imagery_.end() || !it->second.view.id || it->second.vector) continue;
                    it->second.used = frame_;
                    const double minX = bm.originX + ax * zx, maxY = bm.originY - ay * zy;
                    p.view = it->second.view;
                    p.u0 = static_cast<float>(std::clamp((px0 - minX) / zx, 0.0, 1.0));
                    p.u1 = static_cast<float>(std::clamp((px1 - minX) / zx, 0.0, 1.0));
                    p.v0 = static_cast<float>(std::clamp((maxY - py1) / zy, 0.0, 1.0));
                    p.v1 = static_cast<float>(std::clamp((maxY - py0) / zy, 0.0, 1.0));
                    p.z = z;
                    break;
                }
                out.push_back(p);
            }
    };
}

void GlobeRenderer::setBuildings(bool enabled) {
    if (buildingsOn_ == enabled) return;
    buildingsOn_ = enabled;
    if (!enabled) buildings_.clear();
    applyLayers();
    markDirty({-180, -90, 180, 90});
}

void GlobeRenderer::setLabels(bool enabled) {
    if (labelsOn_ == enabled) return;
    labelsOn_ = enabled;
    applyLayers();
    markDirty({-180, -90, 180, 90});
}

void GlobeRenderer::setGrid(bool enabled) {
    std::array<bool, 3> next{};
    if (enabled) next[std::clamp(gridType_, 0, 2)] = true;
    setGrids(next);
}

void GlobeRenderer::setGridType(int type) {
    gridType_ = std::clamp(type, 0, 2);
    if (anyGrid()) setGrid(true);
}

void GlobeRenderer::setGridLayer(int type, bool enabled) {
    std::array<bool, 3> next = gridOn_;
    next[std::clamp(type, 0, 2)] = enabled;
    setGrids(next);
}

void GlobeRenderer::setGrids(const std::array<bool, 3>& on) {
    if (on == gridOn_) return;
    gridOn_ = on;
    gridSpacing_ = {-1, -1, -1};
    markDirty({-180, -90, 180, 90});
}

double GlobeRenderer::terrainHeight(double lat, double lon) const { return elevation_.sample(lat, lon); }

std::uint64_t GlobeRenderer::addOverlay(const GeoRect& rect, int width, int height, const std::vector<std::uint8_t>& rgba,
                                        float opacity) {
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * height * 4) return 0;
    Overlay o;
    o.rect = rect;
    o.opacity = opacity;
    sg_image_desc d{};
    d.width = width;
    d.height = height;
    d.pixel_format = SG_PIXELFORMAT_RGBA8;
    d.data.mip_levels[0] = {rgba.data(), static_cast<size_t>(width) * height * 4};
    o.image = sg_make_image(&d);
    sg_view_desc v{};
    v.texture.image = o.image;
    o.view = sg_make_view(&v);
    std::uint64_t id = nextOverlay_++;
    overlays_[id] = o;
    markDirty(rect);
    return id;
}

bool GlobeRenderer::removeOverlay(std::uint64_t id) {
    auto it = overlays_.find(id);
    if (it == overlays_.end()) return false;
    markDirty(it->second.rect);
    sg_destroy_view(it->second.view);
    sg_destroy_image(it->second.image);
    overlays_.erase(it);
    return true;
}

void GlobeRenderer::invalidateTerrain() {
    buildings_.clear();
    ++terrainEpoch_;
    for (auto& [k, t] : terrain_) t.complete = false;
    busy_ = true;
}

double GlobeRenderer::elevationAt(double lat, double lon) const {
    if (!terrainEnabled_) return 0.0;
    double h = elevation_.sample(lat, lon);
    return std::isnan(h) ? 0.0 : h * exaggeration_;
}

void GlobeRenderer::buildTerrain(const QuadKey& k, TerrainTile& t, double priority) {
    Bounds b = bounds(k);
    double span = b.north - b.south;
    double cell = span / kGrid;
    std::vector<float> h;
    bool complete = true;
    const int n = kPosts + 2;
    if (terrainEnabled_ && !elevation_.empty()) {
        complete = elevation_.gridFor(b.west - cell, std::max(b.south - cell, -90.0), b.east + cell,
                                      std::min(b.north + cell, 90.0), n, priority, h);
        for (float& v : h) v = std::isnan(v) ? 0.0f : static_cast<float>(v * exaggeration_);
    } else {
        h.assign(static_cast<size_t>(n) * n, 0.0f);
    }
    float minH = std::numeric_limits<float>::max(), maxH = -std::numeric_limits<float>::max();
    for (int j = 1; j <= kPosts; ++j)
        for (int i = 1; i <= kPosts; ++i) {
            float v = h[static_cast<size_t>(j) * n + i];
            minH = std::min(minH, v);
            maxH = std::max(maxH, v);
        }
    double clat = 0.5 * (b.south + b.north), clon = 0.5 * (b.west + b.east);
    t.center = geodeticToEcef(clat, clon, 0.5 * (minH + maxH));
    t.minH = minH;
    t.maxH = maxH;

    std::vector<float> v;
    v.reserve(static_cast<size_t>(kVertexCount) * kStride);
    std::vector<float> shade(static_cast<size_t>(kPosts) * kPosts);
    for (int j = 0; j < kPosts; ++j) {
        double lat = b.south + span * j / kGrid;
        double dy = 111132.954 * cell;
        double dx = std::max(111319.49 * std::cos(lat * kDegToRad) * cell, dy * 0.25);
        for (int i = 0; i < kPosts; ++i) {
            double lon = b.west + span * i / kGrid;
            size_t c = static_cast<size_t>(j + 1) * n + (i + 1);
            float hc = h[c];
            double nx = (h[c - 1] - h[c + 1]) / (2.0 * dx);
            double ny = (h[c - n] - h[c + n]) / (2.0 * dy);
            double nz = 1.0 / std::sqrt(nx * nx + ny * ny + 1.0);
            shade[static_cast<size_t>(j) * kPosts + i] = static_cast<float>(nz);
            Vec3 p = geodeticToEcef(lat, lon, hc) - t.center;
            v.insert(v.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z),
                               static_cast<float>(i) / kGrid, static_cast<float>(j) / kGrid,
                               static_cast<float>(nz), hc});
        }
    }
    double edgeDev = 0;
    for (int e = 0; e < 4; ++e) {
        auto edge = [&](int i) {
            int gi = e < 2 ? i : (e == 2 ? 0 : kGrid);
            int gj = e < 2 ? (e == 0 ? 0 : kGrid) : i;
            return static_cast<double>(h[static_cast<size_t>(gj + 1) * n + (gi + 1)]);
        };
        for (int stride : {2, 4, 8})
            for (int i = 0; i <= kGrid; ++i) {
                int a = (i / stride) * stride, z = std::min(a + stride, kGrid);
                if (i == a || z == a) continue;
                double f = static_cast<double>(i - a) / (z - a);
                edgeDev = std::max(edgeDev, std::abs(edge(i) - (edge(a) * (1.0 - f) + edge(z) * f)));
            }
    }
    if (terrainEnabled_ && !elevation_.empty())
        for (int coarse : {kGrid / 2 + 1, kGrid / 4 + 1}) {
            std::vector<float> ch;
            elevation_.gridFor(b.west, b.south, b.east, b.north, coarse, priority, ch);
            if (ch.size() != static_cast<size_t>(coarse) * coarse) continue;
            const int step = kGrid / (coarse - 1);
            for (int e = 0; e < 4; ++e)
                for (int i = 0; i <= kGrid; ++i) {
                    int gi = e < 2 ? i : (e == 2 ? 0 : kGrid);
                    int gj = e < 2 ? (e == 0 ? 0 : kGrid) : i;
                    double fine = h[static_cast<size_t>(gj + 1) * n + (gi + 1)];
                    int a = (i / step) * step, z = std::min(a + step, kGrid);
                    double f = z == a ? 0.0 : static_cast<double>(i - a) / (z - a);
                    auto at = [&](int p) {
                        int ci = e < 2 ? p / step : (e == 2 ? 0 : coarse - 1);
                        int cj = e < 2 ? (e == 0 ? 0 : coarse - 1) : p / step;
                        return static_cast<double>(ch[static_cast<size_t>(cj) * coarse + ci]);
                    };
                    double c0 = at(a), c1 = at(z);
                    if (std::isnan(c0) || std::isnan(c1)) continue;
                    edgeDev = std::max(edgeDev, std::abs(fine - (c0 * (1.0 - f) + c1 * f) * exaggeration_));
                }
        }
    double skirt = std::clamp(std::max({span * 111320.0 * 0.02, static_cast<double>(maxH - minH) * 0.25, edgeDev * 1.5, 1.0}), 1.0, kSkirtMax) *
                   std::max(1.0, exaggeration_);
    for (int e = 0; e < 4; ++e)
        for (int i = 0; i < kPosts; ++i) {
            int gi = e == 0 ? i : e == 1 ? i : e == 2 ? 0 : kGrid;
            int gj = e == 0 ? 0 : e == 1 ? kGrid : i;
            double lat = b.south + span * gj / kGrid, lon = b.west + span * gi / kGrid;
            float hc = h[static_cast<size_t>(gj + 1) * n + (gi + 1)];
            Vec3 p = geodeticToEcef(lat, lon, hc) - t.center;
            v.insert(v.end(), {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z),
                               static_cast<float>(gi) / kGrid, static_cast<float>(gj) / kGrid,
                               shade[static_cast<size_t>(gj) * kPosts + gi], hc});
        }
    t.skirt = static_cast<float>(skirt);
    t.edges.resize(static_cast<size_t>(4) * kPosts);
    for (int e = 0; e < 4; ++e)
        for (int i = 0; i < kPosts; ++i) {
            int gi = e < 2 ? i : (e == 2 ? 0 : kGrid);
            int gj = e < 2 ? (e == 0 ? 0 : kGrid) : i;
            t.edges[static_cast<size_t>(e) * kPosts + i] = h[static_cast<size_t>(gj + 1) * n + (gi + 1)];
        }
    if (t.vbuf.id) sg_destroy_buffer(t.vbuf);
    sg_buffer_desc d{};
    d.usage.vertex_buffer = true;
    d.data = {v.data(), v.size() * sizeof(float)};
    t.vbuf = sg_make_buffer(&d);
    if (sg_query_buffer_state(t.vbuf) != SG_RESOURCESTATE_VALID) {
        poolPressure("buffer");
        if (t.vbuf.id) sg_destroy_buffer(t.vbuf);
        t.vbuf = {};
        t.complete = false;
        return;
    }
    t.complete = complete;
    t.version = elevation_.version() + terrainEpoch_ * 1000000;
}

GlobeRenderer::TerrainTile& GlobeRenderer::terrain(const QuadKey& k) {
    auto it = terrain_.find(k);
    if (it != terrain_.end()) {
        if (sg_query_buffer_state(it->second.vbuf) != SG_RESOURCESTATE_VALID) buildTerrain(k, it->second, 0.0);
        return it->second;
    }
    TerrainTile& t = terrain_[k];
    buildTerrain(k, t, 0.0);
    return t;
}

void GlobeRenderer::dropSurface(SurfaceTile& s) {
    sg_destroy_view(s.texture);
    for (int i = 0; i < kMips; ++i) {
        sg_destroy_view(s.levels[i]);
        sg_destroy_view(s.sources[i]);
    }
    sg_destroy_image(s.image);
}

GlobeRenderer::SurfaceTile* GlobeRenderer::acquireSurface(const QuadKey& k) {
    auto it = surface_.find(k);
    if (it != surface_.end()) return &it->second;
    SurfaceTile s = freshSurface();
    s.complete = false;
    s.version = -1;
    return &surface_.emplace(k, s).first->second;
}

void GlobeRenderer::recycleSurface(SurfaceTile& s) {
    if (freeSurfaces_.size() < freeSurfacePool_) freeSurfaces_.push_back(s);
    else dropSurface(s);
}

GlobeRenderer::SurfaceTile GlobeRenderer::freshSurface() {
    SurfaceTile s;
    if (!freeSurfaces_.empty()) {
        s = freeSurfaces_.back();
        freeSurfaces_.pop_back();
    } else {
        sg_image_desc d{};
        d.usage.color_attachment = true;
        d.width = kSurfaceSize;
        d.height = kSurfaceSize;
        d.pixel_format = SG_PIXELFORMAT_RGBA8;
        d.sample_count = 1;
        d.num_mipmaps = kMips;
        s.image = sg_make_image(&d);
        for (int i = 0; i < kMips; ++i) {
            sg_view_desc a{};
            a.color_attachment.image = s.image;
            a.color_attachment.mip_level = i;
            s.levels[i] = sg_make_view(&a);
            sg_view_desc t{};
            t.texture.image = s.image;
            t.texture.mip_levels.base = i;
            t.texture.mip_levels.count = 1;
            s.sources[i] = sg_make_view(&t);
        }
        s.attachment = s.levels[0];
        sg_view_desc t{};
        t.texture.image = s.image;
        s.texture = sg_make_view(&t);
    }
    return s;
}

bool GlobeRenderer::ensureSnapshotTarget(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (snapshotWidth_ == width && snapshotHeight_ == height && snapshotTexture_.id) return true;
    dropSnapshotTarget();
    const sg_desc desc = sg_query_desc();
    const sg_pixel_format color = desc.environment.defaults.color_format;
    const sg_pixel_format depth = desc.environment.defaults.depth_format;
    const int samples = std::max(1, desc.environment.defaults.sample_count);
    sg_image_desc id{};
    id.width = width;
    id.height = height;
    id.pixel_format = color;
    id.sample_count = 1;
    if (samples > 1) id.usage.resolve_attachment = true;
    else id.usage.color_attachment = true;
    snapshotImage_ = sg_make_image(&id);
    if (samples > 1) {
        sg_image_desc md{};
        md.usage.color_attachment = true;
        md.width = width;
        md.height = height;
        md.pixel_format = color;
        md.sample_count = samples;
        snapshotMsaa_ = sg_make_image(&md);
        sg_view_desc cv{};
        cv.color_attachment.image = snapshotMsaa_;
        snapshotColor_ = sg_make_view(&cv);
        sg_view_desc rv{};
        rv.resolve_attachment.image = snapshotImage_;
        snapshotResolve_ = sg_make_view(&rv);
    } else {
        sg_view_desc cv{};
        cv.color_attachment.image = snapshotImage_;
        snapshotColor_ = sg_make_view(&cv);
    }
    if (depth != SG_PIXELFORMAT_NONE) {
        sg_image_desc dd{};
        dd.usage.depth_stencil_attachment = true;
        dd.width = width;
        dd.height = height;
        dd.pixel_format = depth;
        dd.sample_count = samples;
        snapshotDepth_ = sg_make_image(&dd);
        sg_view_desc dv{};
        dv.depth_stencil_attachment.image = snapshotDepth_;
        snapshotDepthView_ = sg_make_view(&dv);
    }
    sg_view_desc tv{};
    tv.texture.image = snapshotImage_;
    snapshotTexture_ = sg_make_view(&tv);
    snapshotWidth_ = width;
    snapshotHeight_ = height;
    const bool ok = sg_query_view_state(snapshotTexture_) == SG_RESOURCESTATE_VALID &&
                    sg_query_view_state(snapshotColor_) == SG_RESOURCESTATE_VALID &&
                    (samples == 1 || sg_query_view_state(snapshotResolve_) == SG_RESOURCESTATE_VALID) &&
                    (depth == SG_PIXELFORMAT_NONE || sg_query_view_state(snapshotDepthView_) == SG_RESOURCESTATE_VALID);
    if (!ok) dropSnapshotTarget();
    return ok;
}

bool GlobeRenderer::ensureIdleTarget(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (idleWidth_ == width && idleHeight_ == height && idleTexture_.id) return true;
    dropIdleTarget();
    const sg_desc desc = sg_query_desc();
    const sg_pixel_format color = desc.environment.defaults.color_format;
    const sg_pixel_format depth = desc.environment.defaults.depth_format;
    const int samples = std::max(1, desc.environment.defaults.sample_count);
    sg_image_desc id{};
    id.width = width;
    id.height = height;
    id.pixel_format = color;
    id.sample_count = 1;
    if (samples > 1) id.usage.resolve_attachment = true;
    else id.usage.color_attachment = true;
    idleImage_ = sg_make_image(&id);
    if (samples > 1) {
        sg_image_desc md{};
        md.usage.color_attachment = true;
        md.width = width;
        md.height = height;
        md.pixel_format = color;
        md.sample_count = samples;
        idleMsaa_ = sg_make_image(&md);
        sg_view_desc cv{};
        cv.color_attachment.image = idleMsaa_;
        idleColor_ = sg_make_view(&cv);
        sg_view_desc rv{};
        rv.resolve_attachment.image = idleImage_;
        idleResolve_ = sg_make_view(&rv);
    } else {
        sg_view_desc cv{};
        cv.color_attachment.image = idleImage_;
        idleColor_ = sg_make_view(&cv);
    }
    if (depth != SG_PIXELFORMAT_NONE) {
        sg_image_desc dd{};
        dd.usage.depth_stencil_attachment = true;
        dd.width = width;
        dd.height = height;
        dd.pixel_format = depth;
        dd.sample_count = samples;
        idleDepth_ = sg_make_image(&dd);
        sg_view_desc dv{};
        dv.depth_stencil_attachment.image = idleDepth_;
        idleDepthView_ = sg_make_view(&dv);
    }
    sg_view_desc tv{};
    tv.texture.image = idleImage_;
    idleTexture_ = sg_make_view(&tv);
    idleWidth_ = width;
    idleHeight_ = height;
    const bool ok = sg_query_view_state(idleTexture_) == SG_RESOURCESTATE_VALID && sg_query_view_state(idleColor_) == SG_RESOURCESTATE_VALID &&
                    (samples == 1 || sg_query_view_state(idleResolve_) == SG_RESOURCESTATE_VALID) &&
                    (depth == SG_PIXELFORMAT_NONE || sg_query_view_state(idleDepthView_) == SG_RESOURCESTATE_VALID);
    if (!ok) dropIdleTarget();
    return ok;
}

void GlobeRenderer::dropIdleTarget() {
    for (sg_view* v : {&idleColor_, &idleResolve_, &idleDepthView_, &idleTexture_}) {
        if (v->id) sg_destroy_view(*v);
        *v = sg_view{};
    }
    for (sg_image* i : {&idleImage_, &idleMsaa_, &idleDepth_}) {
        if (i->id) sg_destroy_image(*i);
        *i = sg_image{};
    }
    idleWidth_ = 0;
    idleHeight_ = 0;
    idleValid_ = false;
}

bool GlobeRenderer::revealGate(double t) const {
    if (revealHold_ <= 0.0) return true;
    if (heldPrev_ == 0 || t - lastReveal_ < revealGap_) return false;
    const double still = t - lastMotion_;
    const double heldAge = t - oldestHeldPrev_;
    if (still < 0.15) return motionHold_ > 0.0 && heldAge >= motionHold_;
    return waitingPrev_ == 0 || std::min(heldAge, still) >= revealHold_;
}

bool GlobeRenderer::wakeDue() const {
    return waiting_ && (now() >= wakeAt_ || loader_.hasResults() || elevation_.hasResults());
}

bool GlobeRenderer::presentIdle(Camera& cam, const sg_swapchain& swapchain) {
    if (!ready_ || swapchain.width <= 0 || swapchain.height <= 0 || snapshotShown_) return false;
    if (!idleValid_ || wakeDue() || idleWidth_ != swapchain.width || idleHeight_ != swapchain.height) {
        if (!ensureIdleTarget(swapchain.width, swapchain.height)) return false;
        sg_attachments att{};
        att.colors[0] = idleColor_;
        if (idleResolve_.id) att.resolves[0] = idleResolve_;
        if (idleDepthView_.id) att.depth_stencil = idleDepthView_;
        renderingIdle_ = true;
        render(cam, nullptr, &att, swapchain.width, swapchain.height);
        renderingIdle_ = false;
        idleValid_ = !busy_;
    }
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
    pass.action.depth.load_action = SG_LOADACTION_DONTCARE;
    pass.swapchain = swapchain;
    sg_begin_pass(&pass);
    sg_apply_pipeline(snapshotPipeline_);
    sg_bindings b{};
    b.vertex_buffers[0] = skyBuffer_;
    b.views[VIEW_tex] = idleTexture_;
    b.samplers[SMP_smp] = imagerySampler_;
    sg_apply_bindings(&b);
    snapshot_params_t p{};
    p.params[0] = 1.0f;
    p.params[1] = flipSurface_ ? 1.0f : 0.0f;
    sg_apply_uniforms(UB_snapshot_params, SG_RANGE(p));
    sg_draw(0, 3, 1);
    sg_end_pass();
    return true;
}

void GlobeRenderer::dropSnapshotTarget() {
    for (sg_view* v : {&snapshotColor_, &snapshotResolve_, &snapshotDepthView_, &snapshotTexture_}) {
        if (v->id) sg_destroy_view(*v);
        *v = sg_view{};
    }
    for (sg_image* i : {&snapshotImage_, &snapshotMsaa_, &snapshotDepth_}) {
        if (i->id) sg_destroy_image(*i);
        *i = sg_image{};
    }
    snapshotWidth_ = 0;
    snapshotHeight_ = 0;
    snapshotShown_ = false;
}

void GlobeRenderer::drawSnapshot(float alpha, const Camera* now) {
    if (!snapshotTexture_.id || alpha <= 0.0f) return;
    sg_apply_pipeline(snapshotPipeline_);
    sg_bindings b{};
    b.vertex_buffers[0] = skyBuffer_;
    b.views[VIEW_tex] = snapshotTexture_;
    b.samplers[SMP_smp] = imagerySampler_;
    sg_apply_bindings(&b);
    snapshot_params_t p{};
    p.params[0] = alpha;
    p.params[1] = flipSurface_ ? 1.0f : 0.0f;
    if (now) {
        p.params[2] = 1.0f;
        toFloat(now->invViewProjRel(), p.inv_new);
        toFloat(snapshotCam_.viewProjRel(), p.vp_old);
        const Vec3 d = now->eye() - snapshotCam_.eye();
        p.delta[0] = static_cast<float>(d.x);
        p.delta[1] = static_cast<float>(d.y);
        p.delta[2] = static_cast<float>(d.z);
        const double scale = kWgs84A / kWgs84B;
        const Vec3 e = now->eye();
        const Vec3 es{e.x, e.y, e.z * scale};
        const double r = length(es);
        p.up[0] = static_cast<float>(es.x / r);
        p.up[1] = static_cast<float>(es.y / r);
        p.up[2] = static_cast<float>(es.z / r);
        p.up[3] = static_cast<float>(r);
        const double ground = std::isfinite(snapshotCam_.focus.alt) ? std::clamp(snapshotCam_.focus.alt, -500.0, 9000.0) : 0.0;
        const double radius = kWgs84A + std::min(ground, std::max(r - kWgs84A - 1.0, 0.0));
        p.shape[0] = static_cast<float>(radius);
        p.shape[1] = static_cast<float>(scale);
        p.shape[2] = static_cast<float>(r - radius);
    }
    sg_apply_uniforms(UB_snapshot_params, SG_RANGE(p));
    sg_draw(0, 3, 1);
}

GlobeRenderer::NodeInfo GlobeRenderer::info(const Camera& cam, const QuadKey& k) const {
    NodeInfo ni;
    Bounds b = bounds(k);
    double minH = 0, maxH = 0;
    auto tt = terrain_.find(k);
    const bool own = tt != terrain_.end() && tt->second.version >= 0;
    if (own) {
        minH = tt->second.minH;
        maxH = tt->second.maxH;
    }
    if ((!own || !tt->second.complete) && terrainEnabled_ && !elevation_.empty()) {
        double lo = 0.0, hi = kUnknownMaxHeight * exaggeration_;
        bool settled = false;
        for (int up = 1; up <= k.level && !settled; ++up) {
            auto at = terrain_.find(QuadKey{k.level - up, k.x >> up, k.y >> up});
            if (at == terrain_.end() || at->second.version < 0) continue;
            double pad = std::max(100.0 * exaggeration_, (at->second.maxH - at->second.minH) * 0.25);
            lo = std::min(0.0, at->second.minH - pad);
            hi = at->second.complete ? at->second.maxH + pad : std::max(at->second.maxH + pad, kUnknownMaxHeight * exaggeration_);
            settled = at->second.complete;
        }
        minH = own ? std::min(minH, lo) : lo;
        maxH = own ? std::max(maxH, hi) : hi;
    }
    auto cached = spheres_.find(k);
    if (cached == spheres_.end() || cached->second.minH != static_cast<float>(minH) || cached->second.maxH != static_cast<float>(maxH)) {
        double clat = 0.5 * (b.south + b.north), clon = 0.5 * (b.west + b.east);
        Sphere sp;
        sp.center = geodeticToEcef(clat, clon, 0.5 * (minH + maxH));
        for (double la : {b.south, clat, b.north})
            for (double lo : {b.west, clon, b.east}) {
                sp.radius = std::max(sp.radius, length(geodeticToEcef(la, lo, minH) - sp.center));
                sp.radius = std::max(sp.radius, length(geodeticToEcef(la, lo, maxH) - sp.center));
            }
        sp.radius *= 1.02;
        sp.minH = static_cast<float>(minH);
        sp.maxH = static_cast<float>(maxH);
        if (spheres_.size() > 50000) spheres_.clear();
        cached = spheres_.insert_or_assign(k, sp).first;
    }
    Vec3 c = cached->second.center;
    double r = cached->second.radius;
    Vec3 rel = c - eye_;
    double dist = length(rel);
    ni.distance = dist;
    ni.visible = true;
    if (k.level >= kMinLevel) {
        for (int p = 0; p < 4; ++p) {
            const double* pl = &frustum_[p * 4];
            if (pl[0] * rel.x + pl[1] * rel.y + pl[2] * rel.z + pl[3] < -r) {
                ni.visible = false;
                return ni;
            }
        }
        double R = kWgs84B;
        double horizonEye = std::sqrt(std::max(0.0, dot(eye_, eye_) - R * R));
        double outer = length(c) + r;
        double horizonTile = std::sqrt(std::max(0.0, outer * outer - R * R));
        if (dist - r > horizonEye + horizonTile) {
            ni.visible = false;
            return ni;
        }
    }
    double d = std::max(dist - r, 1.0);
    if (uniformMix_ > 0.0) d = std::exp(uniformMix_ * std::log(uniformDist_) + (1.0 - uniformMix_) * std::log(d));
    double texel = spanDeg(k.level) * kDegToRad * kWgs84A / kSurfaceSize;
    if (dist > r) {
        double facing = std::abs(dot(rel * (1.0 / dist), normalize(c)));
        facing += (1.0 - facing) * uniformMix_;
        texel *= std::max(facing, 0.35);
    }
    double threshold = std::max(1.0, dpiScale_ * 0.6) / std::max(imageryScale_, 0.1);
    double ratio = texel / (cam.pixelSizeAt(d) * threshold);
    bool sticky = wasRefined(k);
    int top = maxRefine();
    ni.refine = k.level < kMinLevel || (k.level < top && visits_ < 20000 && (ratio > 1.0 || (sticky && ratio > 0.8)));
    ni.nearRefine = k.level < top && ratio > 0.7;
    ni.ratio = ratio;
    return ni;
}

void GlobeRenderer::buildRoads3D(ImageryTexture& t, int version) {
    if (t.roads3d.id) sg_destroy_buffer(t.roads3d);
    t.roads3d = {};
    t.roads3dCount = 0;
    t.roads3dVersion = version;
    if (!t.vector || t.vector->roads.empty()) return;
    const VectorTile& vt = *t.vector;
    const double n = std::ldexp(1.0, vt.z);
    auto latOf = [&](double yy) { return std::atan(std::sinh(3.14159265358979323846 * (1.0 - 2.0 * yy / n))) * kRadToDeg; };
    const double west = vt.x / n * 360.0 - 180.0, east = (vt.x + 1.0) / n * 360.0 - 180.0;
    const double south = latOf(vt.y + 1.0), north = latOf(vt.y);
    const size_t total = vt.roads.size() / 6;
    const size_t solid = std::min(total, vt.solidRoads);
    std::vector<int> chunkOf(total);
    std::vector<int> rankOf(total);
    for (size_t i = 0; i < total; ++i) {
        const float* v = &vt.roads[i * 6];
        const double mx = vt.originLon + 0.5 * (v[0] + v[2]) - west, my = vt.originLat + 0.5 * (v[1] + v[3]) - south;
        const int cx = std::clamp(static_cast<int>(std::floor(mx / std::max(east - west, 1e-12) * 4.0)), 0, 3);
        const int cy = std::clamp(static_cast<int>(std::floor(my / std::max(north - south, 1e-12) * 4.0)), 0, 3);
        chunkOf[i] = (i < solid ? 0 : 16) + cy * 4 + cx;
        rankOf[i] = LabelLayer::roadRank(v[5]);
    }
    std::array<int, 33> start{};
    for (size_t i = 0; i < total; ++i) ++start[chunkOf[i] + 1];
    for (int c = 0; c < 32; ++c) start[c + 1] += start[c];
    for (auto& ranks : t.roadRanks) ranks.fill(0);
    for (size_t i = 0; i < total; ++i) ++t.roadRanks[chunkOf[i]][rankOf[i] + 1];
    std::array<std::array<int, 12>, 32> slot{};
    for (int c = 0; c < 32; ++c) {
        int at = start[c];
        for (int r = 0; r < 12; ++r) {
            slot[c][r] = at;
            at += t.roadRanks[c][r + 1];
        }
        for (int r = 0; r < 12; ++r) t.roadRanks[c][r + 1] += t.roadRanks[c][r];
    }
    for (GeoRect& box : t.roadBox) box = GeoRect{1e9, 1e9, -1e9, -1e9};
    std::vector<float> sorted(total * 6);
    for (size_t i = 0; i < total; ++i) {
        const int c = chunkOf[i];
        const float* v = &vt.roads[i * 6];
        std::copy(v, v + 6, sorted.begin() + static_cast<std::ptrdiff_t>(slot[c][rankOf[i]]++) * 6);
        GeoRect& box = t.roadBox[c];
        box.west = std::min({box.west, vt.originLon + v[0], vt.originLon + v[2]});
        box.east = std::max({box.east, vt.originLon + v[0], vt.originLon + v[2]});
        box.south = std::min({box.south, vt.originLat + v[1], vt.originLat + v[3]});
        box.north = std::max({box.north, vt.originLat + v[1], vt.originLat + v[3]});
    }
    t.roadChunk = start;
    std::vector<float> out;
    LabelLayer::buildLines3D(sorted, vt.originLat, vt.originLon, [this](double lat, double lon) { return elevationAt(lat, lon); }, out);
    if (out.empty()) return;
    t.roadsOrigin = geodeticToEcef(t.vector->originLat, t.vector->originLon, 0.0);
    sg_buffer_desc bd{};
    bd.usage.vertex_buffer = true;
    bd.data = {out.data(), out.size() * sizeof(float)};
    t.roads3d = sg_make_buffer(&bd);
    if (sg_query_buffer_state(t.roads3d) != SG_RESOURCESTATE_VALID) {
        poolPressure("road lines");
        sg_destroy_buffer(t.roads3d);
        t.roads3d = {};
        return;
    }
    t.roads3dCount = static_cast<int>(out.size() / 12);
}

void GlobeRenderer::setUniform(const Camera& c) {
    uniformDist_ = 0.0;
    uniformMix_ = 0.0;
    if (!uniformLod_ || c.firstPerson || c.hasPose()) return;
    uniformMix_ = std::clamp((25.0 - c.tilt) / 10.0, 0.0, 1.0);
    if (uniformMix_ > 0.0) uniformDist_ = std::max(c.range(), 1.0);
    else uniformMix_ = 0.0;
}

void GlobeRenderer::explain(const Camera& cam, double lat, double lon, const char* tag) const {
    if (!diagnostics_) return;
    std::string line;
    char buf[160];
    for (int level = 0; level <= 22; ++level) {
        double s = spanDeg(level);
        QuadKey k{level, static_cast<int>(std::floor((lon + 180.0) / s)), static_cast<int>(std::floor((90.0 - lat) / s))};
        NodeInfo ni = info(cam, k);
        auto dr = drawn_.find(k);
        bool drawn = dr != drawn_.end() && dr->second + 1 >= frame_;
        auto tt = terrain_.find(k);
        std::snprintf(buf, sizeof(buf), "L%d/%d/%d %s%s%s%s h[%.0f,%.0f] ", level, k.x, k.y, ni.visible ? "v" : "-", ni.refine ? "r" : "-",
                      ready(k) ? "R" : "-", drawn ? "D" : "-", tt == terrain_.end() ? -1.0 : tt->second.minH,
                      tt == terrain_.end() ? -1.0 : tt->second.maxH);
        line += buf;
        if (drawn && ni.refine) {
            for (int i = 0; i < 4; ++i) {
                QuadKey kid{k.level + 1, k.x * 2 + (i & 1), k.y * 2 + (i >> 1)};
                auto ks = surface_.find(kid);
                auto kt = terrain_.find(kid);
                double t = now();
                int failedSoon = 0, failedPermanent = 0, loaded = 0;
                if (ks != surface_.end())
                    for (const TileId& id : ks->second.missing) {
                        if (imagery_.count(id)) ++loaded;
                        auto f = failed_.find(id);
                        if (f == failed_.end()) continue;
                        if (f->second == std::numeric_limits<double>::infinity()) ++failedPermanent;
                        else if (f->second > t) ++failedSoon;
                    }
                std::snprintf(buf, sizeof(buf), "| kid%d surf %s v%d missing %zu (loaded %d retry %d perm %d) terr %s ", i,
                              ks == surface_.end() ? "-" : ks->second.complete ? "C" : "p", ks == surface_.end() ? -9 : ks->second.version,
                              ks == surface_.end() ? size_t(0) : ks->second.missing.size(), loaded, failedSoon, failedPermanent,
                              kt == terrain_.end() ? "-" : kt->second.complete ? "C" : "p");
                line += buf;
            }
        }
        if (!ni.visible || drawn || !ni.refine) break;
    }
    diag("explain %s frame %llu %.5f %.5f: %s", tag, static_cast<unsigned long long>(frame_), lat, lon, line.c_str());
}

double GlobeRenderer::seamSkirt(const QuadKey& k, const TerrainTile& t,
                                const std::unordered_map<QuadKey, const TerrainTile*, QuadKeyHash>& drawn, int coarser[4]) const {
    for (int e = 0; e < 4; ++e) coarser[e] = 0;
    auto profile = [](const TerrainTile& tile, int e, double s) {
        double f = std::clamp(s, 0.0, 1.0) * kGrid;
        int i = std::min(static_cast<int>(f), kGrid - 1);
        double w = f - i;
        const float* row = &tile.edges[static_cast<size_t>(e) * kPosts];
        return row[i] * (1.0 - w) + row[i + 1] * w;
    };
    const int nx = 2 << k.level, ny = 1 << k.level;
    const Bounds a = bounds(k);
    double need = 0;
    for (int dir = 0; dir < 4; ++dir) {
        QuadKey nk = k;
        if (dir == 0) {
            if (k.y + 1 >= ny) continue;
            nk.y = k.y + 1;
        } else if (dir == 1) {
            if (k.y == 0) continue;
            nk.y = k.y - 1;
        } else if (dir == 2) {
            nk.x = (k.x + nx - 1) % nx;
        } else {
            nk.x = (k.x + 1) % nx;
        }
        const int mine = dir, theirs = dir ^ 1;
        std::vector<std::pair<QuadKey, const TerrainTile*>> found;
        for (int up = 0; up <= nk.level && found.empty(); ++up) {
            QuadKey anc{nk.level - up, nk.x >> up, nk.y >> up};
            auto it = drawn.find(anc);
            if (it != drawn.end()) {
                found.push_back({anc, it->second});
                coarser[dir] = up;
            }
        }
        if (found.empty()) {
            std::vector<QuadKey> frontier{nk};
            for (int depth = 0; depth < 6 && !frontier.empty(); ++depth) {
                std::vector<QuadKey> next;
                for (const QuadKey& f : frontier)
                    for (int c = 0; c < 2; ++c) {
                        QuadKey kid{f.level + 1, f.x * 2, f.y * 2};
                        if (dir == 0) kid = {f.level + 1, f.x * 2 + c, f.y * 2};
                        else if (dir == 1) kid = {f.level + 1, f.x * 2 + c, f.y * 2 + 1};
                        else if (dir == 2) kid = {f.level + 1, f.x * 2 + 1, f.y * 2 + c};
                        else kid = {f.level + 1, f.x * 2, f.y * 2 + c};
                        auto it = drawn.find(kid);
                        if (it != drawn.end()) found.push_back({kid, it->second});
                        else next.push_back(kid);
                    }
                frontier.swap(next);
            }
        }
        if (t.edges.size() != static_cast<size_t>(4) * kPosts) continue;
        for (const auto& [nkey, nt] : found) {
            if (nt->edges.size() != static_cast<size_t>(4) * kPosts) continue;
            const Bounds b = bounds(nkey);
            const bool alongLon = dir < 2;
            double lo = alongLon ? std::max(a.west, b.west) : std::max(a.south, b.south);
            double hi = alongLon ? std::min(a.east, b.east) : std::min(a.north, b.north);
            if (hi <= lo) continue;
            const double spanA = a.north - a.south, spanB = b.north - b.south;
            for (int s = 0; s <= kGrid; ++s) {
                double v = lo + (hi - lo) * s / kGrid;
                double sa = alongLon ? (v - a.west) / spanA : (v - a.south) / spanA;
                double sb = alongLon ? (v - b.west) / spanB : (v - b.south) / spanB;
                need = std::max(need, profile(t, mine, sa) - profile(*nt, theirs, sb));
            }
        }
    }
    return std::clamp(std::max(static_cast<double>(t.skirt), need * 1.05 + 1.0), 1.0, kSkirtMax);
}

int GlobeRenderer::idealImageryZ(const QuadKey& k) const {
    for (size_t l = 0; l < matrices_.size() && l < layers_.size(); ++l) {
        if (!shown(static_cast<int>(l)) || layers_[l].polarFill || (l < vectorLayers_.size() && vectorLayers_[l])) continue;
        const TileMatrix& m = matrices_[l];
        if (m.levels.empty()) return -1;
        double target = spanDeg(k.level) / kSurfaceSize * m.unitsPerDegree();
        return m.bestLevel(target);
    }
    return -1;
}

bool GlobeRenderer::wasRefined(const QuadKey& k) const {
    auto it = refined_.find(k);
    return it != refined_.end() && it->second + 2 >= frame_;
}

int GlobeRenderer::readyGap(const QuadKey& k) const {
    for (int up = 1; up <= k.level; ++up)
        if (presentable({k.level - up, k.x >> up, k.y >> up})) return up;
    return k.level + 1;
}

bool GlobeRenderer::roadsReady(const std::vector<TileId>& roads) const {
    for (const TileId& r : roads) {
        auto it = imagery_.find(r);
        if (it != imagery_.end() && it->second.vector && !it->second.vector->roads.empty() && it->second.roads3dVersion < 0) return false;
    }
    return true;
}

bool GlobeRenderer::finished(const QuadKey& k) const {
    auto s = surface_.find(k);
    if (s == surface_.end() || s->second.version < 0) return false;
    if (lines3d_ && labelsOn_ && !roadsReady(s->second.roadTiles)) return false;
    if (s->second.complete) return true;
    if (s->second.missing.empty()) return false;
    const double t = now();
    for (const TileId& id : s->second.missing) {
        auto f = failed_.find(id);
        if (f == failed_.end() || f->second <= t) return false;
    }
    return true;
}

int GlobeRenderer::surfaceZ(const QuadKey& q) const {
    auto st = q.level >= 0 ? surface_.find(q) : surface_.end();
    if (st == surface_.end()) return -1;
    return st->second.complete ? std::max(st->second.sourceZ, st->second.imageryZ) : st->second.sourceZ;
}

int GlobeRenderer::surfaceZMax(const QuadKey& q) const {
    auto st = q.level >= 0 ? surface_.find(q) : surface_.end();
    if (st == surface_.end()) return -1;
    return std::max(st->second.sourceZ, st->second.imageryZ);
}

bool GlobeRenderer::retrying(const QuadKey& k) const {
    auto s = surface_.find(k);
    if (s == surface_.end() || s->second.complete) return false;
    for (const TileId& id : s->second.missing) {
        auto f = failed_.find(id);
        if (f != failed_.end() && std::isfinite(f->second)) return true;
    }
    return false;
}

bool GlobeRenderer::presentable(const QuadKey& k) const {
    auto s = surface_.find(k);
    if (s == surface_.end() || !(s->second.complete || (s->second.version >= 0 && s->second.presentable)))
        return false;
    auto t = terrain_.find(k);
    return t != terrain_.end() && t->second.complete;
}

bool GlobeRenderer::ready(const QuadKey& k) const {
    auto s = surface_.find(k);
    if (s == surface_.end() || !s->second.complete) return false;
    auto t = terrain_.find(k);
    return t != terrain_.end() && t->second.complete;
}

bool GlobeRenderer::progressed(const SurfaceTile& s) const {
    if (s.missing.empty()) return true;
    double t = now();
    for (const TileId& id : s.missing) {
        if (imagery_.count(id)) return true;
        auto f = failed_.find(id);
        if (f != failed_.end() && (f->second == std::numeric_limits<double>::infinity() || f->second <= t)) return true;
    }
    return false;
}

void GlobeRenderer::need(const QuadKey& k, double priority) {
    if (retaining_ > 0) return;
    if (prefetching_) {
        prefetch_.push_back({k, priority * needScale_, true});
        return;
    }
    int depth = 0;
    auto s = surface_.find(k);
    if (s == surface_.end() || !s->second.complete) {
        depth = 1;
        for (int up = 1; up <= 8 && k.level - up >= 0; ++up) {
            auto a = surface_.find({k.level - up, k.x >> up, k.y >> up});
            if (a != surface_.end() && a->second.version >= 0) break;
            ++depth;
        }
    }
    needs_.push_back({k, priority / (1.0 + depth * depth)});
}

void GlobeRenderer::traverse(const Camera& cam, const QuadKey& k, std::vector<DrawTile>& out) {
    ++visits_;
    NodeInfo ni = info(cam, k);
    if (!ni.visible) return;
    QuadKey kids[4];
    for (int i = 0; i < 4; ++i) kids[i] = {k.level + 1, k.x * 2 + (i & 1), k.y * 2 + (i >> 1)};
    if (ni.refine) {
        bool allReady = true;
        bool anyReady = false;
        bool allDrawable = true;
        NodeInfo ci[4];
        for (int i = 0; i < 4; ++i) {
            ci[i] = info(cam, kids[i]);
            if (!ci[i].visible) continue;
            need(kids[i], ci[i].distance * 2.0);
            if (presentable(kids[i])) anyReady = true;
            else allReady = false;
            if (terrain_.find(kids[i]) == terrain_.end()) allDrawable = false;
        }
        if (k.level < kMinLevel) {
            for (int i = 0; i < 4; ++i) {
                if (ci[i].visible) terrain(kids[i]);
                traverse(cam, kids[i], out);
            }
            return;
        }
        bool sticky = !prefetching_ && wasRefined(k);
        if (!allDrawable && sticky) {
            allDrawable = true;
            for (int i = 0; i < 4; ++i) {
                if (!ci[i].visible || terrain_.find(kids[i]) != terrain_.end()) continue;
                if (syncBuilds_ >= 24) {
                    allDrawable = false;
                    break;
                }
                ++syncBuilds_;
                terrain(kids[i]).used = frame_;
            }
        }
        if (allDrawable && (allReady || sticky || (!presentable(k) && (anyReady || maxDive_ <= 0 || readyGap(k) < maxDive_)))) {
            if (!prefetching_) refined_[k] = frame_;
            for (int i = 0; i < 4; ++i) traverse(cam, kids[i], out);
            return;
        }
        if (!prefetching_)
            for (int i = 0; i < 4; ++i) {
                if (!ci[i].visible || !ci[i].refine || presentable(kids[i])) continue;
                for (int j = 0; j < 4; ++j) {
                    QuadKey g{kids[i].level + 1, kids[i].x * 2 + (j & 1), kids[i].y * 2 + (j >> 1)};
                    NodeInfo gi = info(cam, g);
                    if (gi.visible) prefetch(g, gi.distance * 2.5);
                }
            }
    } else if ((!presentable(k) || (wasRefined(k) && !finished(k))) && k.level < maxRefine()) {
        if (!prefetching_ && wasRefined(k)) {
            bool drawable = true;
            NodeInfo kin[4];
            for (int i = 0; i < 4 && drawable; ++i) {
                kin[i] = info(cam, kids[i]);
                if (kin[i].visible && terrain_.find(kids[i]) == terrain_.end()) drawable = false;
            }
            if (drawable) {
                need(k, ni.distance * 0.5);
                refined_[k] = frame_;
                ++retaining_;
                for (int i = 0; i < 4; ++i) {
                    if (!kin[i].visible) continue;
                    auto d = drawn_.find(kids[i]);
                    bool shown = d != drawn_.end() && d->second + 2 >= frame_;
                    if (shown || wasRefined(kids[i])) traverse(cam, kids[i], out);
                    else out.push_back({kids[i], kin[i].distance, 1});
                }
                --retaining_;
                return;
            }
        }
        bool kidsReady = true;
        bool any = false;
        NodeInfo ci[4];
        for (int i = 0; i < 4 && kidsReady; ++i) {
            ci[i] = info(cam, kids[i]);
            if (!ci[i].visible) continue;
            any = true;
            if (!presentable(kids[i])) kidsReady = false;
        }
        if (any && kidsReady) {
            need(k, ni.distance);
            for (int i = 0; i < 4; ++i)
                if (ci[i].visible) out.push_back({kids[i], ci[i].distance});
            return;
        }
    }
    if (ni.refine && !prefetching_ && retaining_ == 0) ++refineWaiting_;
    if (!ni.refine && !prefetching_ && retaining_ == 0 && mergeDepth_ == 0 && mergeSeconds_ > 0.0 && k.level >= kMinLevel &&
        wasRefined(k) && presentable(k)) {
        Merge& m = merges_[k];
        const double t = now();
        const bool moving = t - lastMotion_ < 0.15;
        const double tick = mergeGap_ > 0.0 && moving ? std::ceil(t / mergeGap_) * mergeGap_ : t;
        if (m.frame + 1 != frame_) m.start = tick;
        else if (!moving && m.start > t) m.start = t;
        m.frame = frame_;
        for (int i = 0; i < 4; ++i) {
            auto kt = transitions_.find(kids[i]);
            if (kt != transitions_.end() && kt->second.frame + 1 >= frame_ && kt->second.start >= 0.0 && t - kt->second.start < kFadeSeconds)
                m.start = tick;
        }
        bool capped = false;
        if (uniformMix_ >= 0.5 && uniformZ_ < 99 && !settleFrozen_) {
            const int zk = surfaceZ(k);
            if (zk >= 0 && uniformZ_ > zk && uniformZ_ - zk <= kMaxUniformDrop) {
                pendingMergeZ_ = std::min(pendingMergeZ_, zk);
                m.start = t;
            }
            if (surfaceZMax(k) > uniformZ_) {
                m.start = t;
                capped = true;
            }
        }
        const double p = settleFrozen_ || (stepInstant_ && !capped) ? 1.0 : std::max(0.0, (t - m.start) / (mergeSeconds_ / std::max(1.0, zoomRate_)));
        if (p < 1.0) {
            mergeActive_ = true;
            bool drawable = true;
            NodeInfo kin[4];
            for (int i = 0; i < 4 && drawable; ++i) {
                kin[i] = info(cam, kids[i]);
                if (kin[i].visible && terrain_.find(kids[i]) == terrain_.end()) drawable = false;
            }
            if (drawable) {
                refined_[k] = frame_;
                need(k, ni.distance);
                const size_t first = out.size();
                ++mergeDepth_;
                for (int i = 0; i < 4; ++i)
                    if (kin[i].visible) traverse(cam, kids[i], out);
                --mergeDepth_;
                for (size_t i = first; i < out.size(); ++i) {
                    out[i].merge = k;
                    out[i].mergeT = static_cast<float>(p);
                }
                return;
            }
        }
    }
    need(k, ni.distance);
    if (k.level > kMinLevel) {
        QuadKey parent{k.level - 1, k.x / 2, k.y / 2};
        prefetch(parent, ni.distance * 3.0);
        if (k.level > kMinLevel + 1) prefetch({k.level - 2, k.x / 4, k.y / 4}, ni.distance * 6.0);
    }
    if (ni.nearRefine && presentable(k))
        for (int i = 0; i < 4; ++i) prefetch(kids[i], ni.distance * 4.0);
    out.push_back({k, ni.distance, retaining_ > 0 ? 2 : 0});
}

int GlobeRenderer::processLoads() {
    auto taken = loader_.take(kUploadsPerFrame);
    int uploaded = 0;
    size_t keep = taken.size();
    const double uploadBudget = taken.size() >= kUploadsPerFrame / 2 ? budgetMs * 0.75 : budgetMs * 0.3;
    for (size_t i = 0; i < taken.size(); ++i) {
        if (uploaded >= 4 && elapsedMs() > uploadBudget) {
            keep = i;
            break;
        }
        ++uploaded;
        auto& t = taken[i];
        if (t.ok && t.vector) {
            ImageryTexture tex;
            tex.vector = t.vector;
            tex.used = frame_;
            if (!t.vector->fillVertices.empty()) {
                sg_buffer_desc d{};
                d.usage.vertex_buffer = true;
                d.data = {t.vector->fillVertices.data(), t.vector->fillVertices.size() * sizeof(float)};
                tex.fill = sg_make_buffer(&d);
                tex.fillCount = static_cast<int>(t.vector->fillVertices.size() / 6);
            }
            if (!t.vector->lineVertices.empty()) {
                sg_buffer_desc d{};
                d.usage.vertex_buffer = true;
                d.data = {t.vector->lineVertices.data(), t.vector->lineVertices.size() * sizeof(float)};
                tex.line = sg_make_buffer(&d);
                tex.lineCount = static_cast<int>(t.vector->lineVertices.size() / 10);
            }
            if (!t.vector->roads.empty()) {
                if (!lines3d_) {
                    sg_buffer_desc d{};
                    d.usage.vertex_buffer = true;
                    d.data = {t.vector->roads.data(), t.vector->roads.size() * sizeof(float)};
                    tex.roads = sg_make_buffer(&d);
                }
                tex.roadCount = static_cast<int>(t.vector->roads.size() / 6);
                tex.solidRoads = static_cast<int>(t.vector->solidRoads);
            }
            t.vector->fillVertices.clear();
            t.vector->fillVertices.shrink_to_fit();
            t.vector->lineVertices.clear();
            t.vector->lineVertices.shrink_to_fit();
            if (!lines3d_) {
                t.vector->roads.clear();
                t.vector->roads.shrink_to_fit();
            }
            auto old = imagery_.find(t.id);
            if (old != imagery_.end()) dropImagery(old->second);
            imagery_[t.id] = tex;
            failed_.erase(t.id);
            failCount_.erase(t.id);
        } else if (t.ok) {
            if (t.id.layer >= 0 && t.id.layer < static_cast<int>(layerOpacity_.size()) && !t.rgba.empty()) {
                size_t samples = 0, opaque = 0;
                for (size_t a = 3; a < t.rgba.size(); a += 4 * 61) {
                    ++samples;
                    if (t.rgba[a] >= 128) ++opaque;
                }
                const float f = samples ? static_cast<float>(opaque) / static_cast<float>(samples) : 1.0f;
                float& o = layerOpacity_[t.id.layer];
                o = o < 0.0f ? f : o * 0.8f + f * 0.2f;
            }
            sg_image_desc d{};
            d.width = t.width;
            d.height = t.height;
            d.pixel_format = SG_PIXELFORMAT_RGBA8;
            d.data.mip_levels[0] = {t.rgba.data(), t.rgba.size()};
            ImageryTexture tex;
            tex.image = sg_make_image(&d);
            sg_view_desc v{};
            v.texture.image = tex.image;
            tex.view = sg_make_view(&v);
            tex.used = frame_;
            if (sg_query_view_state(tex.view) != SG_RESOURCESTATE_VALID) {
                poolPressure("imagery texture");
                sg_destroy_view(tex.view);
                sg_destroy_image(tex.image);
                failed_[t.id] = now() + 0.5;
                continue;
            }
            auto old = imagery_.find(t.id);
            if (old != imagery_.end()) dropImagery(old->second);
            imagery_[t.id] = tex;
            failed_.erase(t.id);
            failCount_.erase(t.id);
        } else {
            if (failCount_.size() > 50000) failCount_.clear();
            int attempts = ++failCount_[t.id];
            double backoff = std::min(kRetrySeconds, 0.5 * static_cast<double>(1 << std::min(attempts - 1, 6)));
            failed_[t.id] = t.permanent ? std::numeric_limits<double>::infinity() : now() + backoff;
            auto old = imagery_.find(t.id);
            if (old != imagery_.end() && t.permanent) {
                dropImagery(old->second);
                imagery_.erase(old);
                if (t.id.layer < static_cast<int>(matrices_.size())) {
                    const TileMatrix& m = matrices_[t.id.layer];
                    double x0, y0, x1, y1;
                    m.tileBounds(t.id.z, t.id.x, t.id.y, x0, y0, x1, y1);
                    markDirty({unprojectX(m.srid, x0), unprojectY(m.srid, y0), unprojectX(m.srid, x1), unprojectY(m.srid, y1)});
                }
            }
        }
        ++imageryVersion_;
    }
    for (size_t i = taken.size(); i-- > keep;) loader_.giveBack(taken[i]);
    stats_.uploads = uploaded;
    stats_.imageryLoads += uploaded;
    return uploaded;
}

void GlobeRenderer::evict() {
    const size_t imageryCap = imageryLimit();
    if (imagery_.size() > imageryCap) {
        std::vector<std::pair<std::uint64_t, TileId>> c;
        for (auto& [k, t] : imagery_)
            if (k.z > kBaseZoom && t.used + 1 < frame_) c.push_back({t.used, k});
        std::sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < c.size() && imagery_.size() > imageryCap; ++i) {
            auto it = imagery_.find(c[i].second);
            if (!it->second.drawn) ++stats_.imageryWasted;
            dropImagery(it->second);
            imagery_.erase(it);
            ++stats_.imageryEvictions;
        }
    }
    if (surface_.size() > surfaceCapacity_) {
        std::vector<std::pair<std::uint64_t, QuadKey>> c;
        for (auto& [k, s] : surface_)
            if (k.level > kKeepLevel && s.used + 1 < frame_) c.push_back({s.used, k});
        std::sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < c.size() && surface_.size() > surfaceCapacity_; ++i) {
            auto it = surface_.find(c[i].second);
            if (freeSurfaces_.size() < freeSurfacePool_) freeSurfaces_.push_back(it->second);
            else dropSurface(it->second);
            ++stats_.surfaceEvictions;
            surface_.erase(it);
        }
    }
    if (terrain_.size() > terrainCapacity_) {
        std::vector<std::pair<std::uint64_t, QuadKey>> c;
        for (auto& [k, t] : terrain_)
            if (k.level > kKeepLevel && t.used + 1 < frame_) c.push_back({t.used, k});
        std::sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < c.size() && terrain_.size() > terrainCapacity_; ++i) {
            auto it = terrain_.find(c[i].second);
            sg_destroy_buffer(it->second.vbuf);
            terrain_.erase(it);
            ++stats_.terrainEvictions;
        }
    }
}

void GlobeRenderer::want(const TileId& id, double priority) { requests_.emplace_back(id, priority); }

void GlobeRenderer::requestBase() {
    double t = now();
    for (int l = 0; l < static_cast<int>(matrices_.size()); ++l) {
        if (l < static_cast<int>(layers_.size()) && !shown(l)) continue;
        if (l != baseLayer_ && !(l < static_cast<int>(layers_.size()) && layers_[l].polarFill)) continue;
        const TileMatrix& m = matrices_[l];
        const int top = std::min(kBaseZoom, static_cast<int>(m.levels.size()) - 1);
        for (int z = 0; z <= top; ++z) {
            if (!m.hasLevel(z)) continue;
            const int w = m.levels[z].matrixWidth, h = m.levels[z].matrixHeight;
            if (w * h > 64) break;
            bool fill = l < static_cast<int>(layers_.size()) && layers_[l].polarFill;
            for (int i = 0; i < w * h; ++i) {
                if (fill && i / w != 0 && i / w != h - 1) continue;
                TileId id{l, z, i % w, i / w};
                if (imagery_.find(id) != imagery_.end()) continue;
                auto f = failed_.find(id);
                if (f != failed_.end() && f->second > t) continue;
                want(id, -1.0 + z * 0.01);
            }
        }
    }
}

GlobeRenderer::ImageryTexture* GlobeRenderer::findTexture(const TileId& id, const TileMatrix& m, double priority,
                                                          bool& missing, TileId& src) {
    missing = false;
    src = id;
    auto it = imagery_.find(id);
    if (it != imagery_.end()) return &it->second;
    const double t = now();
    const double cx = m.originX + (id.x + 0.5) * m.spanX(id.z);
    const double cy = m.originY - (id.y + 0.5) * m.spanY(id.z);
    auto ancestor = [&](int z) {
        if (z == id.z) return id;
        return TileId{id.layer, z, static_cast<int>(std::floor((cx - m.originX) / m.spanX(z))),
                      static_cast<int>(std::floor((m.originY - cy) / m.spanY(z)))};
    };
    auto unavailable = [&](const TileId& k) {
        auto f = failed_.find(k);
        return f != failed_.end() && f->second == std::numeric_limits<double>::infinity();
    };
    int target = -1;
    for (int z = id.z; z >= 0; --z) {
        if (z != id.z && !m.hasLevel(z)) continue;
        TileId k = ancestor(z);
        if (imagery_.count(k) || !unavailable(k)) {
            target = z;
            break;
        }
    }
    if (target >= 0) {
        TileId k = ancestor(target);
        auto ti = imagery_.find(k);
        if (ti != imagery_.end()) {
            src = k;
            return &ti->second;
        }
        missing = true;
        auto f = failed_.find(k);
        if (f == failed_.end() || f->second <= t) want(k, priority);
    }
    TileId coarse{-1, 0, 0, 0};
    for (int z = (target >= 0 ? target : id.z) - 1; z >= 0; --z) {
        if (!m.hasLevel(z)) continue;
        TileId a = ancestor(z);
        auto ai = imagery_.find(a);
        if (ai != imagery_.end()) {
            if (missing && coarse.layer >= 0) want(coarse, priority * 0.5);
            src = a;
            return &ai->second;
        }
        if (z == target - 3) {
            auto fa = failed_.find(a);
            if (fa == failed_.end() || fa->second <= t) coarse = a;
        }
    }
    if (missing && coarse.layer >= 0) want(coarse, priority * 0.5);
    return nullptr;
}

void GlobeRenderer::dropImagery(ImageryTexture& t) {
    if (t.image.id) {
        sg_destroy_view(t.view);
        sg_destroy_image(t.image);
    }
    if (t.fill.id) sg_destroy_buffer(t.fill);
    if (t.line.id) sg_destroy_buffer(t.line);
    if (t.roads.id) sg_destroy_buffer(t.roads);
    if (t.roads3d.id) sg_destroy_buffer(t.roads3d);
    t = ImageryTexture{};
}

bool GlobeRenderer::sparseLayer(int l) const {
    if (l < 0 || l >= static_cast<int>(layers_.size())) return false;
    if (l < static_cast<int>(vectorLayers_.size()) && vectorLayers_[l]) return true;
    return l < static_cast<int>(layerOpacity_.size()) && layerOpacity_[l] >= 0.0f && layerOpacity_[l] < 0.6f;
}

bool GlobeRenderer::standIn(const Camera& cam, const QuadKey& k) const {
    if (k.level + 2 > maxRefine()) return false;
    return info(cam, k).ratio > 3.0;
}

bool GlobeRenderer::buildSurfaceJob(const QuadKey& k, double priority, SurfaceJob* job, std::vector<float>* verts,
                                    std::vector<float>* fverts, double texelDeg, bool standIn) {
    Bounds b = bounds(k);
    double span = b.north - b.south;
    bool complete = true;
    double coverLat = 0;
    bool anyCover = false;
    bool unavailable = false;
    for (int l = 0; l < static_cast<int>(matrices_.size()) && l < static_cast<int>(layers_.size()); ++l)
        if (!layers_[l].polarFill && shown(l)) {
            coverLat = std::max(coverLat, maxLatitude(matrices_[l].srid));
            anyCover = true;
        }
    for (int l = 0; l < static_cast<int>(matrices_.size()) && l < static_cast<int>(layers_.size()); ++l) {
      if (!shown(l)) continue;
      if (standIn && l != baseLayer_ && !layers_[l].polarFill && sparseLayer(l)) {
          if (job) job->overlaysOmitted = true;
          continue;
      }
      const TileMatrix& m = matrices_[l];
      double maxLat = maxLatitude(m.srid);
      double bandS[2], bandN[2];
      int bands = 0;
      if (layers_[l].polarFill && anyCover) {
          if (coverLat >= maxLat) continue;
          if (b.north > coverLat) {
              bandS[bands] = std::max(b.south, coverLat);
              bandN[bands++] = std::min(b.north, maxLat);
          }
          if (b.south < -coverLat) {
              bandS[bands] = std::max(b.south, -maxLat);
              bandN[bands++] = std::min(b.north, -coverLat);
          }
      } else {
          bandS[bands] = std::max(b.south, -maxLat);
          bandN[bands++] = std::min(b.north, maxLat);
      }
      for (int band = 0; band < bands; ++band) {
        double south = bandS[band], north = bandN[band];
        if (south >= north) continue;
        double target = span / kSurfaceSize * m.unitsPerDegree();
        int z = m.bestLevel(target);
        if (z < 0 || m.levels[z].resolutionX < target * 0.125) continue;
        std::vector<TileRange> ranges = regionRanges(m, {b.west, south, b.east, north}, z, z);
        if (ranges.empty()) continue;
        const TileRange& r = ranges[0];
        int rows = (m.srid == 3857 && z < 9) ? 8 : 1;
        for (int ty = r.y0; ty <= r.y1; ++ty)
            for (int tx = r.x0; tx <= r.x1; ++tx) {
                TileId id{l, z, tx, ty};
                bool missing = false;
                TileId src;
                ImageryTexture* tex = findTexture(id, m, priority, missing, src);
                if (missing) {
                    complete = false;
                    if (job) job->missing.push_back(id);
                    const bool backingOff = failed_.count(id) > 0;
                    if (job && l != overlayLayer_ && ((l == baseLayer_ || layers_[l].polarFill) ? !backingOff : (!tex || src.z < id.z - 1)))
                        job->presentable = false;
                }
                if (!tex) {
                    if (!missing && l == baseLayer_) unavailable = true;
                    else if (job && l == baseLayer_ && !layers_[l].polarFill)
                        ++job->hollow;
                    continue;
                }
                if (missing && l != baseLayer_ && l != overlayLayer_ && !layers_[l].polarFill && src.z < id.z - 1 && sparseLayer(l)) continue;
                if (job && !missing && src.z < id.z) job->degraded = true;
                tex->used = frame_;
                if (!job) continue;
                tex->drawn = true;
                if (l == baseLayer_) {
                    job->imageryZ = z;
                    job->sourceZ = std::min(job->sourceZ, src.z);
                }
                if (tex->vector) {
                    if (l == baseLayer_) job->background = true;
                    VectorDraw vd;
                    vd.tile = tex;
                    vd.id = src;
                    vd.xform[0] = static_cast<float>(2.0 / span);
                    vd.xform[1] = static_cast<float>(2.0 / span);
                    vd.xform[2] = static_cast<float>((tex->vector->originLon - b.west) * 2.0 / span - 1.0);
                    vd.xform[3] = static_cast<float>((tex->vector->originLat - b.south) * 2.0 / span - 1.0);
                    bool seen = false;
                    for (const auto& e : job->vectors) seen = seen || e.tile == tex;
                    if (!seen) job->vectors.push_back(vd);
                    continue;
                }

                double sMinX, sMinY, sMaxX, sMaxY, tMinX, tMinY, tMaxX, tMaxY;
                m.tileBounds(src.z, src.x, src.y, sMinX, sMinY, sMaxX, sMaxY);
                m.tileBounds(z, tx, ty, tMinX, tMinY, tMaxX, tMaxY);
                double w = std::max(b.west, unprojectX(m.srid, tMinX));
                double e = std::min(b.east, unprojectX(m.srid, tMaxX));
                double s = std::max(south, unprojectY(m.srid, tMinY));
                double nn = std::min(north, unprojectY(m.srid, tMaxY));
                if (w >= e || s >= nn) continue;

                int first = static_cast<int>(verts->size() / 4);
                auto emit = [&](double lon, double lat) {
                    float px = static_cast<float>((lon - b.west) / span * 2.0 - 1.0);
                    float py = static_cast<float>((lat - b.south) / span * 2.0 - 1.0);
                    float u = static_cast<float>((projectX(m.srid, lon) - sMinX) / (sMaxX - sMinX));
                    float v = static_cast<float>((sMaxY - projectY(m.srid, lat)) / (sMaxY - sMinY));
                    verts->insert(verts->end(), {px, py, u, v});
                };
                for (int rr = 0; rr < rows; ++rr) {
                    double la0 = s + (nn - s) * rr / rows, la1 = s + (nn - s) * (rr + 1) / rows;
                    emit(w, la0);
                    emit(e, la0);
                    emit(e, la1);
                    emit(w, la0);
                    emit(e, la1);
                    emit(w, la1);
                }
                SurfaceDraw sd{tex->view, layers_[l].opacity, first, rows * 6};
                if (layers_[l].polarFill) {
                    const float* t = north > 0 ? layers_[l].polarTintNorth : layers_[l].polarTintSouth;
                    for (int c = 0; c < 3; ++c) sd.tint[c] = t[c];
                }
                job->draws.push_back(sd);
            }
      }
    }
    if (job) {
        for (const auto& [id, o] : overlays_) {
            const GeoRect& r = o.rect;
            double w = std::max(b.west, r.west), e = std::min(b.east, r.east);
            double s = std::max(b.south, r.south), nn = std::min(b.north, r.north);
            if (w >= e || s >= nn) continue;
            int first = static_cast<int>(verts->size() / 4);
            auto emit = [&](double lon, double lat) {
                float px = static_cast<float>((lon - b.west) / span * 2.0 - 1.0);
                float py = static_cast<float>((lat - b.south) / span * 2.0 - 1.0);
                float u = static_cast<float>((lon - r.west) / (r.east - r.west));
                float v = static_cast<float>((r.north - lat) / (r.north - r.south));
                verts->insert(verts->end(), {px, py, u, v});
            };
            emit(w, s);
            emit(e, s);
            emit(e, nn);
            emit(w, s);
            emit(e, nn);
            emit(w, nn);
            job->draws.push_back({o.view, o.opacity, first, 6});
        }
        if (unavailable && backdropEnabled_ && backdrop().id) {
            int first = static_cast<int>(verts->size() / 4);
            auto emit = [&](double lon, double lat) {
                float px = static_cast<float>((lon - b.west) / span * 2.0 - 1.0);
                float py = static_cast<float>((lat - b.south) / span * 2.0 - 1.0);
                verts->insert(verts->end(), {px, py, static_cast<float>((lon + 180.0) / 360.0), static_cast<float>((90.0 - lat) / 180.0)});
            };
            emit(b.west, b.south);
            emit(b.east, b.south);
            emit(b.east, b.north);
            emit(b.west, b.south);
            emit(b.east, b.north);
            emit(b.west, b.north);
            job->draws.insert(job->draws.begin(), SurfaceDraw{backdropView_, 1.0f, first, 6});
            job->degraded = true;
        }
        job->complete = complete;
    }
    return complete;
}

void GlobeRenderer::generateMips(const std::vector<sg_image>& images) {
#if defined(OTK_GL_MIPS)
    for (const sg_image& img : images) {
        sg_gl_image_info info = sg_gl_query_image_info(img);
        GLuint tex = info.tex[info.active_slot];
        if (!tex) continue;
        glBindTexture(GL_TEXTURE_2D, tex);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    sg_reset_state_cache();
#else
    (void)images;
#endif
}

double GlobeRenderer::videoMemoryMb() {
#if defined(_WIN32) && defined(SOKOL_D3D11)
    auto* dev = static_cast<ID3D11Device*>(const_cast<void*>(sg_d3d11_device()));
    if (!dev) return 0;
    IDXGIDevice* dxgi = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) return 0;
    double mb = 0;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(dxgi->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc)) && desc.DedicatedVideoMemory > 64ull * 1048576ull)
            mb = static_cast<double>(desc.DedicatedVideoMemory) / 1048576.0;
        adapter->Release();
    }
    dxgi->Release();
    return mb;
#elif defined(OTK_GL_MIPS) && !defined(SOKOL_GLES3)
    GLint kb = 0;
    while (glGetError() != GL_NO_ERROR) {
    }
    glGetIntegerv(0x9047, &kb);
    if (glGetError() != GL_NO_ERROR || kb <= 0) {
        GLint info[4] = {0, 0, 0, 0};
        glGetIntegerv(0x87FC, info);
        if (glGetError() != GL_NO_ERROR) return 0;
        kb = info[0];
    }
    return kb > 0 ? kb / 1024.0 : 0;
#else
    return 0;
#endif
}

double GlobeRenderer::physicalMemoryMb() {
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) return static_cast<double>(m.ullTotalPhys) / 1048576.0;
    return 0;
#elif defined(__APPLE__)
    std::uint64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0) return static_cast<double>(bytes) / 1048576.0;
    return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? static_cast<double>(pages) * static_cast<double>(size) / 1048576.0 : 0;
#endif
}

void GlobeRenderer::diag(const char* fmt, ...) const {
    if (!diagnostics_) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#if defined(__ANDROID__)
    __android_log_write(ANDROID_LOG_INFO, "OTKGlobeEngine", buf);
#else
    if (const char* path = std::getenv("OTK_DIAG")) {
        if (FILE* f = std::fopen(path, "a")) {
            std::fputs(buf, f);
            std::fputc(10, f);
            std::fclose(f);
        }
    } else {
        std::fprintf(stderr, "%s\n", buf);
    }
#endif
}

void GlobeRenderer::explainCollapse(const Camera& cam, const std::vector<DrawTile>& tiles) {
    const DrawTile* coarse = nullptr;
    for (const DrawTile& t : tiles)
        if (!coarse || t.key.level < coarse->key.level) coarse = &t;
    if (!coarse) {
        diag("collapse frame %llu: no tiles drawn gsd %.2f", static_cast<unsigned long long>(frame_), cam.gsd);
        return;
    }
    auto state = [&](const QuadKey& k, char* out, size_t n) {
        auto s = surface_.find(k);
        auto t = terrain_.find(k);
        NodeInfo ni = info(cam, k);
        std::snprintf(out, n, "L%d/%d/%d vis%d ref%d surf%s terr%s", k.level, k.x, k.y, ni.visible ? 1 : 0, ni.refine ? 1 : 0,
                      s == surface_.end() ? "-" : s->second.complete ? "C" : s->second.version < 0 ? "0" : "p",
                      t == terrain_.end() ? "-" : t->second.complete ? "C" : "p");
    };
    char a[160], kid[4][160];
    state(coarse->key, a, sizeof(a));
    for (int i = 0; i < 4; ++i)
        state({coarse->key.level + 1, coarse->key.x * 2 + (i & 1), coarse->key.y * 2 + (i >> 1)}, kid[i], sizeof(kid[i]));
    diag("collapse frame %llu: drawn %zu maxL %d -> coarsest %s | kids %s ; %s ; %s ; %s | gsd %.2f prevGsd %.2f",
         static_cast<unsigned long long>(frame_), tiles.size(), prevMaxLevel_, a, kid[0], kid[1], kid[2], kid[3], cam.gsd, prevGsd_);
}

double GlobeRenderer::threadMs() {
#if defined(_WIN32)
    return 0.0;
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 + static_cast<double>(ts.tv_nsec) / 1.0e6;
#endif
}

void GlobeRenderer::profileMark(int section) {
    if (!profiling_) return;
    double t = elapsedMs();
    if (section > 0) profileAcc_[section - 1] += t - profileLast_;
    else profileAcc_[7] += t;
    profileLast_ = t;
    if (section == 6 && ++profileFrames_ >= 120) {
        if (FILE* f = std::fopen(profilePath_.c_str(), "a")) {
            const char* names[8] = {"traverse", "overview+admit", "terrain", "work", "surface", "main", "overview-only", "pre-traverse"};
            for (int i = 0; i < 8; ++i) std::fprintf(f, "%s %.3f ", names[i], profileAcc_[i] / profileFrames_);
            const char* subs[8] = {"main.globe", "main.labels", "main.overlay", "main.end", "pre.uploads", "pre.elevation", "pre.evict", "surface.mips"};
            for (int i = 0; i < 8; ++i) std::fprintf(f, "%s %.3f/%.3f ", subs[i], profileSub_[i] / profileFrames_, profileSubCpu_[i] / profileFrames_);
            std::fputc(10, f);
            std::fclose(f);
        }
        for (double& a : profileAcc_) a = 0;
        for (double& a : profileSub_) a = 0;
        for (double& a : profileSubCpu_) a = 0;
        profileFrames_ = 0;
    }
}

double GlobeRenderer::screenFraction(const Camera& cam, const QuadKey& k, const TerrainTile& t) const {
    constexpr int kCells = 4;
    const Bounds b = bounds(k);
    const double h = 0.5 * (static_cast<double>(t.minH) + static_cast<double>(t.maxH));
    const Mat4& m = cam.viewProjRel();
    const Vec3 eye = cam.eye();
    std::array<std::array<double, 4>, (kCells + 1) * (kCells + 1)> clip{};
    for (int j = 0; j <= kCells; ++j)
        for (int i = 0; i <= kCells; ++i) {
            const Vec3 p = geodeticToEcef(b.south + (b.north - b.south) * j / kCells, b.west + (b.east - b.west) * i / kCells, h) - eye;
            auto& c = clip[static_cast<size_t>(j) * (kCells + 1) + i];
            for (int r = 0; r < 4; ++r) c[r] = m.at(r, 0) * p.x + m.at(r, 1) * p.y + m.at(r, 2) * p.z + m.at(r, 3);
        }
    double total = 0;
    std::vector<std::array<double, 4>> poly, next;
    for (int j = 0; j < kCells; ++j)
        for (int i = 0; i < kCells; ++i) {
            poly = {clip[static_cast<size_t>(j) * (kCells + 1) + i], clip[static_cast<size_t>(j) * (kCells + 1) + i + 1],
                    clip[static_cast<size_t>(j + 1) * (kCells + 1) + i + 1], clip[static_cast<size_t>(j + 1) * (kCells + 1) + i]};
            for (int plane = 0; plane < 5 && poly.size() >= 3; ++plane) {
                auto dist = [plane](const std::array<double, 4>& v) {
                    switch (plane) {
                    case 0: return v[3] - 1e-6;
                    case 1: return v[3] - v[0];
                    case 2: return v[3] + v[0];
                    case 3: return v[3] - v[1];
                    default: return v[3] + v[1];
                    }
                };
                next.clear();
                for (size_t a = 0; a < poly.size(); ++a) {
                    const auto& p0 = poly[a];
                    const auto& p1 = poly[(a + 1) % poly.size()];
                    const double d0 = dist(p0), d1 = dist(p1);
                    if (d0 >= 0) next.push_back(p0);
                    if ((d0 >= 0) != (d1 >= 0)) {
                        const double s = d0 / (d0 - d1);
                        std::array<double, 4> q{};
                        for (int r = 0; r < 4; ++r) q[r] = p0[r] + (p1[r] - p0[r]) * s;
                        next.push_back(q);
                    }
                }
                poly.swap(next);
            }
            if (poly.size() < 3) continue;
            double area = 0;
            for (size_t a = 0; a < poly.size(); ++a) {
                const auto& p0 = poly[a];
                const auto& p1 = poly[(a + 1) % poly.size()];
                area += (p0[0] / p0[3]) * (p1[1] / p1[3]) - (p1[0] / p1[3]) * (p0[1] / p0[3]);
            }
            total += std::abs(area) * 0.125;
        }
    return total;
}

void GlobeRenderer::render(Camera& cam, const sg_swapchain* swapchain, const sg_attachments* attachments, int width,
                           int height, const std::vector<RenderView>* views) {
    if (!ready_) return;
    if (!renderingIdle_) idleValid_ = false;
    lastFrameStart_ = frameStart_;
    frameStart_ = std::chrono::steady_clock::now();
    profileStart_ = frameStart_;
    profileLast_ = 0;
    if (!trace_)
        if (const char* path = std::getenv("OTK_TRACE")) trace_ = std::fopen(path, "w");
    ++frame_;
    labels_.beginFrame();
    settleFrozen_ = snapshotShown_ && settleSnapshot_ && snapshotFade_ < 0.0;
    stepInstant_ = snapshotShown_ && (stepSnapshot_ || (settleSnapshot_ && snapshotFade_ >= 0.0));
    bool settleCapture = false;
    if ((settleWanted_ || stepWanted_ || switchWanted_) && !views && !snapshotShown_) {
        settleCapture = ensureSnapshotTarget(width, height);
        if (!settleCapture) settleBroken_ = true;
    }
    const bool switchCapture = settleCapture && switchWanted_;
    const bool stepCapture = settleCapture && !settleWanted_ && !switchWanted_;
    const bool capture = settleCapture;
    settleWanted_ = false;
    stepWanted_ = false;
    switchWanted_ = false;
    if (std::uint64_t epoch = net::reachabilityEpoch(); epoch != netEpoch_) {
        netEpoch_ = epoch;
        retryUnavailable();
    }
    requests_.clear();
    needs_.clear();
    prefetch_.clear();
    for (const auto& [bk, bp] : blendParents_) need(bk, bp);
    blendParents_.clear();
    elevation_.beginFrame();
    profileSub(-1);
    const int loadsTaken = processLoads();
    profileSub(4);
    bool elevationChanged = elevation_.processResults(32);
    profileSub(5);
    {
        std::vector<GeoRect> regions;
        if (features_.takeDirty(regions)) {
            for (auto& [k, s] : surface_) {
                Bounds b = bounds(k);
                GeoRect r{b.west, b.south, b.east, b.north};
                for (const GeoRect& g : regions)
                    if (g.intersects(r)) {
                        s.complete = false;
                        s.missing.clear();
                        break;
                    }
            }
        }
    }
    profileSub(-1);
    evict();
    profileSub(6);
    requestBase();

    cam.width = std::max(width, 1);
    cam.height = std::max(height, 1);
    sg_backend backend = sg_query_backend();
    cam.zeroToOneDepth = backend != SG_BACKEND_GLCORE && backend != SG_BACKEND_GLES3;
    cam.update();
    eye_ = cam.eye();
    if (views)
        for (const RenderView& v : *views) {
            v.cam->zeroToOneDepth = cam.zeroToOneDepth;
            v.cam->groundHeight = cam.groundHeight;
            v.cam->update();
        }
    if (anyGrid()) {
        bool changed = false;
        for (int g = 0; g < 3; ++g) {
            if (!gridOn_[g]) continue;
            double spacing = gridStep(cam, g);
            if (spacing != gridSpacing_[g]) {
                gridSpacing_[g] = spacing;
                changed = true;
            }
        }
        if (changed) {
            markDirty({-180, -90, 180, 90});
        }
    }

    const Mat4& m = cam.viewProjRel();
    for (int p = 0; p < 4; ++p) {
        int row = p / 2;
        double sign = (p % 2 == 0) ? 1.0 : -1.0;
        double a = m.at(3, 0) + sign * m.at(row, 0);
        double b = m.at(3, 1) + sign * m.at(row, 1);
        double c = m.at(3, 2) + sign * m.at(row, 2);
        double d = m.at(3, 3) + sign * m.at(row, 3);
        double len = std::sqrt(a * a + b * b + c * c);
        frustum_[p * 4 + 0] = a / len;
        frustum_[p * 4 + 1] = b / len;
        frustum_[p * 4 + 2] = c / len;
        frustum_[p * 4 + 3] = d / len;
    }

    std::vector<DrawTile> tiles;
    visits_ = 0;
    syncBuilds_ = 0;
    if (refined_.size() > 20000)
        for (auto it = refined_.begin(); it != refined_.end();) it = it->second + 3 < frame_ ? refined_.erase(it) : std::next(it);
    terrain({0, 0, 0});
    terrain({0, 1, 0});
    profileMark(0);
    mergeActive_ = false;
    refineWaiting_ = 0;
    setUniform(cam);
    pendingMergeZ_ = 99;
    traverse(cam, {0, 0, 0}, tiles);
    traverse(cam, {0, 1, 0}, tiles);
    profileMark(1);
    auto overview = [&](double gsdScale, double scale, double widen, const Camera* base = nullptr) {
        Camera wide = base ? *base : cam;
        if (gsdScale != 1.0) {
            int side = static_cast<int>(std::hypot(static_cast<double>(cam.width), static_cast<double>(cam.height)));
            wide.width = side;
            wide.height = side;
            wide.gsd = cam.gsd * gsdScale;
            wide.update();
        }
        std::array<double, 16> saved = frustum_;
        Vec3 savedEye = eye_;
        eye_ = wide.eye();
        const Mat4& wm = wide.viewProjRel();
        for (int p = 0; p < 4; ++p) {
            int row = p / 2;
            double sign = (p % 2 == 0) ? 1.0 : -1.0;
            double a = widen * wm.at(3, 0) + sign * wm.at(row, 0), b = widen * wm.at(3, 1) + sign * wm.at(row, 1);
            double c = widen * wm.at(3, 2) + sign * wm.at(row, 2), d = widen * wm.at(3, 3) + sign * wm.at(row, 3);
            double len = std::sqrt(a * a + b * b + c * c);
            frustum_[p * 4 + 0] = a / len;
            frustum_[p * 4 + 1] = b / len;
            frustum_[p * 4 + 2] = c / len;
            frustum_[p * 4 + 3] = d / len;
        }
        std::vector<DrawTile> ignored;
        needScale_ = scale;
        prefetching_ = true;
        int savedVisits = visits_;
        visits_ = 0;
        const double savedDist = uniformDist_, savedMix = uniformMix_;
        setUniform(wide);
        traverse(wide, {0, 0, 0}, ignored);
        traverse(wide, {0, 1, 0}, ignored);
        uniformDist_ = savedDist;
        uniformMix_ = savedMix;
        visits_ = savedVisits;
        needScale_ = 1.0;
        prefetching_ = false;
        frustum_ = saved;
        eye_ = savedEye;
    };
    {
        double camKey[6] = {cam.focus.lat, cam.focus.lon, cam.gsd, cam.azimuth, cam.tilt, static_cast<double>(cam.width * 100000 + cam.height)};
        bool moved = false;
        for (int i = 0; i < 6; ++i) moved = moved || camKey[i] != lastOverviewKey_[i];
        size_t own = prefetch_.size();
        double overviewStart = elapsedMs();
        double tnowZoom = now();
        if (zoomClock_ > 0 && tnowZoom > zoomClock_) {
            double rate = (std::log2(std::max(cam.gsd, 1e-6)) - std::log2(std::max(zoomGsd_, 1e-6))) / (tnowZoom - zoomClock_);
            double a = std::clamp((tnowZoom - zoomClock_) / 0.25, 0.0, 1.0);
            zoomRate_ = zoomRate_ + (rate - zoomRate_) * a;
        }
        zoomClock_ = tnowZoom;
        zoomGsd_ = cam.gsd;
        bool zoomingOut = zoomRate_ > 0.3;
        urgentPrefetch_ = zoomingOut;
        overviewReused_ = !(moved || frame_ % 8 == 0 || overviewCache_.empty() || now() - switchAt_ < 3.0);
        if (!overviewReused_) {
            double margin = marginPrefetch_ > 0 ? marginPrefetch_ : (budgetMb_ >= 512 ? 2.0 : 1.5);
            if (!zoomingOut && margin > 1.0) overview(1.0, 2.0, margin);
            overview(2.0, zoomingOut ? 1.2 : 3.0, 1.0);
            overview(4.0, zoomingOut ? 2.0 : 6.0, 1.0);
            if (hasLookahead_) overview(1.0, lookaheadPriority_, 1.2, &lookahead_);
            if (zoomingOut) overview(8.0, zoomRate_ > 1.5 ? 3.0 : 4.0, 1.0);
            if (now() - switchAt_ < 3.0) {
                overview(4.0, 1.5, 2.0);
                overview(8.0, 1.5, 3.0);
            }

            overviewCache_.assign(prefetch_.begin() + static_cast<std::ptrdiff_t>(own), prefetch_.end());
            for (int i = 0; i < 6; ++i) lastOverviewKey_[i] = camKey[i];
        } else {
            prefetch_.insert(prefetch_.end(), overviewCache_.begin(), overviewCache_.end());
        }
        if (profiling_) profileAcc_[6] += elapsedMs() - overviewStart;
    }
    std::sort(tiles.begin(), tiles.end(), [](const DrawTile& a, const DrawTile& b) { return a.distance < b.distance; });

    int baseSurfaceLevel = 3;
    while (baseSurfaceLevel > 0 && static_cast<size_t>(2 * ((1 << (2 * (baseSurfaceLevel + 1))) - 1) / 3) > surfaceCapacity_ / 4) --baseSurfaceLevel;
    const double baseSurfaces = static_cast<double>(2 * ((1 << (2 * (baseSurfaceLevel + 1))) - 1) / 3);
    if (overviewReused_ && !admittedCache_.empty()) {
        needs_.insert(needs_.end(), admittedCache_.begin(), admittedCache_.end());
        stats_.prefetched = static_cast<int>(admittedCache_.size());
    } else {
        size_t firstAdmitted = needs_.size();
        auto keyLess = [](const QuadKey& a, const QuadKey& b) {
            if (a.level != b.level) return a.level < b.level;
            if (a.x != b.x) return a.x < b.x;
            return a.y < b.y;
        };
        std::vector<QuadKey> seen;
        seen.reserve(needs_.size());
        for (const Need& n : needs_) seen.push_back(n.key);
        std::sort(seen.begin(), seen.end(), keyLess);
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        std::sort(prefetch_.begin(), prefetch_.end(), [&](const Need& a, const Need& b) {
            if (!(a.key == b.key)) return keyLess(a.key, b.key);
            return a.priority < b.priority;
        });
        prefetch_.erase(std::unique(prefetch_.begin(), prefetch_.end(), [](const Need& a, const Need& b) { return a.key == b.key; }),
                        prefetch_.end());
        size_t activeImagery = 0;
        for (const auto& [id, t] : imagery_)
            if (t.used + 1 >= frame_) ++activeImagery;
        double imageryRoom = static_cast<double>(imageryLimit()) * 0.85 - static_cast<double>(activeImagery);
        double surfaceRoom = std::min(static_cast<double>(surfaceCapacity_) * 0.85 - static_cast<double>(seen.size()) - baseSurfaces,
                                      2.5 * static_cast<double>(seen.size()) + 64.0);
        int freshBudget = imageryRoom < 8.0 ? 0 : std::clamp(static_cast<int>(imageryRoom / 2.0), 4, urgentPrefetch_ ? 96 : 32);
        int fresh = 0;
        size_t admitted = 0;
        std::sort(prefetch_.begin(), prefetch_.end(), [](const Need& a, const Need& b) { return a.priority < b.priority; });
        for (const Need& n : prefetch_) {
            if (static_cast<double>(admitted) >= surfaceRoom) break;
            if (std::binary_search(seen.begin(), seen.end(), n.key, keyLess)) continue;
            auto sf = surface_.find(n.key);
            bool done = sf != surface_.end() && sf->second.complete;
            if (!done) {
                if (fresh >= freshBudget) continue;
                ++fresh;
            }
            needs_.push_back(n);
            ++admitted;
        }
        stats_.prefetched = static_cast<int>(admitted);
        admittedCache_.assign(needs_.begin() + static_cast<std::ptrdiff_t>(firstAdmitted), needs_.end());
    }
    for (int level = 0; level <= baseSurfaceLevel; ++level)
        for (int y = 0; y < (1 << level); ++y)
            for (int x = 0; x < (2 << level); ++x) {
                QuadKey k{level, x, y};
                auto sf = surface_.find(k);
                if (sf != surface_.end() && sf->second.complete) sf->second.used = frame_;
                else {
                    const bool stale = switchVersion_ > 0 && (sf == surface_.end() || sf->second.version < switchVersion_);
                    needs_.push_back({k, stale ? 1e-3 * level - 1.0 : 1e12 * (1 + level), true});
                }
            }
    std::sort(needs_.begin(), needs_.end(), [](const Need& a, const Need& b) {
        if (!(a.key == b.key)) {
            if (a.key.level != b.key.level) return a.key.level < b.key.level;
            if (a.key.x != b.key.x) return a.key.x < b.key.x;
            return a.key.y < b.key.y;
        }
        return a.priority < b.priority;
    });
    needs_.erase(std::unique(needs_.begin(), needs_.end(), [](const Need& a, const Need& b) { return a.key == b.key; }),
                 needs_.end());
    std::sort(needs_.begin(), needs_.end(), [](const Need& a, const Need& b) { return a.priority < b.priority; });

    profileMark(2);
    int builds = 0;
    int deferred = 0;
    int visibleBuilds = 0;
    int visibleDeferred = 0;
    int currentTerrain = elevation_.version() + terrainEpoch_ * 1000000;
    for (const Need& n : needs_) {
        auto it = terrain_.find(n.key);
        if (it == terrain_.end()) {
            if (builds < kTerrainBuildsPerFrame && elapsedMs() < budgetMs * 0.6) {
                TerrainTile& t = terrain_[n.key];
                buildTerrain(n.key, t, n.priority);
                t.used = frame_;
                ++builds;
                if (!n.prefetch) ++visibleBuilds;
            } else {
                ++deferred;
                if (!n.prefetch) ++visibleDeferred;
            }
            continue;
        }
        it->second.used = frame_;
        if (!it->second.complete && it->second.version != currentTerrain && builds < kTerrainBuildsPerFrame &&
            elapsedMs() < budgetMs * 0.6) {
            buildTerrain(n.key, it->second, n.priority);
            ++builds;
            if (!n.prefetch) ++visibleBuilds;
        } else if (!it->second.complete && terrainEnabled_ && !elevation_.empty()) {
            std::vector<float> scratch;
            Bounds b = bounds(n.key);
            double cell = (b.north - b.south) / kGrid;
            elevation_.gridFor(b.west - cell, std::max(b.south - cell, -90.0), b.east + cell,
                               std::min(b.north + cell, 90.0), kPosts + 2, n.priority, scratch, false);
        }
        if (!it->second.complete && it->second.version != currentTerrain) {
            ++deferred;
            if (!n.prefetch) ++visibleDeferred;
        }
    }
    elevation_.submit();

    profileMark(3);
    struct Work {
        double priority;
        size_t index;
        bool render;
        bool prefetch;
    };
    double visibleDone = -1;
    std::vector<Work> work;
    const double renderClock = now();
    auto lineDegFor = [&](const QuadKey& key) {
        Bounds jb = bounds(key);
        Vec3 jc = geodeticToEcef(0.5 * (jb.south + jb.north), 0.5 * (jb.west + jb.east), 0.0);
        double jr = (jb.north - jb.south) * kDegToRad * kWgs84A * 0.75;
        double jd = std::max(length(jc - cam.eye()) - jr, cam.eyeAboveGround());
        return std::max(cam.pixelSizeAt(std::max(std::sqrt(jd * length(jc - cam.eye())), cam.eyeAboveGround())) / 111320.0,
                        (jb.north - jb.south) / kSurfaceSize * 0.85);
    };
    for (size_t i = 0; i < needs_.size(); ++i) {
        auto it = surface_.find(needs_[i].key);
        bool pf = needs_[i].prefetch;
        if (it == surface_.end()) {
            work.push_back({needs_[i].priority, i, true, pf});
        } else {
            it->second.used = frame_;
            if (it->second.complete && it->second.overlaysOmitted && !standIn(cam, needs_[i].key)) it->second.complete = false;
            if (it->second.complete && ((labelsOn_ && !lines3d_) || contours_) && it->second.lineDeg > 0.0 && lineDegFor(needs_[i].key) * 2.0 < it->second.lineDeg)
                it->second.complete = false;
            const bool settled = it->second.version < 0 || it->second.renderedAt < 0.0 || renderClock - it->second.renderedAt >= rerenderGap_;
            if (!it->second.complete && settled && progressed(it->second)) work.push_back({needs_[i].priority * 1.5, i, true, pf});
            else if (!it->second.complete) work.push_back({needs_[i].priority, i, false, pf});
        }
    }
    std::stable_sort(work.begin(), work.end(), [](const Work& a, const Work& b) {
        if (a.prefetch != b.prefetch) return !a.prefetch;
        return a.priority < b.priority;
    });
    int rasterLayers = 0;
    for (size_t l = 0; l < matrices_.size() && l < layers_.size(); ++l)
        if (shown(static_cast<int>(l)) && !(l < vectorLayers_.size() && vectorLayers_[l])) ++rasterLayers;
    const int maxTouched = std::clamp(static_cast<int>(imageryLimit() * 0.8 / (2.5 * std::max(rasterLayers, 1))), 16, 128);
    int touched = 0;
    int hollowSkipped = 0;

    std::vector<SurfaceJob> jobs;
    std::vector<float> verts;
    std::vector<float> fverts;
    std::unordered_map<QuadKey, double, QuadKeyHash> jobTexel;
    std::unordered_map<QuadKey, double, QuadKeyHash> jobLabel;
    int renders = 0;
    int visibleRenders = 0;
    for (const Work& w : work) {
        size_t i = w.index;
        bool speculative = w.prefetch;
        if (speculative) {
            if (touched >= maxTouched) break;
            ++touched;
            if (visibleDone < 0) visibleDone = elapsedMs();
        }
        double specBudget = prefetchBudgetMs * (urgentPrefetch_ ? 3.0 : 1.0);
        const bool holding = snapshotShown_ && snapshotFade_ < 0.0;
        bool overBudget = renders > 0 && (elapsedMs() > budgetMs * (holding ? 2.5 : 1.0) || (speculative && elapsedMs() - visibleDone > specBudget));
        const bool placeholder = standIn(cam, needs_[i].key);
        if (!w.render || renders >= kSurfaceRendersPerFrame * (holding ? 2 : 1) || verts.size() * sizeof(float) > kSurfaceBufferBytes / 2 || overBudget) {
            if (w.render) ++deferred;
            if (w.render && !speculative) ++visibleDeferred;
            buildSurfaceJob(needs_[i].key, needs_[i].priority, nullptr, nullptr, nullptr, 0, placeholder);
            continue;
        }
        SurfaceJob job;
        job.key = needs_[i].key;
        job.standIn = placeholder;
        size_t vertsBefore = verts.size();
        Bounds jb = bounds(job.key);
        Vec3 jc = geodeticToEcef(0.5 * (jb.south + jb.north), 0.5 * (jb.west + jb.east), 0.0);
        double jr = (jb.north - jb.south) * kDegToRad * kWgs84A * 0.75;
        double jd = std::max(length(jc - cam.eye()) - jr, cam.eyeAboveGround());
        double texelDeg = std::min((jb.north - jb.south) / kSurfaceSize, cam.pixelSizeAt(jd) / 111320.0);
        const double labelDeg = std::max(cam.pixelSizeAt(std::max(std::sqrt(jd * length(jc - cam.eye())), cam.eyeAboveGround())) / 111320.0,
                                         (jb.north - jb.south) / kSurfaceSize * 0.85);
        if (labelsOn_ && !labels_.prepare({jb.west, jb.south, jb.east, jb.north}, labelDeg)) {
            ++deferred;
            if (!speculative) ++visibleDeferred;
            buildSurfaceJob(needs_[i].key, needs_[i].priority, nullptr, nullptr, nullptr, 0, placeholder);
            continue;
        }
        jobLabel[job.key] = labelDeg;
        size_t vectorStart = fverts.size();
        buildSurfaceJob(job.key, needs_[i].priority, &job, &verts, nullptr, texelDeg, job.standIn);
        if (job.hollow > 0) {
            verts.resize(vertsBefore);
            ++hollowSkipped;
            continue;
        }
        job.featureFirst = static_cast<int>(vectorStart / 6);
        job.featureCount = 0;
        jobTexel[job.key] = texelDeg;
        if ((!features_.empty() || anyGrid()) && fverts.size() * sizeof(float) < kSurfaceBufferBytes / 2) {
            Bounds b = jb;
            size_t before = vectorStart;
            features_.buildSurface({b.west, b.south, b.east, b.north}, texelDeg, dpiScale_, fverts);
            if (gridOn_[1]) buildGraticule({b.west, b.south, b.east, b.north}, texelDeg, dpiScale_, gridSpacing_[1], false, fverts);
            if (gridOn_[2]) buildGraticule({b.west, b.south, b.east, b.north}, texelDeg, dpiScale_, gridSpacing_[2], true, fverts);
            if (gridOn_[0]) buildGrid({b.west, b.south, b.east, b.north}, texelDeg, dpiScale_, gridSpacing_[0], fverts);
            job.featureFirst = static_cast<int>(before / 6);
            job.featureCount = static_cast<int>((fverts.size() - before) / 6);
        }
        jobs.push_back(std::move(job));
        ++renders;
        if (!speculative) ++visibleRenders;
    }
    if (buildingsOn_ && overlayLayer_ >= 0 && overlayLayer_ < static_cast<int>(matrices_.size())) {
        auto tileFn = [this](int x, int y, double priority, std::shared_ptr<const VectorTile>& out) {
            const TileId id{overlayLayer_, 14, x, y};
            auto it = imagery_.find(id);
            if (it != imagery_.end()) {
                it->second.used = frame_;
                out = it->second.vector;
                return out ? 1 : -1;
            }
            auto f = failed_.find(id);
            if (f != failed_.end()) return f->second == std::numeric_limits<double>::infinity() ? -1 : 0;
            want(id, priority / 1000.0);
            return 0;
        };
        auto heightsFn = [this](const std::vector<std::pair<double, double>>& ll) { return elevation_.query(ll, 4.0); };
        // auto canopyFn = [this](int x, int y, std::vector<std::uint8_t>& out) { return static_cast<int>(canopy_.heights(x, y, out)); };
        // buildings_.update(cam, tileFn, heightsFn, roofImagery(), canopyFn, exaggeration_, terrainEnabled_);
        buildings_.update(cam, tileFn, heightsFn, roofImagery(), {}, exaggeration_, terrainEnabled_);
    }
    {
        std::sort(requests_.begin(), requests_.end(), [](const auto& a, const auto& b) {
            const TileId& x = a.first;
            const TileId& y = b.first;
            if (x.layer != y.layer) return x.layer < y.layer;
            if (x.z != y.z) return x.z < y.z;
            if (x.x != y.x) return x.x < y.x;
            if (x.y != y.y) return x.y < y.y;
            return a.second < b.second;
        });
        requests_.erase(std::unique(requests_.begin(), requests_.end(),
                                    [](const auto& a, const auto& b) { return a.first == b.first; }),
                        requests_.end());
        loader_.submit(requests_);
    }
    if (verts.size() * sizeof(float) > kSurfaceBufferBytes) verts.resize(kSurfaceBufferBytes / sizeof(float));

    if (!verts.empty()) {
        sg_write_buffer_desc wd{};
        wd.src.data = {verts.data(), verts.size() * sizeof(float)};
        wd.dst.buffer = surfaceBuffer_;
        wd.size = verts.size() * sizeof(float);
        sg_write_buffer_transient(&wd);
    }

    if (fverts.size() * sizeof(float) > kSurfaceBufferBytes) fverts.resize(kSurfaceBufferBytes / sizeof(float) / 6 * 6);
    if (!fverts.empty()) {
        sg_write_buffer_desc wd{};
        wd.src.data = {fverts.data(), fverts.size() * sizeof(float)};
        wd.dst.buffer = featureBuffer_;
        wd.size = fverts.size() * sizeof(float);
        sg_write_buffer_transient(&wd);
    }

    profileMark(4);
    std::vector<sg_image> mipQueue;
    if (capture) jobs.clear();
    for (auto& job : jobs) {
        SurfaceTile* s = acquireSurface(job.key);
        if (sg_query_view_state(s->texture) != SG_RESOURCESTATE_VALID || sg_query_view_state(s->levels[0]) != SG_RESOURCESTATE_VALID) {
            poolPressure("surface");
            dropSurface(*s);
            surface_.erase(job.key);
            continue;
        }
        if (fadeUpgrades_ && s->version >= 0) {
            if (s->shown + 1 >= frame_) {
                SurfaceTile fresh = freshSurface();
                if (sg_query_view_state(fresh.texture) == SG_RESOURCESTATE_VALID) {
                    retiring_[job.key].push_back({*s, frame_});
                    ++retiredCount_;
                    s->image = fresh.image;
                    s->attachment = fresh.attachment;
                    s->texture = fresh.texture;
                    s->levels = fresh.levels;
                    s->sources = fresh.sources;
                }
            }
        }
        s->used = frame_;
        bool jobComplete = job.complete;
        sg_pass pass{};
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        if (job.background) pass.action.colors[0].clear_value = {242 / 255.0f, 239 / 255.0f, 233 / 255.0f, 1.0f};
        else pass.action.colors[0].clear_value = {ground_[0], ground_[1], ground_[2], 1.0f};
        pass.attachments.colors[0] = glMips_ ? s->levels[0] : scratchAttachment_;
        sg_begin_pass(&pass);
        if (!job.draws.empty()) {
            sg_apply_pipeline(surfacePipeline_);
            for (auto& d : job.draws) {
                if ((d.first + d.count) * 4 * sizeof(float) > kSurfaceBufferBytes) continue;
                if (sg_query_view_state(d.texture) != SG_RESOURCESTATE_VALID) {
                    ++invalidResources_;
                    jobComplete = false;
                    continue;
                }
                sg_bindings bind{};
                bind.vertex_buffers[0] = surfaceBuffer_;
                bind.views[VIEW_tex] = d.texture;
                bind.samplers[SMP_smp] = imagerySampler_;
                sg_apply_bindings(&bind);
                surface_fs_params_t fsp{};
                fsp.tint[0] = d.tint[0];
                fsp.tint[1] = d.tint[1];
                fsp.tint[2] = d.tint[2];
                fsp.tint[3] = d.opacity;
                sg_apply_uniforms(UB_surface_fs_params, SG_RANGE(fsp));
                sg_draw(d.first, d.count, 1);
            }
        }
        if (!job.vectors.empty()) {
            Bounds jb = bounds(job.key);
            double coslat = std::max(std::cos((jb.south + jb.north) * 0.5 * kDegToRad), 0.05);
            double texel = jobTexel.count(job.key) ? jobTexel[job.key] : (jb.north - jb.south) / kSurfaceSize;
            for (int pass = 0; pass < 2; ++pass) {
                sg_apply_pipeline(pass == 0 ? vfillPipeline_ : vlinePipeline_);
                for (const VectorDraw& vd : job.vectors) {
                    const ImageryTexture* t = vd.tile;
                    if (pass == 0 ? !t->fill.id : !t->line.id) continue;
                    if (sg_query_buffer_state(pass == 0 ? t->fill : t->line) != SG_RESOURCESTATE_VALID) {
                        ++invalidResources_;
                        jobComplete = false;
                        continue;
                    }
                    sg_bindings vb{};
                    vb.vertex_buffers[0] = pass == 0 ? t->fill : t->line;
                    sg_apply_bindings(&vb);
                    vector_params_t vp{};
                    std::memcpy(vp.xform, vd.xform, sizeof(vd.xform));
                    vp.line[0] = static_cast<float>(coslat);
                    vp.line[1] = static_cast<float>(texel * dpiScale_);
                    sg_apply_uniforms(UB_vector_params, SG_RANGE(vp));
                    sg_draw(0, pass == 0 ? t->fillCount : t->lineCount, 1);
                }
            }
        }
        if (labelsOn_ || contours_) {
            Bounds lb = bounds(job.key);
            double unit = jobLabel.count(job.key) ? jobLabel[job.key] : (lb.north - lb.south) / kSurfaceSize;
            if (contours_) labels_.drawContours({lb.west, lb.south, lb.east, lb.north}, unit, dpiScale_);
            if (labelsOn_ && !lines3d_) {
                std::vector<RoadBatch> roads;
                for (const VectorDraw& vd : job.vectors)
                    if (vd.tile->roadCount > 0) {
                        RoadBatch rb;
                        rb.buffer = vd.tile->roads;
                        rb.count = buildingsOn_ && lb.north - lb.south < 0.015 ? vd.tile->solidRoads : vd.tile->roadCount;
                        std::memcpy(rb.xform, vd.xform, sizeof(rb.xform));
                        roads.push_back(rb);
                    }
                labels_.drawRoads(roads, {lb.west, lb.south, lb.east, lb.north}, unit, dpiScale_);
                labels_.drawSurface({lb.west, lb.south, lb.east, lb.north}, unit, dpiScale_);
            }
        }
        if (job.featureCount > 0 && (job.featureFirst + job.featureCount) * 6 * sizeof(float) <= kSurfaceBufferBytes)
            features_.drawSurface(featureBuffer_, job.featureFirst, job.featureCount);
        sg_end_pass();
        if (glMips_) {
            mipQueue.push_back(s->image);
            s->complete = jobComplete;
            s->version = imageryVersion_;
            s->missing = std::move(job.missing);
            s->imageryZ = job.imageryZ;
            s->sourceZ = job.sourceZ;
            s->degraded = job.degraded;
            s->overlaysOmitted = job.overlaysOmitted;
            s->lineDeg = jobLabel.count(job.key) ? jobLabel[job.key] : 0.0;
            s->presentable = job.presentable;
            s->renderedAt = now();
            s->roadTiles.clear();
            for (const VectorDraw& vd : job.vectors)
                if (vd.tile->roadCount > 0) s->roadTiles.push_back(vd.id);
            if (lines3d_)
                for (const TileId& r : s->roadTiles) roadBuildQueue_.insert(r);
            if (visualLog_ && frame_ % 20 == 0) {
                int withVector = 0;
                for (const VectorDraw& vd : job.vectors) withVector += vd.tile && vd.tile->vector ? 1 : 0;
                std::fprintf(visualLog_, "surfjob frame %llu L%d/%d/%d vectors %zu withvector %d roads %zu standin %d omitted %d\n",
                             static_cast<unsigned long long>(frame_), job.key.level, job.key.x, job.key.y, job.vectors.size(), withVector,
                             s->roadTiles.size(), job.standIn ? 1 : 0, job.overlaysOmitted ? 1 : 0);
            }
            continue;
        }
        for (int level = 0; level < kMips; ++level) {
            sg_pass mp{};
            mp.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
            mp.attachments.colors[0] = s->levels[level];
            sg_begin_pass(&mp);
            sg_apply_pipeline(mipPipeline_);
            sg_bindings mb{};
            mb.vertex_buffers[0] = skyBuffer_;
            mb.views[VIEW_tex] = scratchTexture_;
            mb.samplers[SMP_smp] = mipSampler_;
            sg_apply_bindings(&mb);
            mip_params_t mpp{};
            float block = static_cast<float>(1 << level);
            mpp.params[0] = flipSurface_ ? 1.0f : 0.0f;
            mpp.params[1] = block;
            mpp.params[2] = std::min(block, 16.0f);
            mpp.params[3] = 1.0f / kSurfaceSize;
            sg_apply_uniforms(UB_mip_params, SG_RANGE(mpp));
            sg_draw(0, 3, 1);
            sg_end_pass();
        }
        s->complete = jobComplete;
        s->version = imageryVersion_;
        s->missing = std::move(job.missing);
        s->imageryZ = job.imageryZ;
        s->sourceZ = job.sourceZ;
        s->degraded = job.degraded;
        s->overlaysOmitted = job.overlaysOmitted;
        s->lineDeg = jobLabel.count(job.key) ? jobLabel[job.key] : 0.0;
        s->presentable = job.presentable;
        s->renderedAt = now();
        s->roadTiles.clear();
        for (const VectorDraw& vd : job.vectors)
            if (vd.tile->roadCount > 0) s->roadTiles.push_back(vd.id);
        if (lines3d_)
            for (const TileId& r : s->roadTiles) roadBuildQueue_.insert(r);
    }

    profileSub(-1);
    if (!mipQueue.empty()) generateMips(mipQueue);
    profileSub(7);
    if (buildingsOn_) buildings_.bake(roofImagery(), flipSurface_);

    profileMark(5);
    profileSub(-1);
    const RenderView* firstView = views && !views->empty() ? &views->front() : nullptr;
    const Camera& dc = firstView ? *firstView->cam : cam;
    auto ground = [this](double lat, double lon) { return elevationAt(lat, lon); };
    sg_pass main{};
    main.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    main.action.colors[0].clear_value = {clear_[0], clear_[1], clear_[2], 1.0f};
    main.action.depth.load_action = SG_LOADACTION_CLEAR;
    main.action.depth.clear_value = 1.0f;
    const sg_swapchain* mainSwapchain = firstView ? firstView->swapchain : swapchain;
    const sg_attachments* mainAttachments = firstView ? firstView->attachments : attachments;
    if (capture) {
        main.attachments.colors[0] = snapshotColor_;
        main.attachments.resolves[0] = snapshotResolve_;
        main.attachments.depth_stencil = snapshotDepthView_;
    } else {
        if (mainSwapchain) main.swapchain = *mainSwapchain;
        if (mainAttachments) main.attachments = *mainAttachments;
    }
    sg_begin_pass(&main);
    if (firstView && firstView->width > 0) sg_apply_viewport(firstView->x, firstView->y, firstView->width, firstView->height, true);

    auto drawSky = [&](const Camera& c) {
        sg_apply_pipeline(skyPipeline_);
        sg_bindings sb{};
        sb.vertex_buffers[0] = skyBuffer_;
        sg_apply_bindings(&sb);
        sky_fs_params_t sky{};
        toFloat(c.invViewProjRel(), sky.inv_vp);
        Vec3 center = -c.eye();
        sky.center[0] = static_cast<float>(center.x);
        sky.center[1] = static_cast<float>(center.y);
        sky.center[2] = static_cast<float>(center.z);
        sky.radii[0] = 6356752.0f;
        sky.radii[1] = 6356752.0f + 100000.0f;
        sky.radii[2] = c.eyeAltitude() < 100000.0 ? 1.0f : 0.0f;
        sky.radii[3] = holeMarker_ ? 1.0f : 0.0f;
        sg_apply_uniforms(UB_sky_fs_params, SG_RANGE(sky));
        sg_draw(0, 3, 1);
    };
    bool xray = terrainAlpha_ < 0.999f;
    if (skyVisible_ && xray) drawSky(dc);
    bool tilesBusy = false;
    stats_.tilesetTiles = 0;
    stats_.tilesetFailed = 0;
    auto drawObjects = [&]() {
        if (buildingsOn_) buildings_.draw(dc, sunDir_, sunOn_);
        models_.draw(dc, ground);
        points_.draw(dc, dpiScale_);
        for (auto& [id, t] : tilesets_) {
            t->update(cam, frustum_, frame_);
            t->draw(dc, models_, points_, dpiScale_);
            tilesBusy = tilesBusy || t->busy();
            stats_.tilesetTiles += t->drawnTiles();
            stats_.tilesetFailed += t->failedTiles();
        }
    };
    if (xray) drawObjects();
    sg_apply_pipeline(xray ? globeBlendPipeline_ : globePipeline_);
    globe_fs_params_t gfs{};
    gfs.flags[0] = flipSurface_ ? 1.0f : 0.0f;
    gfs.flags[1] = depthPerception_ && terrainEnabled_ ? 1.0f : 0.0f;
    gfs.flags[3] = terrainAlpha_;
    gfs.heat[0] = static_cast<float>(heatMin_);
    gfs.heat[1] = static_cast<float>(1.0 / std::max(heatMax_ - heatMin_, 1.0));
    gfs.heat[2] = heatmap_ ? heatOpacity_ : 0.0f;
    gfs.slope[0] = slope_ && terrainEnabled_ ? slopeOpacity_ : 0.0f;
    gfs.slope[1] = static_cast<float>(1.0 / std::max(exaggeration_, 0.01));
    gfs.slope[2] = static_cast<float>(slopeMax_);
    sg_apply_uniforms(UB_globe_fs_params, SG_RANGE(gfs));
    int maxLevel = 0;
    int incomplete = 0;
    int blank = 0;
    int coarse = 0;
    int maxDepth = 0;
    int deepLogged = 0;
    int coverageOnly = 0, retained = 0;
    for (const DrawTile& dt : tiles) {
        if (dt.kind == 1) ++coverageOnly;
        if (dt.kind == 2) ++retained;
    }
    bool fading = false;
    int fadeKinds[3] = {0, 0, 0};
    double tnow = now();
    auto source = [&](const QuadKey& key, int minUp, sg_view& view, float uvx[4]) {
        int bestK = -1;
        int bestZ = -1000;
        SurfaceTile* best = nullptr;
        for (int pass = 0; pass < 2 && !best; ++pass)
            for (int k = std::max(0, minUp); k <= key.level; ++k) {
                QuadKey a{key.level - k, key.x >> k, key.y >> k};
                auto it = surface_.find(a);
                if (it == surface_.end() || it->second.version < 0) continue;
                if (pass == 0 && it->second.version < switchVersion_) continue;
                int z = it->second.complete ? std::max(it->second.sourceZ, it->second.imageryZ) : it->second.sourceZ;
                if (z > bestZ) {
                    bestZ = z;
                    bestK = k;
                    best = &it->second;
                }
                if (it->second.complete) break;
            }
        if (!best) return QuadKey{-1, 0, 0};
        best->used = frame_;
        best->shown = frame_;
        view = best->texture;
        double sc = 1.0 / (1 << bestK);
        QuadKey a{key.level - bestK, key.x >> bestK, key.y >> bestK};
        int rx = key.x - (a.x << bestK), ry = key.y - (a.y << bestK);
        uvx[0] = static_cast<float>(sc);
        uvx[1] = static_cast<float>(sc);
        uvx[2] = static_cast<float>(rx * sc);
        uvx[3] = static_cast<float>(((1 << bestK) - 1 - ry) * sc);
        return a;
    };
    if (diagnostics_ && frame_ % 90 == 45 && !tiles.empty()) {
        const DrawTile& probe = tiles.front();
        sg_view pv{};
        float pu[4];
        QuadKey ps = source(probe.key, 0, pv, pu);
        auto os = surface_.find(probe.key);
        auto ss = ps.level >= 0 ? surface_.find(ps) : surface_.end();
        diag("probe frame %llu: L%d/%d/%d kind %d src L%d own %s v%d imgZ %d srcZ %d missing %zu | src surf imgZ %d srcZ %d | failed %zu imagery %zu",
             static_cast<unsigned long long>(frame_), probe.key.level, probe.key.x, probe.key.y, probe.kind, ps.level,
             os == surface_.end() ? "-" : os->second.complete ? "C" : "p", os == surface_.end() ? -9 : os->second.version,
             os == surface_.end() ? -1 : os->second.imageryZ, os == surface_.end() ? -1 : os->second.sourceZ,
             os == surface_.end() ? size_t(0) : os->second.missing.size(), ss == surface_.end() ? -1 : ss->second.imageryZ,
             ss == surface_.end() ? -1 : ss->second.sourceZ, failed_.size(), imagery_.size());
    }
    for (auto& dt : tiles) drawn_[dt.key] = frame_;
    {
        auto refLess = [](const FadeRef& a, const FadeRef& b) {
            if (a.key.level != b.key.level) return a.key.level < b.key.level;
            if (a.key.x != b.key.x) return a.key.x < b.key.x;
            if (a.key.y != b.key.y) return a.key.y < b.key.y;
            return a.version < b.version;
        };
        std::sort(fadeRefs_.begin(), fadeRefs_.end(), refLess);
        auto progressOf = [&](const QuadKey& k, int version) {
            auto r = std::lower_bound(fadeRefs_.begin(), fadeRefs_.end(), FadeRef{k, version, 0.0}, refLess);
            double p = -1.0;
            for (; r != fadeRefs_.end() && r->key == k && r->version == version; ++r) p = std::max(p, r->progress);
            return p;
        };
        struct Held {
            double progress;
            QuadKey key;
            int version;
        };
        std::vector<Held> held;
        for (auto it = retiring_.begin(); it != retiring_.end();) {
            auto& list = it->second;
            for (size_t i = 0; i < list.size();) {
                const double p = list[i].frame == frame_ ? -1.0 : progressOf(it->first, list[i].surface.version);
                if (list[i].frame == frame_ || p >= 0.0) {
                    if (p >= 0.0) held.push_back({p, it->first, list[i].surface.version});
                    ++i;
                    continue;
                }
                recycleSurface(list[i].surface);
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                --retiredCount_;
            }
            it = list.empty() ? retiring_.erase(it) : std::next(it);
        }
        const size_t cap = std::max<size_t>(64, freeSurfacePool_ * 3);
        if (retiredCount_ > cap) {
            std::sort(held.begin(), held.end(), [](const Held& a, const Held& b) { return a.progress > b.progress; });
            for (const Held& h : held) {
                if (retiredCount_ <= cap) break;
                auto it = retiring_.find(h.key);
                if (it == retiring_.end()) continue;
                auto& list = it->second;
                for (size_t i = 0; i < list.size(); ++i)
                    if (list[i].surface.version == h.version && list[i].frame != frame_) {
                        recycleSurface(list[i].surface);
                        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                        --retiredCount_;
                        break;
                    }
                if (list.empty()) retiring_.erase(it);
            }
        }
        fadeRefs_.clear();
    }
    if (drawn_.size() > 20000)
        for (auto it = drawn_.begin(); it != drawn_.end();) it = it->second + 2 < frame_ ? drawn_.erase(it) : std::next(it);
    if (transitions_.size() > 20000)
        for (auto it = transitions_.begin(); it != transitions_.end();) it = it->second.frame + 2 < frame_ ? transitions_.erase(it) : std::next(it);
    if (merges_.size() > 20000)
        for (auto it = merges_.begin(); it != merges_.end();) it = it->second.frame + 2 < frame_ ? merges_.erase(it) : std::next(it);
    if (morphSince_.size() > 20000)
        for (auto it = morphSince_.begin(); it != morphSince_.end();)
            it = it->second.second + 2 < frame_ ? morphSince_.erase(it) : std::next(it);
    struct TerrainDraw {
        sg_buffer vbuf;
        sg_view view;
        sg_view view2;
        sg_view view3;
        globe_vs_params_t vsp;
        Vec3 center;
        float blend;
        float merge;
    };
    std::vector<TerrainDraw> terrainDraws;
    if (views && views->size() > 1) terrainDraws.reserve(tiles.size());
    std::unordered_map<QuadKey, const TerrainTile*, QuadKeyHash> drawnNow;
    drawnNow.reserve(tiles.size() * 2);
    for (auto& dt : tiles) drawnNow[dt.key] = &terrain(dt.key);
    auto displayLevel = [&](const Transition& r) {
        if (r.src.level < 0) return -1.0f;
        double level = static_cast<double>(r.src.level);
        if (r.held && r.from.level >= 0) level = r.from.level;
        else if (r.start >= 0.0) {
            const double t = (tnow - r.start) / kFadeSeconds;
            if (t < 1.0 && r.from.level >= 0 && r.from.level < r.src.level)
                level = r.from.level + t * t * (3.0 - 2.0 * t) * (r.src.level - r.from.level);
        }
        if (r.merge.level >= 0 && r.mergeFrame + 1 >= frame_) {
            const double m = std::clamp(static_cast<double>(r.mergeT), 0.0, 1.0);
            level -= m * m * (3.0 - 2.0 * m) * (level - r.merge.level);
        }
        return static_cast<float>(std::max(0.0, level - r.depth));
    };
    auto neighbourLevel = [&](const QuadKey& k, int dir) {
        const int nx = 2 << k.level, ny = 1 << k.level;
        QuadKey nk = k;
        if (dir == 0) {
            if (k.y + 1 >= ny) return -1.0f;
            nk.y = k.y + 1;
        } else if (dir == 1) {
            if (k.y == 0) return -1.0f;
            nk.y = k.y - 1;
        } else if (dir == 2) {
            nk.x = (k.x + nx - 1) % nx;
        } else {
            nk.x = (k.x + 1) % nx;
        }
        auto levelOf = [&](const QuadKey& q) {
            auto r = transitions_.find(q);
            return r == transitions_.end() || r->second.frame + 1 < frame_ ? -1.0f : displayLevel(r->second);
        };
        for (int up = 0; up <= nk.level; ++up) {
            const QuadKey a{nk.level - up, nk.x >> up, nk.y >> up};
            if (drawnNow.count(a)) return levelOf(a);
        }
        float low = -1.0f;
        std::vector<QuadKey> frontier{nk};
        for (int depth = 0; depth < 4 && !frontier.empty(); ++depth) {
            std::vector<QuadKey> next;
            for (const QuadKey& f : frontier)
                for (int c = 0; c < 2; ++c) {
                    QuadKey kid{f.level + 1, f.x * 2, f.y * 2};
                    if (dir == 0) kid = {f.level + 1, f.x * 2 + c, f.y * 2};
                    else if (dir == 1) kid = {f.level + 1, f.x * 2 + c, f.y * 2 + 1};
                    else if (dir == 2) kid = {f.level + 1, f.x * 2 + 1, f.y * 2 + c};
                    else kid = {f.level + 1, f.x * 2, f.y * 2 + c};
                    if (drawnNow.count(kid)) {
                        const float v = levelOf(kid);
                        if (v >= 0.0f) low = low < 0.0f ? v : std::min(low, v);
                    } else {
                        next.push_back(kid);
                    }
                }
            frontier.swap(next);
        }
        return low;
    };
    {
        const double key[5] = {cam.focus.lat, cam.focus.lon, std::log(std::max(cam.gsd, 1e-6)), cam.azimuth, cam.tilt};
        const double perDegree = 111320.0 / std::max(cam.gsd, 1e-6);
        const double half = 0.5 * std::max(cam.width, cam.height);
        const double moved = std::max({std::abs(key[0] - motionKey_[0]) * perDegree,
                                       std::abs(std::remainder(key[1] - motionKey_[1], 360.0)) * perDegree * std::cos(cam.focus.lat * kDegToRad),
                                       std::abs(key[2] - motionKey_[2]) * half,
                                       std::abs(std::remainder(key[3] - motionKey_[3], 360.0)) * kDegToRad * half,
                                       std::abs(key[4] - motionKey_[4]) * kDegToRad * half});
        if (moved > motionSpeed_ * std::max(tnow - motionTime_, 1.0 / 240.0)) {
            lastMotion_ = tnow;
            if (visualLog_)
                std::fprintf(visualLog_, "motion frame %llu t %.3f px %.2f dlat %.3g dlon %.3g dlog %.3g daz %.3g dtilt %.3g\n",
                             static_cast<unsigned long long>(frame_), tnow, moved, key[0] - motionKey_[0], key[1] - motionKey_[1],
                             key[2] - motionKey_[2], key[3] - motionKey_[3], key[4] - motionKey_[4]);
        }
        std::copy(key, key + 5, motionKey_);
        motionTime_ = tnow;
    }
    if (snapshotShown_ && settleSnapshot_) {
        const bool resized = width != snapshotWidth_ || height != snapshotHeight_;
        const bool moved = lastMotion_ > snapshotStart_;
        if (moved && !resized) {
            if (snapshotFade_ < 0.0) {
                snapshotFade_ = tnow;
                snapshotFadeLength_ = settleMoveFade_;
                if (visualLog_)
                    std::fprintf(visualLog_, "settle frame %llu t %.3f release moving\n", static_cast<unsigned long long>(frame_), tnow);
            }
        } else if (moved || resized) {
            snapshotShown_ = false;
            if (visualLog_)
                std::fprintf(visualLog_, "settle frame %llu t %.3f drop %s\n", static_cast<unsigned long long>(frame_), tnow,
                             snapshotFade_ >= 0.0 ? "dissolving" : "frozen");
        }
    }
    const bool frozenNow = snapshotShown_ && settleSnapshot_ && snapshotFade_ < 0.0;
    if (snapshotShown_ && settleSnapshot_ && snapshotFade_ >= 0.0) stepInstant_ = true;
    const bool settleStill = settleFreeze_ && !settleBroken_ && !views && terrainAlpha_ >= 0.999f && tnow - lastMotion_ >= 0.15;
    bool gateOpen = !settleStill && revealGate(tnow);
    const bool stepping = settleFreeze_ && !settleBroken_ && !views && uniformMix_ >= 0.5 && tnow - lastMotion_ < 0.15;
    if (gateOpen && stepping && !stepInstant_) {
        gateOpen = false;
        if (!snapshotShown_ && !stepCapture) stepWanted_ = true;
    }
    revealOpen_ = frozenNow || gateOpen;
    if (revealOpen_ && heldPrev_ > 0) lastReveal_ = tnow;
    int heldNow = 0;
    int waitingNow = refineWaiting_;
    double oldestHeld = tnow;
    double visPop = 0, visMerge = 0, visTerrain = 0, visBlur = 0, visBlur2 = 0, visDepth = 0, visFade = 0, visCover = 0, visReveal = 0;
    int visReveals = 0;
    double visUngated = 0, visMergeStart = 0;
    int visUngatedTiles = 0, visDown = 0, visRefade = 0, visLost = 0, visMergeStarts = 0;
    int visPops = 0;
    if (visualStats_ && shown_.size() > 20000)
        for (auto it = shown_.begin(); it != shown_.end();) it = it->second.frame + 2 < frame_ ? shown_.erase(it) : std::next(it);
    auto depthOf = [&](const QuadKey& key, const QuadKey& s) {
        if (s.level < 0) return 99;
        auto st = surface_.find(s);
        if (st == surface_.end()) return 0;
        const int ideal = idealImageryZ(key);
        const int effective = st->second.sourceZ;
        if (ideal >= 0 && effective < 99) return std::max(0, ideal - effective);
        return key.level - s.level + (st->second.complete ? 0 : 1);
    };
    auto expecting = [&](const QuadKey& key) { return !finished(key); };
    int capDepth = 0;
    if (uniformShare_ > 0.0 && revealHold_ > 0.0 && !tiles.empty()) {
        int hist[17] = {};
        int counted = 0;
        for (const DrawTile& d : tiles) {
            if (!expecting(d.key)) continue;
            sg_view v{};
            float u[4];
            ++hist[std::min(depthOf(d.key, source(d.key, 0, v, u)), 16)];
            ++counted;
        }
        const double need = uniformShare_ * static_cast<double>(counted);
        double acc = 0.0;
        while (counted > 0 && capDepth < 16 && acc + hist[capDepth] < need) acc += hist[capDepth++];
    }
    auto zOf = [&](const QuadKey& q) {
        auto st = q.level >= 0 ? surface_.find(q) : surface_.end();
        if (st == surface_.end()) return -1;
        return st->second.complete ? std::max(st->second.sourceZ, st->second.imageryZ) : st->second.sourceZ;
    };
    int zCap = 99;
    bool capBehind = false;
    if (uniformMix_ >= 0.5 && uniformShare_ > 0.0) {
        int available = 99;
        int ideal = -1;
        std::vector<int> tileZ;
        tileZ.reserve(tiles.size());
        const bool reveal = frozenNow || (revealOpen_ && !stepping) || uniformZ_ >= 99;
        for (const DrawTile& d : tiles) {
            int z = idealImageryZ(d.key);
            ideal = std::max(ideal, z);
            if (expecting(d.key) || retrying(d.key)) {
                sg_view v{};
                float u[4];
                const QuadKey s = source(d.key, 0, v, u);
                if (s.level < 0) continue;
                z = zOf(s);
            }
            if (!reveal) {
                auto held = transitions_.find(d.key);
                if (held != transitions_.end() && held->second.frame + 1 == frame_ && held->second.held && held->second.from.level >= 0) {
                    const int hz = zOf(held->second.from);
                    if (hz >= 0) z = std::min(z, hz);
                }
            }
            if (z >= 0) {
                available = std::min(available, z);
                tileZ.push_back(z);
            }
        }
        int floorZ = ideal >= 0 ? ideal - kMaxUniformDrop : -1;
        if (floorZ >= 0) {
            size_t below = 0;
            for (int z : tileZ) below += z < floorZ ? 1 : 0;
            if (below * 4 > tileZ.size()) floorZ = -1;
        }
        if (pendingMergeZ_ < 99) available = std::min(available, pendingMergeZ_);
        if (available < 99 && floorZ >= 0) available = std::max(available, floorZ);
        int target = available >= 99 ? 99 : reveal ? available : std::min(uniformZ_, available);
        if (target < uniformZ_ && target < 99 && uniformZ_ < 99) {
            bool reachable = true;
            for (const DrawTile& d : tiles) {
                sg_view v{};
                float u[4];
                const QuadKey s = source(d.key, 0, v, u);
                const int sz = s.level >= 0 ? surfaceZMax(s) : -1;
                if (sz <= target) continue;
                int found = -1;
                for (int up = d.key.level - s.level + 1; up <= d.key.level && found < 0; ++up) {
                    const QuadKey a{d.key.level - up, d.key.x >> up, d.key.y >> up};
                    auto st = surface_.find(a);
                    if (st == surface_.end() || st->second.version < 0 || !(st->second.complete || st->second.presentable)) continue;
                    const int az = surfaceZMax(a);
                    if (az >= 0 && az <= target) found = az;
                }
                if (found >= target - 1) continue;
                reachable = false;
                const int up = std::clamp(d.key.level - s.level + (sz - target), 1, d.key.level);
                if (blendParents_.size() < 256) blendParents_.push_back({QuadKey{d.key.level - up, d.key.x >> up, d.key.y >> up}, d.distance * 0.25});
            }
            if (reachable) {
                stepWait_ = -1.0;
            } else {
                if (stepWait_ < 0.0) stepWait_ = tnow;
                if (tnow - stepWait_ < 0.6) target = uniformZ_;
            }
        } else {
            stepWait_ = -1.0;
        }
        if (stepping && !frozenNow && !stepInstant_ && target != uniformZ_ && target < 99 && uniformZ_ < 99) {
            zCap = uniformZ_;
            if (!snapshotShown_ && !stepCapture) stepWanted_ = true;
        } else {
            zCap = target;
        }
        capBehind = zCap < 99 && available < 99 && available > zCap;
        if (visualLog_ && zCap != uniformZ_)
            std::fprintf(visualLog_, "zcap frame %llu t %.3f z %d -> %d available %d tiles %zu\n", static_cast<unsigned long long>(frame_), tnow, uniformZ_,
                         zCap, available, tiles.size());
    }
    if (zCap < uniformZ_) coarsenAt_ = tnow;
    uniformZ_ = zCap;
    if (visualLog_ && capDepth != uniformDepth_)
        std::fprintf(visualLog_, "cap frame %llu t %.3f depth %d -> %d tiles %zu\n", static_cast<unsigned long long>(frame_), tnow, uniformDepth_, capDepth,
                     tiles.size());
    uniformDepth_ = capDepth;
    std::unordered_map<int, int> levelMix;
    std::vector<std::pair<QuadKey, QuadKey>> lineTiles;
    int capFail[4] = {0, 0, 0, 0};
    QuadKey capExample{-1, 0, 0};
    int capExampleZ = -1;
    auto levelMixText = [&]() {
        std::vector<std::pair<int, int>> v(levelMix.begin(), levelMix.end());
        std::sort(v.begin(), v.end());
        std::string out;
        char buf[40];
        for (const auto& [k, n] : v) {
            std::snprintf(buf, sizeof(buf), " L%dz%d:%d", k / 100, k % 100 - 1, n);
            out += buf;
        }
        return out;
    };
    for (auto& dt : tiles) {
        TerrainTile& tt = terrain(dt.key);
        tt.used = frame_;
        maxLevel = std::max(maxLevel, dt.key.level);
        sg_view view = fallbackView_;
        float uvx[4] = {1, 1, 0, 0};
        QuadKey src = source(dt.key, 0, view, uvx);
        if (src.level < 0) backdropUv(dt.key, view, uvx);
        int shownDepth = 0;
        if (src.level < 0) ++blank;
        else {
            auto ownSurface = surface_.find(src);
            int depth = 0;
            int ideal = idealImageryZ(dt.key);
            if (ownSurface != surface_.end()) {
                const SurfaceTile& st = ownSurface->second;
                int effective = st.sourceZ;
                if (ideal >= 0 && effective < 99) depth = std::max(0, ideal - effective);
                else depth = dt.key.level - src.level + (st.complete ? 0 : 1);
            }
            shownDepth = depth;
            if (depth >= 2) ++coarse;
            maxDepth = std::max(maxDepth, depth);
            if (diagnostics_ && depth >= 3 && frame_ % 90 == 0 && deepLogged < 6) {
                ++deepLogged;
                bool needed = false;
                for (const Need& n : needs_)
                    if (n.key == dt.key) {
                        needed = true;
                        break;
                    }
                auto os = surface_.find(dt.key);
                auto ot = terrain_.find(dt.key);
                diag("deep frame %llu: L%d/%d/%d src L%d depth %d kind %s needed %d sticky %d surf %s missing %zu terr %s | needs %zu imagery %zu/%zu pending %d",
                     static_cast<unsigned long long>(frame_), dt.key.level, dt.key.x, dt.key.y, src.level, depth,
                     dt.kind == 1 ? "coverage" : dt.kind == 2 ? "retained" : "normal", needed ? 1 : 0, wasRefined(dt.key) ? 1 : 0,
                     os == surface_.end() ? "-" : os->second.complete ? "C" : os->second.version < 0 ? "0" : "p",
                     os == surface_.end() ? size_t(0) : os->second.missing.size(),
                     ot == terrain_.end() ? "-" : ot->second.complete ? "C" : "p", needs_.size(), imagery_.size(), imageryCapacity_,
                     loader_.pending());
            }
        }
        if (zCap < 99 && src.level >= 0 && surfaceZMax(src) > zCap) {
            for (int up = dt.key.level - src.level + 1; up <= dt.key.level; ++up) {
                sg_view cv{};
                float cu[4];
                const QuadKey c = source(dt.key, up, cv, cu);
                if (c.level < 0) break;
                if (surfaceZMax(c) > zCap) continue;
                src = c;
                view = cv;
                std::memcpy(uvx, cu, sizeof(cu));
                shownDepth = depthOf(dt.key, src);
                break;
            }
        } else if (zCap == 99 && capDepth > 0 && src.level >= 0 && shownDepth < capDepth) {
            int prev = 0;
            auto pt = transitions_.find(dt.key);
            if (pt != transitions_.end() && pt->second.frame + 1 == frame_) {
                prev = pt->second.held && pt->second.from.level >= 0 ? depthOf(dt.key, pt->second.from) : pt->second.depth;
            } else {
                bool related = false;
                for (int up = 1; up <= 8 && up <= dt.key.level; ++up) {
                    auto a = transitions_.find({dt.key.level - up, dt.key.x >> up, dt.key.y >> up});
                    if (a == transitions_.end() || a->second.frame + 1 != frame_) continue;
                    const QuadKey shown = a->second.held && a->second.from.level >= 0 ? a->second.from : a->second.src;
                    prev = depthOf(dt.key, shown);
                    related = true;
                    break;
                }
                for (int i = 0; i < 4 && !related; ++i) {
                    auto c = transitions_.find({dt.key.level + 1, dt.key.x * 2 + (i & 1), dt.key.y * 2 + (i >> 1)});
                    related = c != transitions_.end() && c->second.frame + 1 == frame_;
                }
                if (!related) prev = 99;
            }
            const int target = std::min(capDepth, prev);
            for (int up = dt.key.level - src.level + 1; shownDepth < target && up <= dt.key.level; ++up) {
                sg_view cv{};
                float cu[4];
                const QuadKey c = source(dt.key, up, cv, cu);
                if (c.level < 0) break;
                const int cd = depthOf(dt.key, c);
                if (cd > target) break;
                if (cd <= shownDepth) continue;
                src = c;
                view = cv;
                std::memcpy(uvx, cu, sizeof(cu));
                shownDepth = cd;
            }
        }
        auto own = surface_.find(dt.key);
        if (own == surface_.end() || !own->second.complete) ++incomplete;
        if (src.level >= 0 && (uniformShare_ > 0.0 ? expecting(dt.key) : (src.level < dt.key.level || shownDepth >= 1))) ++waitingNow;
        sg_view view2 = view;
        float uvx2[4] = {uvx[0], uvx[1], uvx[2], uvx[3]};
        float blend = 1.0f;
        int shownVersion = -1;
        if (src.level >= 0) {
            auto shown = surface_.find(src);
            if (shown != surface_.end()) shownVersion = shown->second.version;
        }
        bool fadingFrom = false;
        int fromVersion = -1;
        auto fadeTexture = [&](const QuadKey& tile, const QuadKey& f, int version, sg_view& out, float uv[4], double progress = 0.0) {
            uv[0] = uv[1] = 1.0f;
            uv[2] = uv[3] = 0.0f;
            if (f.level == -2) {
                out = fallbackView_;
                backdropUv(tile, out, uv);
                return true;
            }
            const int k = tile.level - f.level;
            if (f.level < 0 || k < 0 || (tile.x >> k) != f.x || (tile.y >> k) != f.y) return false;
            out = {};
            auto fs = surface_.find(f);
            if (fs != surface_.end() && fs->second.version == version) {
                out = fs->second.texture;
                fs->second.used = frame_;
                fs->second.shown = frame_;
            } else {
                auto rs = retiring_.find(f);
                if (rs == retiring_.end()) return false;
                for (const Retired& r : rs->second)
                    if (r.surface.version == version) out = r.surface.texture;
                if (!out.id) return false;
                fadeRefs_.push_back({f, version, progress});
            }
            if (!out.id || sg_query_view_state(out) != SG_RESOURCESTATE_VALID) return false;
            const double sc = 1.0 / static_cast<double>(1 << k);
            const int rx = tile.x - (f.x << k), ry = tile.y - (f.y << k);
            uv[0] = static_cast<float>(sc);
            uv[1] = static_cast<float>(sc);
            uv[2] = static_cast<float>(rx * sc);
            uv[3] = static_cast<float>(((1 << k) - 1 - ry) * sc);
            return true;
        };
        Transition& tr = transitions_[dt.key];
        const bool continuing = tr.frame + 1 == frame_;
        sg_view fv{};
        float fu[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        auto improving = [&](const QuadKey& from, int fromVer) {
            if (from.level == -2) return true;
            if (from.level < 0 || src.level < 0) return false;
            if (fromVer >= 0 && fromVer < switchVersion_ && shownVersion >= switchVersion_) return false;
            return from.level < src.level || (from == src && fromVer != shownVersion);
        };
        auto hold = [&]() {
            const bool better = improving(tr.from, tr.fromVersion);
            const bool switched = tr.fromVersion >= 0 && tr.fromVersion < switchVersion_ && shownVersion >= switchVersion_;
            const bool coarsen = !better && zCap < 99 && tr.from.level >= 0;
            if (revealHold_ <= 0.0 || revealOpen_ || switched || coarsen || (!better && tr.from.level < 0)) {
                tr.held = false;
                tr.start = coarsen && !revealOpen_ && tnow - coarsenAt_ < kFadeSeconds ? coarsenAt_ : tnow;
                tr.why = revealOpen_ ? 'g' : coarsen ? 'c' : !better ? 'd' : 'o';
                return;
            }
            if (!tr.held) tr.heldSince = tnow;
            tr.held = true;
            tr.start = -1.0;
        };
        if (continuing) {
            if (!(tr.src == src) || tr.version != shownVersion) {
                const QuadKey showing = tr.held && tr.from.level >= 0 ? tr.from : tr.src;
                if (zCap < 99 && showing.level >= 0 && surfaceZMax(showing) > zCap && surfaceZMax(showing) > surfaceZMax(src)) {
                    if (!tr.held) {
                        tr.from = tr.src;
                        tr.fromVersion = tr.version;
                    }
                    tr.held = false;
                    tr.finishing = false;
                    tr.start = tnow - coarsenAt_ < kFadeSeconds ? coarsenAt_ : tnow;
                    tr.why = 'c';
                } else if (!tr.held && tr.start >= 0.0 && tnow - tr.start < kFadeSeconds && tr.src.level >= 0) {
                    tr.finishing = true;
                } else if (!tr.held) {
                    tr.finishing = false;
                    const bool settledBefore = tr.start < 0.0 || tnow - tr.start >= kFadeSeconds;
                    tr.from = tr.src.level >= 0 ? tr.src : QuadKey{-2, 0, 0};
                    tr.fromVersion = tr.version;
                    if (settledBefore) {
                        hold();
                    } else {
                        tr.start = tnow;
                        tr.why = 'f';
                    }
                }
            } else {
                tr.finishing = false;
            }
        } else {
            tr.finishing = false;
            tr.from = {-1, 0, 0};
            tr.start = -1.0;
            tr.held = false;
            for (int up = 1; up <= 8 && up <= dt.key.level; ++up) {
                auto a = transitions_.find({dt.key.level - up, dt.key.x >> up, dt.key.y >> up});
                if (a == transitions_.end() || a->second.frame + 1 != frame_) continue;
                const bool ancestorHeld = a->second.held && a->second.from.level != -1;
                const QuadKey shown = ancestorHeld ? a->second.from : a->second.src;
                const int shownVer = ancestorHeld ? a->second.fromVersion : a->second.version;
                if (!(shown == src) || shownVer != shownVersion) {
                    tr.from = shown.level >= 0 ? shown : QuadKey{-2, 0, 0};
                    tr.fromVersion = shownVer;
                    hold();
                    if (ancestorHeld && tr.held) tr.heldSince = std::min(tr.heldSince, a->second.heldSince);
                }
                break;
            }
        }
        if (tr.held && revealOpen_) {
            tr.held = false;
            tr.start = tnow;
            tr.why = 'g';
        }
        if (tr.held) {
            if (src.level >= 0 && fadeTexture(dt.key, tr.from, tr.fromVersion, fv, fu)) {
                ++heldNow;
                oldestHeld = std::min(oldestHeld, tr.heldSince);
            } else {
                tr.held = false;
                tr.start = tnow;
                tr.why = 'l';
            }
        }
        if (tr.finishing && !fadeTexture(dt.key, tr.src, tr.version, view, uvx, (tnow - tr.start) / kFadeSeconds)) tr.finishing = false;
        if (!tr.finishing) {
            tr.src = src;
            tr.version = shownVersion;
        }
        tr.frame = frame_;
        tr.depth = shownDepth;
        if (dt.merge.level < 0 && tr.merge.level >= 0) {
            if (tr.mergeFrame + 1 == frame_) {
                tr.held = false;
                tr.from = tr.merge;
                tr.fromVersion = tr.mergeVersion;
                tr.start = tnow - (1.0 - tr.mergeT) * kFadeSeconds;
                tr.why = 'm';
            }
            tr.merge = {-1, 0, 0};
        }
        if ((frozenNow || stepInstant_) && tr.start >= 0.0 && tnow - tr.start < kFadeSeconds) tr.start = tnow - kFadeSeconds * 1.01;
        if (zCap < 99 && tr.from.level >= 0 && surfaceZMax(tr.from) > zCap) {
            if (tr.start >= 0.0 && tnow - tr.start < kFadeSeconds) tr.start = tnow - kFadeSeconds * 1.01;
            if (tr.held) {
                tr.held = false;
                tr.start = tnow - kFadeSeconds * 1.01;
                tr.why = 'c';
            }
        }
        if (lines3d_ && labelsOn_) lineTiles.push_back({dt.key, tr.held && tr.from.level >= 0 ? tr.from : src});
        if (visualLog_) {
            const QuadKey shownKey = tr.held && tr.from.level >= 0 ? tr.from : src;
            auto ss = shownKey.level >= 0 ? surface_.find(shownKey) : surface_.end();
            const int z = ss == surface_.end() ? -1 : ss->second.complete ? std::max(ss->second.sourceZ, ss->second.imageryZ) : ss->second.sourceZ;
            ++levelMix[dt.key.level * 100 + std::clamp(z, -1, 98) + 1];
        }
        if (tr.held) {
            view2 = fv;
            std::memcpy(uvx2, fu, sizeof(fu));
            blend = 0.0f;
            fadingFrom = true;
            fromVersion = tr.fromVersion;
        } else if (tr.start >= 0.0) {
            const double t = (tnow - tr.start) / kFadeSeconds;
            if (t < 1.0 && src.level >= 0 && fadeTexture(dt.key, tr.from, tr.fromVersion, fv, fu, t)) {
                view2 = fv;
                std::memcpy(uvx2, fu, sizeof(fu));
                blend = static_cast<float>(t * t * (3 - 2 * t));
                fading = true;
                ++fadeKinds[0];
                fadingFrom = true;
                fromVersion = tr.fromVersion;
            } else {
                if (t < 1.0 && src.level >= 0) {
                    ++fadeMisses_;
                    if (visualLog_) {
                        auto rs = retiring_.find(tr.from);
                        auto fs = surface_.find(tr.from);
                        std::fprintf(visualLog_, "miss frame %llu tile L%d/%d/%d from L%d/%d/%d v%d surface %s v%d retired %zu count %zu\n",
                                     static_cast<unsigned long long>(frame_), dt.key.level, dt.key.x, dt.key.y, tr.from.level, tr.from.x, tr.from.y,
                                     tr.fromVersion, fs == surface_.end() ? "gone" : "here", fs == surface_.end() ? -9 : fs->second.version,
                                     rs == retiring_.end() ? size_t(0) : rs->second.size(), retiredCount_);
                    }
                }
                tr.start = -1.0;
            }
        }
        float mergeMix = 0.0f;
        sg_view mergeView{};
        float mergeUv[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        const bool mergeContinuing = tr.mergeFrame + 1 == frame_;
        const float mergeBefore = mergeContinuing ? tr.mergeT : 0.0f;
        if (dt.merge.level >= 0 && src.level >= 0) {
            auto target = surface_.find(dt.merge);
            if (target != surface_.end() && target->second.version >= 0 &&
                fadeTexture(dt.key, dt.merge, target->second.version, mergeView, mergeUv)) {
                const double m = std::clamp(static_cast<double>(dt.mergeT), 0.0, 1.0);
                mergeMix = static_cast<float>(m * m * (3.0 - 2.0 * m));
                tr.merge = dt.merge;
                tr.mergeVersion = target->second.version;
                tr.mergeT = dt.mergeT;
                tr.mergeFrame = frame_;
            }
        }
        if (visualLog_ && zCap < 99 && src.level >= 0) {
            const int zs = surfaceZMax(src);
            int reason = zs > zCap ? 0 : -1;
            if (reason < 0 && fadingFrom && tr.from.level >= 0 && surfaceZMax(tr.from) > zCap) reason = tr.held ? 1 : 2;
            if (reason < 0 && mergeMix > 0.0f && surfaceZMax(dt.merge) > zCap) reason = 3;
            if (reason >= 0) {
                ++capFail[reason];
                if (capExample.level < 0) {
                    capExample = dt.key;
                    capExampleZ = reason == 0 ? zs : reason == 3 ? zOf(dt.merge) : zOf(tr.from);
                }
            }
        }
        int coarser[4];
        double skirtLen = seamSkirt(dt.key, tt, drawnNow, coarser);
        float deficit[4] = {0, 0, 0, 0};
        float mine = static_cast<float>(dt.key.level);
        float lowest = mine;
        if (lodBlend_ && src.level == dt.key.level && dt.merge.level < 0) {
            mine = displayLevel(tr);
            lowest = mine;
            for (int e = 0; e < 4; ++e) {
                const float n = neighbourLevel(dt.key, e);
                if (n >= 0.0f && n < mine - 0.05f) {
                    deficit[e] = mine - n;
                    lowest = std::min(lowest, n);
                }
            }
        }
        const int lodUp = lowest < mine ? std::clamp(dt.key.level - static_cast<int>(std::floor(lowest)), 1, dt.key.level) : 0;
        float lodEdges[4] = {0, 0, 0, 0};
        float edgeTarget[4] = {0, 0, 0, 0};
        sg_view view3 = view;
        float uvx3[4] = {uvx[0], uvx[1], uvx[2], uvx[3]};
        bool morphing = false;
        bool spatial = false;
        const bool edgeBlend = lodUp > 0 || tr.edge[0] > 0.0f || tr.edge[1] > 0.0f || tr.edge[2] > 0.0f || tr.edge[3] > 0.0f;
        const bool morphBlend = lodMorph_ > 0.0 && dt.key.level > kMinLevel;
        if (src.level == dt.key.level && (edgeBlend || morphBlend)) {
            const int up = lodUp > 0 ? lodUp : std::max(1, tr.edgeUp);
            const QuadKey want{dt.key.level - up, dt.key.x >> up, dt.key.y >> up};
            sg_view pv = view;
            float pu[4] = {uvx[0], uvx[1], uvx[2], uvx[3]};
            QuadKey got = source(dt.key, up, pv, pu);
            auto ancestor = got.level >= 0 ? surface_.find(got) : surface_.end();
            const bool ancestorCurrent = ancestor != surface_.end() && ancestor->second.version >= switchVersion_;
            const bool sameGeneration = ancestor != surface_.end() && ancestorCurrent == (shownVersion >= switchVersion_) &&
                                        (!fadingFrom || fromVersion < 0 || ancestorCurrent == (fromVersion >= switchVersion_));
            const bool sameLayers = ancestor != surface_.end() && own != surface_.end() &&
                                    ancestor->second.overlaysOmitted == own->second.overlaysOmitted;
            if (got.level >= 0 && got.level < src.level && sameGeneration && sameLayers) {
                view3 = pv;
                std::memcpy(uvx3, pu, sizeof(pu));
                const float span = std::max(mine - static_cast<float>(got.level), 0.25f);
                for (int e = 0; e < 4; ++e) edgeTarget[e] = std::clamp(deficit[e] / span, 0.0f, 1.0f);
                tr.edgeUp = dt.key.level - got.level;
                morphing = morphBlend && got.level <= want.level && (ancestor->second.complete || ancestor->second.presentable);
                spatial = true;
            } else {
                for (float& e : tr.edge) e = 0.0f;
            }
            if ((got.level != want.level || !sameLayers) && blendParents_.size() < 256) blendParents_.push_back({want, dt.distance * 2.0});
        }
        if (visualStats_) {
            const double area = screenFraction(cam, dt.key, tt);
            visCover += area;
            if (shownDepth >= 1) visBlur += area;
            if (shownDepth >= 2) visBlur2 += area;
            visDepth += area * shownDepth;
            const bool faded = blend < 0.999f || dt.merge.level >= 0;
            if (faded) visFade += area;
            if (tr.start == tnow && !tr.held) {
                visReveal += area;
                ++visReveals;
                if (tr.why != 'g') {
                    visUngated += area;
                    ++visUngatedTiles;
                    if (tr.why == 'd') ++visDown;
                    if (tr.why == 'f') ++visRefade;
                    if (tr.why == 'l') ++visLost;
                }
                if (visualLog_)
                    std::fprintf(visualLog_, "fade frame %llu t %.3f tile L%d/%d/%d why %c from L%d v%d to L%d v%d depth %d area %.4f\n",
                                 static_cast<unsigned long long>(frame_), tnow, dt.key.level, dt.key.x, dt.key.y, tr.why ? tr.why : '?', tr.from.level,
                                 tr.fromVersion, src.level, shownVersion, shownDepth, area);
            }
            if (dt.merge.level >= 0 && dt.mergeT > 0.0f && mergeBefore <= 0.0f) {
                visMergeStart += area;
                ++visMergeStarts;
                if (visualLog_)
                    std::fprintf(visualLog_, "merge frame %llu t %.3f tile L%d/%d/%d into L%d area %.4f\n", static_cast<unsigned long long>(frame_), tnow,
                                 dt.key.level, dt.key.x, dt.key.y, dt.merge.level, area);
            }
            const bool held = snapshotShown_ && snapshotFade_ < 0.0;
            const bool merged = dt.merge.level >= 0 && mergeMix >= 0.5f;
            const QuadKey shownKey = merged ? dt.merge : src;
            const int shownVer = merged ? tr.mergeVersion : shownVersion;
            Shown& sh = shown_[dt.key];
            if (sh.frame + 1 == frame_) {
                if (!held && !faded && (!(sh.src == shownKey) || sh.version != shownVer)) {
                    visPop += area;
                    ++visPops;
                    if (visualLog_)
                        std::fprintf(visualLog_, "pop frame %llu tile L%d/%d/%d src L%d v%d -> L%d v%d merge L%d t %.2f area %.4f\n",
                                     static_cast<unsigned long long>(frame_), dt.key.level, dt.key.x, dt.key.y, sh.src.level, sh.version, src.level,
                                     shownVersion, dt.merge.level, dt.mergeT, area);
                }
                if (!held && sh.terrain >= 0 && sh.terrain != tt.version) visTerrain += area;
            } else if (!held && !faded) {
                int before = -1;
                for (int up = 1; up <= 8 && up <= dt.key.level && before < 0; ++up) {
                    auto a = shown_.find({dt.key.level - up, dt.key.x >> up, dt.key.y >> up});
                    if (a != shown_.end() && a->second.frame + 1 == frame_) before = a->second.src.level;
                }
                if (before >= 0 && shownKey.level > before) {
                    visPop += area;
                    ++visPops;
                    if (visualLog_)
                        std::fprintf(visualLog_, "newpop frame %llu tile L%d/%d/%d before L%d now L%d v%d kind %d area %.4f\n",
                                     static_cast<unsigned long long>(frame_), dt.key.level, dt.key.x, dt.key.y, before, src.level, shownVersion, dt.kind, area);
                } else if (before < 0) {
                    for (int i = 0; i < 4; ++i) {
                        auto c = shown_.find({dt.key.level + 1, dt.key.x * 2 + (i & 1), dt.key.y * 2 + (i >> 1)});
                        if (c != shown_.end() && c->second.frame + 1 == frame_ && c->second.src.level > shownKey.level) {
                            visMerge += area;
                            break;
                        }
                    }
                }
            }
            sh.src = shownKey;
            sh.version = shownVer;
            sh.terrain = tt.version;
            sh.frame = frame_;
        }
        if (sg_query_buffer_state(tt.vbuf) != SG_RESOURCESTATE_VALID) {
            ++invalidResources_;
            continue;
        }
        if (sg_query_view_state(view) != SG_RESOURCESTATE_VALID) {
            ++invalidResources_;
            view = fallbackView_;
        }
        if (sg_query_view_state(view2) != SG_RESOURCESTATE_VALID) {
            ++invalidResources_;
            view2 = view;
        }
        {
            const double step = continuing && tr.edgeTime >= 0.0 && !frozenNow && !stepInstant_ ? std::clamp(tnow - tr.edgeTime, 0.0, 0.1) / kFadeSeconds : 1.0;
            tr.edgeTime = tnow;
            bool any = false;
            for (int e = 0; e < 4; ++e) {
                const float target = spatial ? edgeTarget[e] : 0.0f;
                tr.edge[e] = target > tr.edge[e] ? std::min(target, tr.edge[e] + static_cast<float>(step))
                                                 : std::max(target, tr.edge[e] - static_cast<float>(step));
                lodEdges[e] = tr.edge[e];
                any = any || tr.edge[e] > 0.0f;
                if (tr.edge[e] != target) {
                    fading = true;
                    ++fadeKinds[1];
                }
            }
            if (!any && !morphing) spatial = false;
        }
        if (mergeMix > 0.0f && mergeView.id) {
            view3 = mergeView;
            std::memcpy(uvx3, mergeUv, sizeof(mergeUv));
            morphing = false;
            spatial = false;
            for (float& e : lodEdges) e = 0.0f;
        }
        if (sg_query_view_state(view3) != SG_RESOURCESTATE_VALID) {
            ++invalidResources_;
            view3 = view;
            mergeMix = 0.0f;
            morphing = false;
            spatial = false;
            for (float& e : lodEdges) e = 0.0f;
        }
        Mat4 mvp = dc.viewProjRel() * translation(tt.center - dc.eye());
        globe_vs_params_t vsp{};
        toFloat(mvp, vsp.mvp);
        std::memcpy(vsp.uv_xform, uvx, sizeof(uvx));
        std::memcpy(vsp.uv_xform2, uvx2, sizeof(uvx2));
        std::memcpy(vsp.uv_xform3, uvx3, sizeof(uvx3));
        vsp.skirt[0] = static_cast<float>(skirtLen);
        std::memcpy(vsp.edges, lodEdges, sizeof(lodEdges));
        vsp.skirt[1] = static_cast<float>(kPosts * kPosts);
        vsp.center[0] = static_cast<float>(tt.center.x);
        vsp.center[1] = static_cast<float>(tt.center.y);
        vsp.center[2] = static_cast<float>(tt.center.z);
        if (spatial) {
            auto& since = morphSince_[dt.key];
            if (since.second + 1 < frame_) since.first = fadingFrom ? tnow - 1.0 : tnow;
            if (frozenNow || stepInstant_) since.first = std::min(since.first, tnow - 1.0);
            since.second = frame_;
            const double t = (tnow - since.first) / 0.3;
            vsp.morph[3] = t >= 1.0 ? 1.0f : static_cast<float>(t * t * (3 - 2 * t));
            if (t < 1.0) {
                fading = true;
                ++fadeKinds[2];
            }
        }
        if (morphing) {
            const Vec3 rel = tt.center - eye_;
            vsp.eyerel[0] = static_cast<float>(rel.x);
            vsp.eyerel[1] = static_cast<float>(rel.y);
            vsp.eyerel[2] = static_cast<float>(rel.z);
            auto sphere = spheres_.find(dt.key);
            vsp.eyerel[3] = uniformMix_ >= 1.0 ? static_cast<float>(-uniformDist_)
                            : sphere != spheres_.end() ? static_cast<float>(sphere->second.radius) : 0.0f;
            const double texel = spanDeg(dt.key.level) * kDegToRad * kWgs84A / kSurfaceSize;
            const double threshold = std::max(1.0, dpiScale_ * 0.6) / std::max(imageryScale_, 0.1);
            vsp.morph[0] = static_cast<float>(texel / (cam.pixelSizeAt(1.0) * threshold));
            vsp.morph[1] = static_cast<float>(0.5 * (1.0 + lodMorph_));
            vsp.morph[2] = static_cast<float>(0.5 * lodMorph_);
        }
        gfs.flags[2] = blend;
        gfs.heat[3] = mergeMix;
        if (views && views->size() > 1) terrainDraws.push_back({tt.vbuf, view, view2, view3, vsp, tt.center, blend, mergeMix});
        sg_apply_uniforms(UB_globe_fs_params, SG_RANGE(gfs));
        sg_bindings bind{};
        bind.vertex_buffers[0] = tt.vbuf;
        bind.index_buffer = indexBuffer_;
        bind.views[VIEW_tex] = view;
        bind.views[VIEW_tex2] = view2;
        bind.views[VIEW_tex3] = view3;
        bind.samplers[SMP_smp] = sampler_;
        sg_apply_bindings(&bind);
        sg_apply_uniforms(UB_globe_vs_params, SG_RANGE(vsp));
        sg_draw(0, xray ? kGrid * kGrid * 6 : kIndexCount, 1);
    }
    if (visualLog_ && capFail[0] + capFail[1] + capFail[2] + capFail[3] > 0) {
        std::string chain;
        if (capExample.level >= 0)
            for (int up = 0; up <= capExample.level && up <= 8; ++up) {
                const QuadKey a{capExample.level - up, capExample.x >> up, capExample.y >> up};
                auto st = surface_.find(a);
                char buf[48];
                std::snprintf(buf, sizeof(buf), " L%d:%s%d", a.level,
                              st == surface_.end() ? "-" : st->second.version < 0 ? "0" : st->second.complete ? "C" : st->second.presentable ? "p" : "i",
                              st == surface_.end() ? -1 : zOf(a));
                chain += buf;
            }
        std::fprintf(visualLog_, "capfail frame %llu t %.3f cap %d src %d held %d fade %d merge %d example L%d/%d/%d z %d chain%s\n",
                     static_cast<unsigned long long>(frame_), tnow, zCap, capFail[0], capFail[1], capFail[2], capFail[3], capExample.level, capExample.x,
                     capExample.y, capExampleZ, chain.c_str());
    }
    const GeoRect viewArea = viewFootprint(dc);
    const GeoPoint eyeGeo = ecefToGeodetic(dc.eye());
    int roadsWaiting = 0;
    if (lines3d_ && labelsOn_ && !xray && !lineTiles.empty()) {
        const int lineVersion = elevation_.version() + terrainEpoch_ * 1000000;
        std::vector<GeoRect> rects;
        std::vector<ViewRect> borderRects;
        rects.reserve(lineTiles.size());
        for (const auto& lt : lineTiles) {
            const Bounds b = bounds(lt.first);
            rects.push_back({b.west, b.south, b.east, b.north});
            clipToView(rects.back(), viewArea, dc, eyeGeo, borderRects);
        }
        labels_.warmLines3D();
        bordersReady_ = labels_.drawBorders3D(dc, ground, dpiScale_, borderRects, lineVersion);
        std::unordered_map<TileId, std::vector<size_t>, TileIdHash> users;
        for (size_t i = 0; i < lineTiles.size(); ++i) {
            auto s = lineTiles[i].second.level >= 0 ? surface_.find(lineTiles[i].second) : surface_.end();
            if (s == surface_.end()) continue;
            for (const TileId& r : s->second.roadTiles) users[r].push_back(i);
        }
        struct RoadDraw {
            const ImageryTexture* tex;
            Vec3 origin;
            int first;
            int count;
            float clip[4];
        };
        GeoRect seen{1e9, 1e9, -1e9, -1e9};
        for (const GeoRect& r : rects) {
            seen.west = std::min(seen.west, r.west);
            seen.south = std::min(seen.south, r.south);
            seen.east = std::max(seen.east, r.east);
            seen.north = std::max(seen.north, r.north);
        }
        const int roadRanksShown = LabelLayer::visibleRoadRanks(dc, dpiScale_);
        auto addChunks = [&](std::vector<RoadDraw>& outDraws, const ImageryTexture& t, const GeoRect& cull, const float clip[4], bool solidOnly) {
            int runFirst = -1, runEnd = -1;
            auto flush = [&]() {
                if (runFirst >= 0 && runEnd > runFirst) {
                    RoadDraw d{&t, t.roadsOrigin, runFirst, runEnd - runFirst, {clip[0], clip[1], clip[2], clip[3]}};
                    outDraws.push_back(d);
                }
                runFirst = runEnd = -1;
            };
            for (int c = 0; c < 32; ++c) {
                const int a = t.roadChunk[c], e = a + t.roadRanks[c][std::clamp(roadRanksShown, 0, 12)];
                const GeoRect& bx = t.roadBox[c];
                const bool visible = e > a && !(solidOnly && c >= 16) && bx.west <= cull.east && cull.west <= bx.east && bx.south <= cull.north &&
                                     cull.south <= bx.north;
                if (!visible || (runFirst >= 0 && runEnd != a)) flush();
                if (!visible) continue;
                if (runFirst < 0) runFirst = a;
                runEnd = e;
            }
            flush();
        };
        std::vector<RoadDraw> roadDraws;
        const auto buildStart = std::chrono::steady_clock::now();
        bool forcedBuild = false;
        for (auto q = roadBuildQueue_.begin(); q != roadBuildQueue_.end();) {
            auto it = imagery_.find(*q);
            if (it == imagery_.end() || !it->second.vector || it->second.vector->roads.empty() || it->second.roads3dVersion >= 0) {
                q = roadBuildQueue_.erase(q);
                continue;
            }
            if (std::chrono::steady_clock::now() - buildStart >= std::chrono::milliseconds(2) && forcedBuild) break;
            forcedBuild = true;
            it->second.roads3dBuilt = tnow;
            buildRoads3D(it->second, lineVersion);
            q = roadBuildQueue_.erase(q);
        }
        auto overlaps = [](const GeoRect& a, const GeoRect& b) {
            const double eps = 1e-9;
            return a.west < b.east - eps && b.west < a.east - eps && a.south < b.north - eps && b.south < a.north - eps;
        };
        for (auto& [rid, list] : users) {
            auto it = imagery_.find(rid);
            if (it == imagery_.end() || !it->second.vector) continue;
            ImageryTexture& t = it->second;
            t.used = frame_;
            const bool due = t.roads3dVersion != lineVersion && (!t.roads3d.id || tnow - t.roads3dBuilt > 1.0);
            const bool allowed = std::chrono::steady_clock::now() - buildStart < std::chrono::milliseconds(2) || (!t.roads3d.id && !forcedBuild);
            if (due && allowed) {
                if (!t.roads3d.id) forcedBuild = true;
                t.roads3dBuilt = tnow;
                buildRoads3D(t, lineVersion);
            }
            if (t.roads3dVersion < 0) ++roadsWaiting;
            if (!t.roads3d.id || t.roads3dCount <= 0) continue;
            const VectorTile& vt = *t.vector;
            const double n = std::ldexp(1.0, vt.z);
            auto latOf = [&](double yy) { return std::atan(std::sinh(3.14159265358979323846 * (1.0 - 2.0 * yy / n))) * kRadToDeg; };
            const GeoRect area{vt.x / n * 360.0 - 180.0, latOf(vt.y + 1.0), (vt.x + 1.0) / n * 360.0 - 180.0, latOf(vt.y)};
            std::vector<bool> mine(lineTiles.size(), false);
            for (size_t i : list) mine[i] = true;
            bool shared = false;
            for (size_t j = 0; j < lineTiles.size() && !shared; ++j)
                if (!mine[j] && overlaps(rects[j], area)) shared = true;
            const bool solidOnly = buildingsOn_ && cam.gsd < 6.0;
            if (!shared) {
                const float none[4] = {1.0f, 0.0f, -1.0f, 0.0f};
                addChunks(roadDraws, t, seen, none, solidOnly);
                continue;
            }
            GeoRect group{1e9, 1e9, -1e9, -1e9};
            for (size_t i : list) {
                group.west = std::min(group.west, rects[i].west);
                group.south = std::min(group.south, rects[i].south);
                group.east = std::max(group.east, rects[i].east);
                group.north = std::max(group.north, rects[i].north);
            }
            bool groupClear = true;
            for (size_t j = 0; j < lineTiles.size() && groupClear; ++j)
                if (!mine[j] && overlaps(rects[j], group)) groupClear = false;
            if (groupClear) {
                const float clip[4] = {static_cast<float>(group.west - vt.originLon), static_cast<float>(group.south - vt.originLat),
                                       static_cast<float>(group.east - vt.originLon), static_cast<float>(group.north - vt.originLat)};
                addChunks(roadDraws, t, group, clip, solidOnly);
                continue;
            }
            for (size_t i : list) {
                const GeoRect& r = rects[i];
                const float clip[4] = {static_cast<float>(r.west - vt.originLon), static_cast<float>(r.south - vt.originLat),
                                       static_cast<float>(r.east - vt.originLon), static_cast<float>(r.north - vt.originLat)};
                addChunks(roadDraws, t, r, clip, solidOnly);
            }
        }
        if (visualLog_ && frame_ % 30 == 0) {
            size_t withRoads = 0;
            for (const auto& lt : lineTiles) {
                auto s = lt.second.level >= 0 ? surface_.find(lt.second) : surface_.end();
                if (s != surface_.end() && !s->second.roadTiles.empty()) ++withRoads;
            }
            std::fprintf(visualLog_, "lines frame %llu t %.3f tiles %zu withroads %zu roadtiles %zu draws %zu\n", static_cast<unsigned long long>(frame_), tnow,
                         lineTiles.size(), withRoads, users.size(), roadDraws.size());
        }
        labels_.beginRoads3D(dc, dpiScale_);
        for (int pass = 0; pass < 2; ++pass)
            for (const RoadDraw& d : roadDraws) labels_.drawRoads3D(d.tex->roads3d, d.first, d.count, d.origin, d.clip, pass);
    }
    auto snapshotAlpha = [&]() {
        float alpha = 1.0f;
        if (snapshotFade_ >= 0.0) {
            const double t = std::clamp((tnow - snapshotFade_) / snapshotFadeLength_, 0.0, 1.0);
            alpha = static_cast<float>(1.0 - t * t * (3.0 - 2.0 * t));
            if (t >= 1.0) snapshotShown_ = false;
        }
        return alpha;
    };
    if (skyVisible_ && !xray) drawSky(dc);
    if (!xray) drawObjects();
    auto drawSettle = [&]() {
        if (capture || !snapshotShown_ || !settleSnapshot_) return;
        bool steered = false;
        for (int i = 0; i < 5; ++i) steered = steered || std::abs(motionKey_[i] - snapshotKey_[i]) > 1.0e-9;
        drawSnapshot(snapshotAlpha(), steered ? &dc : nullptr);
    };
    profileSub(0);
    std::vector<FeatureLayer::ExternalLabel> vectorLabels;
    if (!vectorLayers_.empty() && !labelsOn_) {
        std::unordered_set<TileId, TileIdHash> seen;
        for (const auto& dt : tiles) {
            Bounds b = bounds(dt.key);
            for (int l = 0; l < static_cast<int>(matrices_.size()) && l < static_cast<int>(vectorLayers_.size()); ++l) {
                if (!vectorLayers_[l] || l == overlayLayer_) continue;
                const TileMatrix& m = matrices_[l];
                double target = (b.north - b.south) / kSurfaceSize * m.unitsPerDegree();
                int z = m.bestLevel(target);
                if (z < 0) continue;
                double clat = std::clamp(0.5 * (b.south + b.north), -84.0, 84.0), clon = 0.5 * (b.west + b.east);
                for (int zz = z; zz >= 0; --zz) {
                    if (!m.hasLevel(zz)) continue;
                    TileId id{l, zz, static_cast<int>(std::floor((projectX(m.srid, clon) - m.originX) / m.spanX(zz))),
                              static_cast<int>(std::floor((m.originY - projectY(m.srid, clat)) / m.spanY(zz)))};
                    auto it = imagery_.find(id);
                    if (it == imagery_.end() || !it->second.vector) continue;
                    if (!seen.insert(id).second) break;
                    for (const VectorLabel& vl : it->second.vector->labels)
                        vectorLabels.push_back({vl.lat, vl.lon, vl.text, vl.priority, vl.color, vl.size, true});
                    break;
                }
            }
        }
    }
    std::vector<ViewRect> visibleRects;
    std::vector<const VectorTile*> overlayTiles;
    GroundFn labelAbove;
    if (labelsOn_ || contours_) {
        visibleRects.reserve(tiles.size());
        std::unordered_set<const VectorTile*> seenOverlay;
        for (const auto& dt : tiles) {
            Bounds b = bounds(dt.key);
            clipToView({b.west, b.south, b.east, b.north}, viewArea, dc, eyeGeo, visibleRects);
            if (overlayLayer_ < 0 || overlayLayer_ >= static_cast<int>(matrices_.size())) continue;
            const TileMatrix& m = matrices_[overlayLayer_];
            const double target = (b.north - b.south) / kSurfaceSize * m.unitsPerDegree();
            const int z = m.bestLevel(target);
            if (z < 0) continue;
            const double maxLat = maxLatitude(m.srid);
            const double s = std::max(b.south, -maxLat), n = std::min(b.north, maxLat);
            if (s >= n) continue;
            const int deep = std::min(z + 3, m.maxZoom);
            for (const TileRange& r : regionRanges(m, {b.west, s, b.east, n}, deep, deep))
                for (int ty = r.y0; ty <= r.y1; ++ty)
                    for (int tx = r.x0; tx <= r.x1; ++tx)
                        for (int up = 0; up <= 8 && deep - up >= 0; ++up) {
                            const TileId lid{overlayLayer_, deep - up, tx >> up, ty >> up};
                            auto it = imagery_.find(lid);
                            if (it == imagery_.end() || !it->second.vector) continue;
                            labelTileUse_[lid] = frame_;
                            it->second.used = frame_;
                            if (seenOverlay.insert(it->second.vector.get()).second) overlayTiles.push_back(it->second.vector.get());
                            break;
                        }
        }
        for (auto it = labelTileUse_.begin(); it != labelTileUse_.end();) {
            auto tex = imagery_.find(it->first);
            if (it->second + 30 < frame_ || it->first.layer != overlayLayer_ || tex == imagery_.end() || !tex->second.vector) {
                it = labelTileUse_.erase(it);
                continue;
            }
            tex->second.used = frame_;
            if (it->second != frame_ && seenOverlay.insert(tex->second.vector.get()).second) overlayTiles.push_back(tex->second.vector.get());
            ++it;
        }
        if (contours_ && contours_->marks) overlayTiles.push_back(contours_->marks.get());
        if (buildingsOn_) labelAbove = [this](double lat, double lon) { return buildings_.heightAt(lat, lon); };
    }
    if (labelsOn_ || contours_) labels_.drawLabels(dc, ground, dpiScale_, visibleRects, overlayTiles, 0, labelsOn_, labelAbove);
    profileSub(1);
    drawSettle();
    auto drawTop = [&]() { features_.drawOverlay(dc, ground, dpiScale_, flipSurface_, &vectorLabels); };
    if (!settleCapture) drawTop();
    if (!capture && !views && overlay_) overlay_(width, height);
    profileSub(2);
    sg_end_pass();
    profileSub(3);
    if (capture) {
        sg_pass show{};
        show.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        show.action.colors[0].clear_value = {clear_[0], clear_[1], clear_[2], 1.0f};
        if (mainSwapchain) show.swapchain = *mainSwapchain;
        if (mainAttachments) show.attachments = *mainAttachments;
        sg_begin_pass(&show);
        drawSnapshot(1.0f);
        if (settleCapture) drawTop();
        if (overlay_) overlay_(width, height);
        sg_end_pass();
        snapshotShown_ = true;
        settleSnapshot_ = settleCapture;
        stepSnapshot_ = stepCapture;
        switchHold_ = switchCapture;
        settleQuiet_ = 0;
        snapshotStart_ = tnow;
        snapshotFade_ = -1.0;
        if (stepCapture) {
            snapshotFade_ = tnow;
            snapshotFadeLength_ = settleMoveFade_;
            if (visualLog_) std::fprintf(visualLog_, "settle frame %llu t %.3f step capture\n", static_cast<unsigned long long>(frame_), tnow);
        }
        if (visualLog_ && settleCapture)
            std::fprintf(visualLog_, "settle frame %llu t %.3f capture held %d waiting %d tiles%s\n", static_cast<unsigned long long>(frame_), tnow,
                         heldPrev_, waitingPrev_, levelMixText().c_str());
        if (visualLog_ && switchCapture) std::fprintf(visualLog_, "settle frame %llu t %.3f switch capture\n", static_cast<unsigned long long>(frame_), tnow);
        snapshotCam_ = cam;
        std::copy(motionKey_, motionKey_ + 5, snapshotKey_);
    }
    if (views)
        for (size_t vi = 1; vi < views->size(); ++vi) {
            const RenderView& v = (*views)[vi];
            const Camera& vc = *v.cam;
            sg_pass pass{};
            pass.action.colors[0].load_action = v.clear ? SG_LOADACTION_CLEAR : SG_LOADACTION_LOAD;
            pass.action.colors[0].clear_value = {clear_[0], clear_[1], clear_[2], 1.0f};
            pass.action.depth.load_action = v.clear ? SG_LOADACTION_CLEAR : SG_LOADACTION_LOAD;
            pass.action.depth.clear_value = 1.0f;
            if (v.swapchain) pass.swapchain = *v.swapchain;
            if (v.attachments) pass.attachments = *v.attachments;
            sg_begin_pass(&pass);
            if (v.width > 0) sg_apply_viewport(v.x, v.y, v.width, v.height, true);
            if (skyVisible_ && xray) drawSky(vc);
            auto objects = [&]() {
                if (buildingsOn_) buildings_.draw(vc, sunDir_, sunOn_);
                models_.draw(vc, ground, true);
                points_.draw(vc, dpiScale_);
                for (auto& [id, t] : tilesets_) t->draw(vc, models_, points_, dpiScale_);
            };
            if (xray) objects();
            sg_apply_pipeline(xray ? globeBlendPipeline_ : globePipeline_);
            for (const TerrainDraw& td : terrainDraws) {
                globe_vs_params_t vsp = td.vsp;
                toFloat(vc.viewProjRel() * translation(td.center - vc.eye()), vsp.mvp);
                gfs.flags[2] = td.blend;
                gfs.heat[3] = td.merge;
                sg_apply_uniforms(UB_globe_fs_params, SG_RANGE(gfs));
                sg_bindings bind{};
                bind.vertex_buffers[0] = td.vbuf;
                bind.index_buffer = indexBuffer_;
                bind.views[VIEW_tex] = td.view;
                bind.views[VIEW_tex2] = td.view2;
                bind.views[VIEW_tex3] = td.view3;
                bind.samplers[SMP_smp] = sampler_;
                sg_apply_bindings(&bind);
                sg_apply_uniforms(UB_globe_vs_params, SG_RANGE(vsp));
                sg_draw(0, xray ? kGrid * kGrid * 6 : kIndexCount, 1);
            }
            if (skyVisible_ && !xray) drawSky(vc);
            if (!xray) objects();
            if (labelsOn_ || contours_)
                labels_.drawLabels(vc, ground, dpiScale_, visibleRects, overlayTiles, static_cast<int>(vi), labelsOn_,
                                   buildingsOn_ ? GroundFn([this](double lat, double lon) { return buildings_.heightAt(lat, lon); }) : GroundFn{});
            features_.drawOverlay(vc, ground, dpiScale_, flipSurface_, &vectorLabels);
            sg_end_pass();
        }

    bool pending = false;
    for (const Need& n : needs_)
        if (!ready(n.key)) {
            pending = true;
            break;
        }
    if (diagnostics_ && prevGsd_ > 0 && maxLevel + 2 < prevMaxLevel_ && std::abs(std::log2(cam.gsd / prevGsd_)) < 1.0)
        explainCollapse(cam, tiles);
    prevMaxLevel_ = maxLevel;
    prevGsd_ = cam.gsd;
    stats_.terrainTiles = static_cast<int>(tiles.size());
    profileMark(6);
    stats_.surfaceRenders = renders;
    stats_.hollowSkipped = hollowSkipped;
    stats_.placeholders = loader_.placeholders();
    stats_.terrainBuilds = builds;
    stats_.imageryTextures = static_cast<int>(imagery_.size());
    stats_.imageryPending = loader_.pending();
    stats_.staleTiles = loader_.staleServed();
    stats_.unreachableHosts = net::hostsDown();
    stats_.networkBytes = static_cast<long long>(net::bytesReceived());
    stats_.networkRequests = static_cast<long long>(net::requestsMade());
    stats_.elevationPending = elevation_.pending();
    stats_.pendingRequests = stats_.imageryPending + stats_.elevationPending;
    stats_.maxLevel = maxLevel;
    stats_.incompleteTiles = incomplete;
    stats_.blankTiles = blank;
    stats_.popArea = visPop;
    stats_.mergeArea = visMerge;
    stats_.terrainPopArea = visTerrain;
    stats_.blurArea = visBlur;
    stats_.blurArea2 = visBlur2;
    stats_.blurDepth = visDepth;
    stats_.fadeArea = visFade;
    stats_.coverArea = visCover;
    stats_.pops = visPops;
    heldPrev_ = heldNow;
    oldestHeldPrev_ = oldestHeld;
    waitingPrev_ = waitingNow;
    stats_.heldTiles = heldNow;
    stats_.reveals = visReveals;
    stats_.revealArea = visReveal;
    stats_.ungated = visUngatedTiles;
    stats_.ungatedArea = visUngated;
    stats_.fadeDown = visDown;
    stats_.fadeRefade = visRefade;
    stats_.fadeLost = visLost;
    stats_.mergeStarts = visMergeStarts;
    stats_.mergeStartArea = visMergeStart;
    stats_.fadeMisses = fadeMisses_;
    stats_.coarseTiles = coarse;
    stats_.coverageOnlyTiles = coverageOnly;
    stats_.retainedTiles = retained;
    stats_.maxFallbackDepth = maxDepth;
    stats_.surfaces = static_cast<int>(surface_.size());
    stats_.freeSurfaces = static_cast<int>(freeSurfaces_.size());
    stats_.invalidResources = invalidResources_;
    stats_.terrainMeshes = static_cast<int>(terrain_.size());
    stats_.gpuMegabytes = (surface_.size() + freeSurfaces_.size()) * (kSurfaceSize * kSurfaceSize * 4.0 * 4.0 / 3.0) / 1048576.0 +
                          imagery_.size() * (256.0 * 256.0 * 4.0) / 1048576.0 + terrain_.size() * (kVertexCount * kStride * 4.0) / 1048576.0;
    {
        const size_t slot = timingCount_ % frameSamples_.size();
        frameSamples_[slot] = timingCount_ > 0 ? static_cast<float>(std::chrono::duration<double, std::milli>(frameStart_ - lastFrameStart_).count()) : 0.0f;
        renderSamples_[slot] = static_cast<float>(elapsedMs());
        ++timingCount_;
        const size_t n = std::min(timingCount_, frameSamples_.size());
        auto percentile = [n](const std::array<float, 240>& ring, double q, size_t skip) {
            if (n <= skip) return 0.0;
            std::vector<float> v(ring.begin(), ring.begin() + n);
            if (skip) v.erase(v.begin());
            size_t k = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5));
            std::nth_element(v.begin(), v.begin() + k, v.end());
            return static_cast<double>(v[k]);
        };
        const size_t skipFirst = timingCount_ <= frameSamples_.size() ? 1 : 0;
        stats_.frameMsP50 = percentile(frameSamples_, 0.50, skipFirst);
        stats_.frameMsP95 = percentile(frameSamples_, 0.95, skipFirst);
        stats_.frameMsP99 = percentile(frameSamples_, 0.99, skipFirst);
        stats_.renderMsP95 = percentile(renderSamples_, 0.95, 0);
        stats_.renderMsP99 = percentile(renderSamples_, 0.99, 0);
        if (sg_stats_enabled()) {
            sg_stats gs = sg_query_stats();
            stats_.drawCalls = static_cast<int>(gs.prev_frame.num_draw + gs.prev_frame.num_draw_ex);
        }
    }
    viewComplete_ = incomplete == 0;
    if (snapshotShown_ && settleSnapshot_ && snapshotFade_ < 0.0) {
        const bool quiet = heldNow == 0 && waitingNow == 0 && !fading && !mergeActive_ && visibleRenders == 0 && visibleDeferred == 0 &&
                           visibleBuilds == 0 && bordersReady_ && roadsWaiting == 0;
        settleQuiet_ = quiet ? settleQuiet_ + 1 : 0;
        if (settleQuiet_ >= 2 || tnow - snapshotStart_ >= (switchHold_ ? std::max(settleWait_, switchWait_) : settleWait_)) {
            snapshotFade_ = tnow;
            snapshotFadeLength_ = settleFade_;
            if (visualLog_)
                std::fprintf(visualLog_, "settle frame %llu t %.3f dissolve after %.3f quiet %d waiting %d renders %d deferred %d tiles%s\n",
                             static_cast<unsigned long long>(frame_), tnow, tnow - snapshotStart_, settleQuiet_, waitingNow, visibleRenders,
                             visibleDeferred, levelMixText().c_str());
        }
    } else if (settleFreeze_ && !settleBroken_ && !views && !snapshotShown_ && terrainAlpha_ >= 0.999f && tnow - lastMotion_ >= 0.15 &&
               (heldNow > 0 || mergeActive_ || fading || capBehind || frame_ <= 2)) {
        settleWanted_ = true;
        if (visualLog_)
            std::fprintf(visualLog_, "settle frame %llu t %.3f want held %d merging %d fades %d edges %d morphs %d cap %d\n",
                         static_cast<unsigned long long>(frame_), tnow, heldNow, mergeActive_ ? 1 : 0, fadeKinds[0], fadeKinds[1], fadeKinds[2],
                         capBehind ? 1 : 0);
    }
    busy_ = settleWanted_ || stepWanted_ || !bordersReady_ || !roadBuildQueue_.empty() || fading || mergeActive_ || snapshotShown_ || tilesBusy || renders > 0 || builds > 0 || deferred > 0 || loadsTaken > 0 || elevationChanged ||
            ((labelsOn_ || contours_) && labels_.animating()) || (buildingsOn_ && buildings_.busy());
    stats_.loading = pending || stats_.pendingRequests > 0 || heldNow > 0 ? 1 : 0;
    stats_.renderedFrames = static_cast<long long>(frame_);
    waiting_ = !busy_ && stats_.loading;
    if (busy_) {
        pollGap_ = 0.25;
    } else if (waiting_) {
        double wake = tnow + pollGap_;
        pollGap_ = std::min(pollGap_ * 2.0, 4.0);
        if (heldNow > 0) {
            if (revealGate(tnow)) wake = tnow;
            for (double d : {lastReveal_ + revealGap_, lastMotion_ + 0.15, std::max(oldestHeld, lastMotion_) + revealHold_, oldestHeld + motionHold_})
                if (d > tnow) wake = std::min(wake, d);
        }
        wakeAt_ = wake;
    }
    if (visualLog_) std::fflush(visualLog_);
    if (busyLog_ && busy_ && frame_ % 30 == 0)
        std::fprintf(stderr, "[otk] busy frame %llu pending %d fading %d merging %d labels %d buildings %d tiles %d requests %d renders %d builds %d elevation %d needs %zu deferred %d loads %d held %d\n",
                     static_cast<unsigned long long>(frame_), pending ? 1 : 0, fading ? 1 : 0, mergeActive_ ? 1 : 0,
                     (labelsOn_ || contours_) && labels_.animating() ? 1 : 0, buildingsOn_ && buildings_.busy() ? 1 : 0, tilesBusy ? 1 : 0,
                     stats_.pendingRequests, renders, builds, elevationChanged ? 1 : 0, needs_.size(), deferred, loadsTaken, heldNow);
    if (busyLog_ && busy_ && frame_ % 30 == 0) std::fprintf(stderr, "[otk]   t %.2f base %d %s\n", now(), baseLayer_, loader_.debug().c_str());
    if (trace_) {
        int waitImagery = 0, waitTerrain = 0, unrendered = 0;
        for (auto& dt : tiles) {
            auto sf = surface_.find(dt.key);
            auto tf = terrain_.find(dt.key);
            if (sf == surface_.end()) ++unrendered;
            else if (!sf->second.complete) ++waitImagery;
            if (tf == terrain_.end() || !tf->second.complete) ++waitTerrain;
        }
        std::fprintf(trace_,
                     "t %.3f frame %llu tiles %zu needs %zu work %zu renders %d builds %d loader %d elev %d incomplete %d "
                     "waitimg %d unrendered %d waitterrain %d L%d gsd %.1f\n",
                     now(), static_cast<unsigned long long>(frame_), tiles.size(), needs_.size(), work.size(), renders, builds,
                     loader_.pending(), elevation_.pending(), incomplete, waitImagery, unrendered, waitTerrain, maxLevel,
                     cam.gsd);
        if (std::getenv("OTK_TRACE_TILES")) {
            GeoPoint eg = ecefToGeodetic(cam.eye());
            std::fprintf(trace_, "  eye %.5f %.5f alt %.1f ground %.1f\n", eg.lat, eg.lon, eg.alt, elevationAt(eg.lat, eg.lon));
            for (size_t i = 0; i < tiles.size() && i < 6; ++i) {
                Bounds tb = bounds(tiles[i].key);
                std::fprintf(trace_, "  draw L%d %d,%d dist %.0f bounds %.4f %.4f %.4f %.4f\n", tiles[i].key.level, tiles[i].key.x,
                             tiles[i].key.y, tiles[i].distance, tb.west, tb.south, tb.east, tb.north);
            }
        }
        std::fprintf(trace_, "  loader %s\n", loader_.debug().c_str());
        int shown = 0;
        for (auto& dt : tiles) {
            auto sf = surface_.find(dt.key);
            if (sf == surface_.end() || sf->second.complete || shown >= 2) continue;
            ++shown;
            std::fprintf(trace_, "  tile L%d %d,%d missing", dt.key.level, dt.key.x, dt.key.y);
            for (const TileId& m : sf->second.missing) {
                auto f = failed_.find(m);
                std::fprintf(trace_, " [%d/%d/%d %s]", m.z, m.x, m.y,
                             imagery_.count(m) ? "have" : f != failed_.end() ? (std::isinf(f->second) ? "perm" : "retry") : "wait");
            }
            std::fprintf(trace_, "\n");
        }
        std::fflush(trace_);
    }
}

}
