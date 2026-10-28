#pragma once

#include "core/camera.hpp"
#include "elevation/elevation.hpp"
#include "feature/feature_layer.hpp"
#include "model/model_layer.hpp"
#include "model/pointcloud.hpp"
#include "raster/canopy.hpp"
#include "render/building_layer.hpp"
#include "render/label_layer.hpp"
#include "tiles3d/tileset.hpp"
#include "raster/tile_cache.hpp"
#include "raster/tile_loader.hpp"
#include "raster/tile_provider.hpp"

#include <sokol_gfx.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace otk {

struct QuadKey {
    int level = 0;
    int x = 0;
    int y = 0;
    bool operator==(const QuadKey& o) const { return level == o.level && x == o.x && y == o.y; }
};

struct QuadKeyHash {
    size_t operator()(const QuadKey& k) const {
        return std::hash<std::uint64_t>()((static_cast<std::uint64_t>(k.level) << 56) ^
                                          (static_cast<std::uint64_t>(k.x) << 28) ^ static_cast<std::uint64_t>(k.y));
    }
};

struct RenderView {
    Camera* cam = nullptr;
    const sg_swapchain* swapchain = nullptr;
    const sg_attachments* attachments = nullptr;
    int x = 0, y = 0, width = 0, height = 0;
    bool clear = true;
};

class GlobeRenderer {
public:
    GlobeRenderer();
    ~GlobeRenderer();

    bool init();
    void shutdown();
    void setLayers(const std::vector<ImageryLayer>& layers);
    void applyLayers();
    void setLayerList(const std::vector<ImageryLayer>& layers);
    bool shown(int l) const { return l >= 0 && l < static_cast<int>(layers_.size()) && layers_[l].visible && layers_[l].opacity > 0.0f; }
    const std::vector<ImageryLayer>& layers() const { return userLayers_; }
    const std::vector<TileMatrix>& matrices() const { return matrices_; }
    void setCacheDirectory(const std::string& dir);
    const std::string& cacheDirectory() const { return cacheDir_; }
    TileCache& cache() { return cache_; }
    bool reachabilityChanged() const;
    void setOffline(bool offline);
    int fetch(const std::string& url, std::vector<std::uint8_t>& body, const FetchOptions& options) {
        return fetchUrl(url, body, options, cache_.isOpen() ? &cache_ : nullptr, &offline_, "OTKGlobeEngine/0.1");
    }
    void retryUnavailable();
    void retryLayer(int layer);
    void setBackdrop(bool enabled) { backdropEnabled_ = enabled; }
    void setMemoryBudget(int megabytes);
    void fitPools();
    size_t imageryLimit() const;
    void poolPressure(const char* what);
    void setElevation(const std::vector<ElevationLayer>& layers);
    std::vector<ElevationLayer> elevationLayers() const { return elevation_.sources(); }
    void setTerrainEnabled(bool enabled);
    void setTerrainExaggeration(double factor);
    void setDepthPerception(bool enabled) { depthPerception_ = enabled; }
    void setDiagnostics(bool enabled) { diagnostics_ = enabled; }
    bool diagnostics() const { return diagnostics_; }
    template <typename... A>
    void log(const char* fmt, A... args) const { diag(fmt, args...); }
    void setTerrainAlpha(float alpha) { terrainAlpha_ = std::clamp(alpha, 0.0f, 1.0f); }
    void setHeatmap(bool enabled, double minHeight, double maxHeight, float opacity) {
        heatmap_ = enabled;
        heatMin_ = minHeight;
        heatMax_ = maxHeight;
        heatOpacity_ = opacity;
    }
    void setGrid(bool enabled);
    void setGridType(int type);
    void setGridLayer(int type, bool enabled);
    void setGrids(const std::array<bool, 3>& on);
    bool anyGrid() const { return gridOn_[0] || gridOn_[1] || gridOn_[2]; }
    bool gridOn(int type) const { return gridOn_[std::clamp(type, 0, 2)]; }
    double gridStep(const Camera& cam, int type) const;
    void setLabels(bool enabled);
    bool labels() const { return labelsOn_; }
    void setLabelsSource(const std::string& source);
    const std::string& labelsSource() const { return labelsSource_; }
    int labelsLayer() const { return overlayLayer_; }
    void setContours(std::shared_ptr<const ContourSet> set);
    void setBuildings(bool enabled);
    bool buildings() const { return buildingsOn_; }
    BuildingLayer::ImageryFn roofImagery();
    void setOverlay(std::function<void(int width, int height)> draw) { overlay_ = std::move(draw); }
    bool terrainEnabled() const { return terrainEnabled_; }
    void setLookahead(const Camera* cam, double priority = 2.5) {
        hasLookahead_ = cam != nullptr;
        lookaheadPriority_ = priority;
        if (cam) lookahead_ = *cam;
    }
    double buildingHeightAt(double lat, double lon) const { return buildingsOn_ ? buildings_.heightAt(lat, lon) : 0.0; }
    bool hasContours() const { return contours_ != nullptr; }
    void setSkyVisible(bool visible) { skyVisible_ = visible; }
    void setGroundColor(float r, float g, float b) {
        ground_[0] = r;
        ground_[1] = g;
        ground_[2] = b;
        markDirty({-180, -90, 180, 90});
    }
    void setBackground(float r, float g, float b) {
        clear_[0] = r;
        clear_[1] = g;
        clear_[2] = b;
    }
    void setSlope(bool enabled, float opacity, double maxDegrees) {
        slope_ = enabled;
        slopeOpacity_ = opacity;
        slopeMax_ = std::max(maxDegrees, 1.0);
    }
    double terrainHeight(double lat, double lon) const;
    std::uint64_t addOverlay(const GeoRect& rect, int width, int height, const std::vector<std::uint8_t>& rgba, float opacity);
    bool removeOverlay(std::uint64_t id);
    double elevationAt(double lat, double lon) const;
    std::vector<double> queryElevation(const std::vector<std::pair<double, double>>& latLon, double resolutionMeters,
                                       std::vector<double>* resolutions = nullptr) const {
        return elevation_.query(latLon, resolutionMeters, resolutions);
    }
    void explain(const Camera& cam, double lat, double lon, const char* tag) const;
    TileLoader& loader() { return loader_; }
    FeatureLayer& features() { return features_; }
    ModelLayer& models() { return models_; }
    PointCloudLayer& points() { return points_; }
    std::uint64_t addTileset(const std::string& uri, const TilesetOptions& options, Vec3* center, double* radius = nullptr);
    std::vector<std::string> credits() const;
    void setSunLighting(bool enabled, double unixSeconds);
    bool removeTileset(std::uint64_t id) { return tilesets_.erase(id) > 0; }
    bool setTilesetOpacity(std::uint64_t id, float opacity) {
        auto it = tilesets_.find(id);
        if (it == tilesets_.end()) return false;
        it->second->setOpacity(std::clamp(opacity, 0.0f, 1.0f));
        return true;
    }
    const FeatureLayer& features() const { return features_; }
    const TileLoader& loader() const { return loader_; }
    void setImageryScale(double s) { imageryScale_ = s; }
    void setLodMorph(double fraction) { lodMorph_ = softwareRaster_ ? 0.0 : std::clamp(fraction, 0.0, 1.0); }
    double lodMorph() const { return lodMorph_; }
    void setDpiScale(double s) { dpiScale_ = s; }
    void render(Camera& cam, const sg_swapchain* swapchain, const sg_attachments* attachments, int width, int height,
                const std::vector<RenderView>* views = nullptr);
    bool busy() const { return busy_; }
    bool wakeDue() const;
    bool waiting() const { return waiting_; }
    bool fetching() const { return stats_.pendingRequests > 0; }
    double wakeIn() const { return wakeAt_ - now(); }
    bool presentIdle(Camera& cam, const sg_swapchain& swapchain);
    Stats stats() const { return stats_; }
    void setCpuMs(double ms, double threadMs = 0) {
        stats_.cpuMs = ms;
        stats_.cpuThreadMs = threadMs;
    }

private:
    struct Bounds {
        double west, south, east, north;
    };
    struct TerrainTile {
        sg_buffer vbuf{};
        Vec3 center;
        float minH = 0;
        float maxH = 0;
        float skirt = 1;
        std::vector<float> edges;
        int version = -1;
        bool complete = false;
        std::uint64_t used = 0;
    };
    static constexpr int kMips = 9;
    struct SurfaceTile {
        sg_image image{};
        sg_view attachment{};
        sg_view texture{};
        std::array<sg_view, kMips> levels{};
        std::array<sg_view, kMips> sources{};
        bool complete = false;
        int version = -1;
        std::uint64_t used = 0;
        std::vector<TileId> missing;
        int imageryZ = -1;
        int sourceZ = -1;
        bool degraded = false;
        double lineDeg = 0.0;
        bool overlaysOmitted = false;
        std::vector<TileId> roadTiles;
        bool presentable = false;
        std::uint64_t shown = 0;
        double renderedAt = -1.0;
    };
    struct ImageryTexture {
        sg_image image{};
        sg_view view{};
        std::uint64_t used = 0;
        std::shared_ptr<VectorTile> vector;
        sg_buffer fill{};
        sg_buffer line{};
        sg_buffer roads{};
        int fillCount = 0;
        int lineCount = 0;
        int roadCount = 0;
        int solidRoads = 0;
        bool drawn = false;
        sg_buffer roads3d{};
        int roads3dCount = 0;
        int roads3dVersion = -1;
        double roads3dBuilt = -1.0e9;
        Vec3 roadsOrigin{};
        std::array<int, 33> roadChunk{};
        std::array<std::array<int, 13>, 32> roadRanks{};
        std::array<GeoRect, 32> roadBox{};
    };
    struct DrawTile {
        QuadKey key;
        double distance = 0;
        int kind = 0;
        QuadKey merge{-1, 0, 0};
        float mergeT = 0.0f;
    };
    struct Need {
        QuadKey key;
        double priority = 0;
        bool prefetch = false;
    };
    struct NodeInfo {
        bool visible = false;
        bool refine = false;
        bool nearRefine = false;
        double distance = 0;
        double ratio = 0;
    };
    struct Transition {
        QuadKey src{-1, 0, 0};
        int version = -1;
        QuadKey from{-1, 0, 0};
        int fromVersion = -1;
        double start = -1.0;
        std::uint64_t frame = 0;
        QuadKey merge{-1, 0, 0};
        int mergeVersion = -1;
        float mergeT = 0.0f;
        std::uint64_t mergeFrame = 0;
        int depth = 0;
        float edge[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        int edgeUp = 0;
        double edgeTime = -1.0;
        bool held = false;
        double heldSince = -1.0;
        char why = 0;
        bool finishing = false;
    };
    struct Merge {
        double start = 0.0;
        std::uint64_t frame = 0;
    };
    std::unordered_map<QuadKey, Merge, QuadKeyHash> merges_;
    int mergeDepth_ = 0;
    bool mergeActive_ = false;
    double revealHold_ = 3.0;
    double revealGap_ = 0.3;
    double lastReveal_ = -1.0e9;
    double motionHold_ = 2.0;
    double uniformShare_ = 1.0;
    int uniformDepth_ = 0;
    double lastMotion_ = -1.0e9;
    double motionKey_[5] = {};
    double snapshotKey_[5] = {};
    double motionTime_ = 0.0;
    double motionSpeed_ = 60.0;
    bool revealOpen_ = true;
    int heldPrev_ = 0;
    double oldestHeldPrev_ = 0.0;
    int waitingPrev_ = 0;
    int refineWaiting_ = 0;
    double mergeSeconds_ = 0.35;
    double mergeGap_ = 0.6;
    struct SurfaceDraw {
        sg_view texture{};
        float opacity = 1.0f;
        int first = 0;
        int count = 0;
        float tint[3] = {1.0f, 1.0f, 1.0f};
    };
    struct VectorDraw {
        const ImageryTexture* tile = nullptr;
        float xform[4] = {};
        TileId id{};
    };
    struct SurfaceJob {
        QuadKey key;
        std::vector<SurfaceDraw> draws;
        std::vector<TileId> missing;
        std::vector<VectorDraw> vectors;
        bool background = false;
        int featureFirst = 0;
        int featureCount = 0;
        bool complete = false;
        bool degraded = false;
        bool standIn = false;
        bool overlaysOmitted = false;
        bool presentable = true;
        int hollow = 0;
        int imageryZ = -1;
        int sourceZ = 99;
    };

    static Bounds bounds(const QuadKey& k);
    NodeInfo info(const Camera& cam, const QuadKey& k) const;
    void setUniform(const Camera& c);
    void buildRoads3D(ImageryTexture& t, int version);
    double seamSkirt(const QuadKey& k, const TerrainTile& t, const std::unordered_map<QuadKey, const TerrainTile*, QuadKeyHash>& drawn,
                     int coarser[4]) const;
    bool ready(const QuadKey& k) const;
    bool presentable(const QuadKey& k) const;
    bool finished(const QuadKey& k) const;
    bool retrying(const QuadKey& k) const;
    int surfaceZ(const QuadKey& q) const;
    int surfaceZMax(const QuadKey& q) const;
    bool roadsReady(const std::vector<TileId>& roads) const;
    int readyGap(const QuadKey& k) const;
    bool sparseLayer(int l) const;
    bool progressed(const SurfaceTile& s) const;
    void need(const QuadKey& k, double priority);
    void prefetch(const QuadKey& k, double priority) {
        if (prefetching_) return;
        for (size_t i = prefetch_.size(), n = 0; i-- > 0 && n < 6; ++n)
            if (prefetch_[i].key == k) {
                prefetch_[i].priority = std::min(prefetch_[i].priority, priority);
                return;
            }
        prefetch_.push_back({k, priority, true});
    }
    void traverse(const Camera& cam, const QuadKey& k, std::vector<DrawTile>& out);
    TerrainTile& terrain(const QuadKey& k);
    void buildTerrain(const QuadKey& k, TerrainTile& t, double priority);
    SurfaceTile* acquireSurface(const QuadKey& k);
    SurfaceTile freshSurface();
    void recycleSurface(SurfaceTile& s);
    bool ensureSnapshotTarget(int width, int height);
    bool ensureIdleTarget(int width, int height);
    void dropIdleTarget();
    void dropSnapshotTarget();
    void drawSnapshot(float alpha, const Camera* now = nullptr);
    static void dropSurface(SurfaceTile& s);
    int processLoads();
    void evict();
    bool buildSurfaceJob(const QuadKey& k, double priority, SurfaceJob* job, std::vector<float>* verts,
                         std::vector<float>* fverts = nullptr, double texelDeg = 0, bool standIn = false);
    bool standIn(const Camera& cam, const QuadKey& k) const;
    static void dropImagery(ImageryTexture& t);
    void requestBase();
    void want(const TileId& id, double priority);
    ImageryTexture* findTexture(const TileId& id, const TileMatrix& m, double priority, bool& missing, TileId& src);
    void invalidateTerrain();
    void markDirty(const GeoRect& rect);
    double now() const;
    bool revealGate(double t) const;
    std::chrono::steady_clock::time_point frameStart_;
    std::chrono::steady_clock::time_point lastFrameStart_;
    std::array<float, 240> frameSamples_{};
    std::array<float, 240> renderSamples_{};
    size_t timingCount_ = 0;
    double elapsedMs() const { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart_).count(); }

    TileCache cache_;
    CanopySource canopy_{&cache_};
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE) || defined(__EMSCRIPTEN__)
    TileLoader loader_{3, 8};
    ElevationManager elevation_{4};
#else
    TileLoader loader_{4, 24};
    ElevationManager elevation_{8};
#endif
    FeatureLayer features_;
    LabelLayer labels_;
    ModelLayer models_;
    PointCloudLayer points_;
    std::unordered_map<std::uint64_t, std::unique_ptr<Tileset3D>> tilesets_;
    std::uint64_t nextTileset_ = 1;
    std::atomic<bool> offline_{false};
    std::uint64_t netEpoch_ = 0;
    int gridType_ = 0;
    int baseLayer_ = 0;
    bool fadeUpgrades_ = true;
    bool busyLog_ = false;
    int maxDive_ = 0;
    struct Retired {
        SurfaceTile surface;
        std::uint64_t frame = 0;
    };
    std::unordered_map<QuadKey, std::vector<Retired>, QuadKeyHash> retiring_;
    struct FadeRef {
        QuadKey key;
        int version = -1;
        double progress = 0.0;
    };
    std::vector<FadeRef> fadeRefs_;
    size_t retiredCount_ = 0;
    bool slope_ = false;
    float slopeOpacity_ = 0.6f;
    double slopeMax_ = 45.0;
    std::string cacheDir_;
    std::vector<ImageryLayer> layers_;
    std::vector<ImageryLayer> userLayers_;
    std::string labelsSource_ = "https://tiles.openfreemap.org/planet";
    int overlayLayer_ = -1;
    std::shared_ptr<const ContourSet> contours_;
    BuildingLayer buildings_;
    bool basePhoto_ = true;
    // bool buildingsOn_ = true;
    bool buildingsOn_ = false;
    bool sunOn_ = false;
    Vec3 sunDir_;
    std::function<void(int, int)> overlay_;
    std::unordered_map<TileId, std::uint64_t, TileIdHash> labelTileUse_;
    std::vector<TileMatrix> matrices_;
    std::vector<bool> vectorLayers_;
    std::unordered_map<QuadKey, TerrainTile, QuadKeyHash> terrain_;
    std::unordered_map<QuadKey, SurfaceTile, QuadKeyHash> surface_;
    std::unordered_map<TileId, ImageryTexture, TileIdHash> imagery_;
    std::unordered_map<TileId, double, TileIdHash> failed_;
    std::unordered_map<TileId, int, TileIdHash> failCount_;
    std::vector<SurfaceTile> freeSurfaces_;
    std::vector<std::pair<TileId, double>> requests_;
    std::vector<Need> needs_;
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
    size_t surfaceCapacity_ = 400, imageryCapacity_ = 244, terrainCapacity_ = 1500, freeSurfacePool_ = 25;
#else
    size_t surfaceCapacity_ = 800, imageryCapacity_ = 488, terrainCapacity_ = 3000, freeSurfacePool_ = 50;
#endif
    std::unordered_map<QuadKey, Transition, QuadKeyHash> transitions_;
    struct Sphere {
        Vec3 center;
        double radius = 0;
        float minH = 0, maxH = 0;
    };
    mutable std::unordered_map<QuadKey, Sphere, QuadKeyHash> spheres_;
    double needScale_ = 1.0;
    bool prefetching_ = false;
    bool hasLookahead_ = false;
    Camera lookahead_;
    double lookaheadPriority_ = 2.5;
    std::vector<Need> prefetch_;
    bool viewComplete_ = false;
    std::unordered_map<QuadKey, std::uint64_t, QuadKeyHash> drawn_;
    std::unordered_map<QuadKey, std::uint64_t, QuadKeyHash> refined_;
    int syncBuilds_ = 0;
    int retaining_ = 0;
    int usefulLevel_ = 22;
    std::vector<Need> overviewCache_;
    std::vector<Need> admittedCache_;
    bool overviewReused_ = false;
    bool urgentPrefetch_ = false;
    double zoomRate_ = 0, zoomClock_ = 0, zoomGsd_ = 0;
    double lastOverviewKey_[6] = {};
    bool budgetSet_ = false;
    static double physicalMemoryMb();
    static double videoMemoryMb();
    void profileMark(int section);
    bool profiling_ = false;
    std::string profilePath_;
    std::chrono::steady_clock::time_point profileStart_;
    double profileLast_ = 0;
    double profileAcc_[8] = {};
    double profileSub_[8] = {};
    double profileSubCpu_[8] = {};
    double profileSubLast_ = 0;
    double profileSubCpuLast_ = 0;
    static double threadMs();
    void profileSub(int i) {
        if (!profiling_) return;
        const double t = elapsedMs();
        const double c = threadMs();
        if (i >= 0) {
            profileSub_[i] += t - profileSubLast_;
            profileSubCpu_[i] += c - profileSubCpuLast_;
        }
        profileSubLast_ = t;
        profileSubCpuLast_ = c;
    }
    int profileFrames_ = 0;
    bool glMips_ = false;
    bool softwareRaster_ = false;
    void generateMips(const std::vector<sg_image>& images);
    int maxRefine() const;
    bool wasRefined(const QuadKey& k) const;
    int idealImageryZ(const QuadKey& k) const;
    struct Overlay {
        GeoRect rect;
        sg_image image{};
        sg_view view{};
        float opacity = 1.0f;
    };
    std::unordered_map<std::uint64_t, Overlay> overlays_;
    std::uint64_t nextOverlay_ = 1;
    std::array<bool, 3> gridOn_{};
    std::array<double, 3> gridSpacing_{0, 0, 0};
    bool labelsOn_ = true;
    bool skyVisible_ = true;
    float clear_[3] = {0.0f, 0.0f, 0.0f};
    float ground_[3] = {10 / 255.0f, 30 / 255.0f, 62 / 255.0f};

    sg_buffer indexBuffer_{};
    sg_buffer skyBuffer_{};
    sg_buffer surfaceBuffer_{};
    sg_buffer featureBuffer_{};
    sg_image fallbackImage_{};
    sg_view fallbackView_{};
    sg_image backdropImage_{};
    sg_view backdropView_{};
    bool backdropTried_ = false;
    bool backdropEnabled_ = true;
    sg_view backdrop();
    bool backdropUv(const QuadKey& key, sg_view& view, float uv[4]);
    sg_sampler sampler_{};
    sg_sampler imagerySampler_{};
    sg_shader globeShader_{};
    sg_shader surfaceShader_{};
    sg_shader skyShader_{};
    sg_pipeline globePipeline_{};
    sg_pipeline globeBlendPipeline_{};
    sg_pipeline surfacePipeline_{};
    sg_pipeline skyPipeline_{};
    bool holeMarker_ = false;
    bool lodBlend_ = true;
    double marginPrefetch_ = 0;
    sg_shader vfillShader_{};
    sg_shader vlineShader_{};
    sg_pipeline vfillPipeline_{};
    sg_pipeline vlinePipeline_{};
    sg_shader mipShader_{};
    sg_pipeline mipPipeline_{};
    sg_sampler mipSampler_{};
    sg_image scratch_{};
    sg_view scratchAttachment_{};
    sg_view scratchTexture_{};

    std::array<double, 16> frustum_{};
    Vec3 eye_;
    double pixelScale_ = 1.0;
    std::uint64_t frame_ = 0;
    int visits_ = 0;
    bool diagnostics_ = false;
    int prevMaxLevel_ = 0;
    double prevGsd_ = 0;
    void diag(const char* fmt, ...) const;
    void explainCollapse(const Camera& cam, const std::vector<DrawTile>& tiles);
    FILE* trace_ = nullptr;
    int imageryVersion_ = 0;
    int terrainEpoch_ = 0;
    double imageryScale_ = 1.0;
    double lodMorph_ = 1.0;
    std::unordered_map<QuadKey, std::pair<double, std::uint64_t>, QuadKeyHash> morphSince_;
    bool visualStats_ = false;
    long long fadeMisses_ = 0;
    FILE* visualLog_ = nullptr;
    double rerenderGap_ = 0.0;
    struct Shown {
        QuadKey src{-1, 0, 0};
        int version = -1;
        int terrain = -1;
        std::uint64_t frame = 0;
    };
    std::unordered_map<QuadKey, Shown, QuadKeyHash> shown_;
    double screenFraction(const Camera& cam, const QuadKey& k, const TerrainTile& t) const;
    int switchVersion_ = 0;
    sg_shader snapshotShader_{};
    sg_pipeline snapshotPipeline_{};
    sg_image snapshotImage_{};
    sg_image snapshotMsaa_{};
    sg_image snapshotDepth_{};
    sg_view snapshotColor_{};
    sg_view snapshotResolve_{};
    sg_view snapshotDepthView_{};
    sg_view snapshotTexture_{};
    int snapshotWidth_ = 0;
    sg_image idleImage_{};
    sg_image idleMsaa_{};
    sg_image idleDepth_{};
    sg_view idleColor_{};
    sg_view idleResolve_{};
    sg_view idleDepthView_{};
    sg_view idleTexture_{};
    int idleWidth_ = 0;
    int idleHeight_ = 0;
    bool idleValid_ = false;
    bool renderingIdle_ = false;
    int snapshotHeight_ = 0;
    bool switchWanted_ = false;
    bool switchHold_ = false;
    double switchWait_ = 5.0;
    double switchAt_ = -1.0e9;
    std::vector<float> layerOpacity_;
    bool snapshotShown_ = false;
    double snapshotStart_ = 0.0;
    double snapshotFade_ = -1.0;
    double snapshotFadeLength_ = 0.4;
    Camera snapshotCam_{};
    double settleMoveFade_ = 0.25;
    bool settleFreeze_ = false;
    bool uniformLod_ = true;
    bool lines3d_ = true;
    bool bordersReady_ = true;
    std::unordered_set<TileId, TileIdHash> roadBuildQueue_;
    double uniformDist_ = 0.0;
    double uniformMix_ = 0.0;
    int uniformZ_ = 99;
    double coarsenAt_ = -1.0;
    int pendingMergeZ_ = 99;
    double stepWait_ = -1.0;
    bool stepWanted_ = false;
    bool stepSnapshot_ = false;
    bool stepInstant_ = false;
    double settleWait_ = 3.0;
    double settleFade_ = 0.35;
    bool settleSnapshot_ = false;
    bool settleWanted_ = false;
    bool settleFrozen_ = false;
    bool settleBroken_ = false;
    int settleQuiet_ = 0;
    double dpiScale_ = 1.0;
    double exaggeration_ = 1.0;
    bool terrainEnabled_ = true;
    bool depthPerception_ = true;
    float terrainAlpha_ = 1.0f;
    bool heatmap_ = false;
    double heatMin_ = 0, heatMax_ = 4500;
    float heatOpacity_ = 0.55f;
    bool flipSurface_ = false;
    bool busy_ = true;
    bool waiting_ = false;
    double wakeAt_ = 0.0;
    double pollGap_ = 0.25;
    bool ready_ = false;
    Stats stats_;
    int invalidResources_ = 0;
    double budgetMb_ = 0;
    size_t imageryPoolMax_ = 0;
    std::vector<std::pair<QuadKey, double>> blendParents_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

}
