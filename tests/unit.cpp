#include "core/camera.hpp"
#include "core/geo.hpp"
#include "core/geoid.hpp"
#include "core/prj.hpp"
#include "elevation/quantized_mesh.hpp"
#include "otk/features.hpp"
#include "otk/astro.hpp"
#include "otk/klv.hpp"
#include "otk/formats.hpp"
#include "otk/mgrs.hpp"
#include "otk/milsym.hpp"
#include "otk/navigation.hpp"
#include "net/http.hpp"
#include "otk/engine.hpp"
#include "raster/tile_cache.hpp"
#include "raster/tile_loader.hpp"
#include "raster/tile_matrix.hpp"
#include "raster/tile_package.hpp"
#include "raster/tile_provider.hpp"
#include "raster/pmtiles.hpp"
#include "otk/image.hpp"
#include "raster/vector_tile.hpp"
#include "feature/buildings.hpp"
#include "formats/adapters.hpp"
#include "labels/label_data.hpp"
#include "core/astro.hpp"
#include "model/gltf_codecs.hpp"
#include "model/model_convert.hpp"
#include "model/model_layer.hpp"

#if defined(OTK_WITH_MESHOPT)
#include <meshoptimizer.h>
#endif
#include <json.hpp>
#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    } else {
        std::printf("ok   %s\n", what);
    }
}

std::string data(const char* name) { return std::string(OTK_TEST_DATA) + "/" + name; }

}

int main() {
    using namespace otk;
    using adapters::parseWkt;
    using adapters::writeWkt;

    for (auto [lat, lon, h] : {std::tuple{0.0, 0.0, 0.0}, {46.8529, -121.7604, 4392.0}, {-33.9, 151.2, -20.0}, {89.9, 45.0, 100.0}}) {
        GeoPoint g = ecefToGeodetic(geodeticToEcef(lat, lon, h));
        bool ok = std::abs(g.lat - lat) < 1e-9 && std::abs(g.lon - lon) < 1e-9 && std::abs(g.alt - h) < 1e-4;
        check(ok, ("ecef roundtrip " + std::to_string(lat)).c_str());
    }

    check(toMgrs(46.8529, -121.7604, 5) == "10T ES 94499 89563", "mgrs rainier");
    for (auto [lat, lon] : {std::pair{46.8529, -121.7604}, {-33.8688, 151.2093}, {60.0, 5.5}, {0.5, 0.5}, {-77.8, 166.7}}) {
        double rl, ro;
        bool ok = fromMgrs(toMgrs(lat, lon, 5), rl, ro) && length(geodeticToEcef(rl, ro, 0) - geodeticToEcef(lat, lon, 0)) < 1.5;
        check(ok, ("mgrs roundtrip " + std::to_string(lat)).c_str());
    }
    check(utmZone(60.0, 5.5) == 32, "utm norway exception");
    check(utmBand(-80.0) == 'C' && utmBand(83.9) == 'X', "utm bands");

    TileMatrix wm = TileMatrix::webMercator(0, 19);
    double minX, minY, maxX, maxY;
    wm.tileBounds(1, 1, 0, minX, minY, maxX, maxY);
    check(std::abs(minX) < 1e-6 && std::abs(maxY - 20037508.342789244) < 1e-3, "web mercator tile bounds");
    auto ranges = regionRanges(wm, {-122.95, 47.0, -122.85, 47.08}, 10, 15);
    std::int64_t tiles = 0;
    for (auto& r : ranges) tiles += r.count();
    check(ranges.size() == 6 && tiles == 172, "region tile count");
    TileMatrix geo = TileMatrix::geographic(0, 10);
    check(geo.levels[0].matrixWidth == 2 && geo.levels[0].matrixHeight == 1, "geographic matrix level 0");

    Camera cam;
    cam.width = 1280;
    cam.height = 720;
    cam.focus = {47.0, -122.0, 0.0};
    cam.gsd = 10.0;
    cam.tilt = 45.0;
    cam.azimuth = 30.0;
    cam.update();
    Vec3 hit;
    bool picked = cam.screenToEcef(640, 360, hit);
    GeoPoint g = ecefToGeodetic(hit);
    check(picked && std::abs(g.lat - 47.0) < 1e-6 && std::abs(g.lon + 122.0) < 1e-6, "camera center pick");
    double sx, sy;
    check(cam.ecefToScreen(cam.focusEcef(), sx, sy) && std::abs(sx - 640) < 1e-3 && std::abs(sy - 360) < 1e-3, "camera project focus");
    cam.tilt = 89.0;
    cam.update();
    check(cam.tilt <= Camera::kMaxTilt, "camera tilt clamp");

    check(std::abs(geoidHeight(0, 0) - 17.2) < 0.5, "geoid at 0,0");
    check(std::abs(geoidHeight(47.0, -122.0) + 21.0) < 2.0, "geoid at olympia");

    std::vector<std::uint8_t> raw{1, 2, 3, 4};
    check(gunzip(raw) == raw, "gunzip passthrough");

    {
        std::ifstream in(data("overlay_z14.mvt"), std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto tile = decodeOverlayTile(bytes, 14, 2600, 5760);
        auto has = [&](MarkType type, const std::string& text) {
            for (const TileMark& m : tile->marks)
                if (m.type == type && m.text == text) return true;
            return false;
        };
        bool shield = false;
        for (const TileMark& m : tile->marks) shield = shield || (m.type == MarkType::Shield && m.text == "5" && m.network == "us-interstate");
        check(tile && tile->overlay && tile->roads.size() == 3 * 6, "overlay roads clipped to the tile, rail skipped");
        float west = 1e9f, east = -1e9f;
        for (size_t i = 0; tile && i + 5 < tile->roads.size(); i += 6) {
            west = std::min({west, tile->roads[i], tile->roads[i + 2]});
            east = std::max({east, tile->roads[i], tile->roads[i + 2]});
        }
        check(west >= -1e-6f && east <= 360.0f / 16384.0f + 1e-6f, "overlay roads stay inside the tile");
        check(has(MarkType::Road, "22nd Ave SE") && shield, "overlay road names and shields");
        check(has(MarkType::Place, "CASTLEWOOD") && !has(MarkType::Place, "Olympia"), "overlay neighbourhoods");
        check(has(MarkType::Park, "Watershed Park") && has(MarkType::Water, "Chambers Lake") && !has(MarkType::Water, "Centennial Fountain") &&
                  has(MarkType::Waterway, "Indian Creek"),
              "overlay parks and water");
        check(shortRoadName("North Street Southeast") == "North Street SE" && shortRoadName("Capitol Way South") == "Capitol Way S" &&
                  shortRoadName("North 1st Street") == "N 1st St" && shortRoadName("Boulevard Road Southeast") == "Boulevard Rd SE",
              "short road names");
    }

    {
        const double tileM = 40075016.686 * std::cos(47.04 * 3.14159265358979323846 / 180.0) / 16384.0;
        auto rect = [&](double u, double v, double wm, double hm) {
            const float u0 = static_cast<float>(u), v0 = static_cast<float>(v);
            const float u1 = static_cast<float>(u + wm / tileM), v1 = static_cast<float>(v + hm / tileM);
            return std::vector<std::array<float, 2>>{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}, {u0, v0}};
        };
        BuildingShape house;
        house.height = 10.0f;
        house.rings.push_back(rect(0.5, 0.5, 12.0, 8.0));
        BuildingShape tower;
        tower.height = 80.0f;
        tower.rings.push_back(rect(0.3, 0.3, 30.0, 30.0));
        BuildingShape edge;
        edge.height = 30.0f;
        edge.rings.push_back(rect(1.0 - 10.0 / tileM, 0.7, 20.0, 15.0));
        const std::vector<BuildingShape> shapes{house, tower, edge};
        const auto points = buildingSamplePoints(shapes, {}, 14, 2598, 5759);
        const BuildingMesh mesh = buildBuildingMesh(shapes, {}, 14, 2598, 5759, std::vector<double>(points.size(), 100.0), 2048);
        bool indices = !mesh.indices.empty() && mesh.indices.size() % 3 == 0;
        for (std::uint32_t i : mesh.indices) indices = indices && i < mesh.vertices.size();
        bool sloped = false, tall = false, boundaryWall = false;
        double lowest = 1e9, east = -1e9;
        for (const BuildingVertex& v : mesh.vertices) {
            if (v.kind > 0.5 && v.normal[2] < 0.99f) sloped = true;
            if (v.kind < 0.5 && v.texcoord[3] <= 2.0f && v.texcoord[1] >= 79.0f) tall = true;
            if (v.kind < 0.5 && v.normal[0] > 0.99f && v.position[0] > tileM * 0.5 * 1.004 - 1.0) boundaryWall = true;
            if (v.kind < 0.5) lowest = std::min(lowest, static_cast<double>(v.position[2]));
            east = std::max(east, static_cast<double>(v.position[0]));
        }
        check(points.size() == 12 && mesh.buildings == 3 && indices, "building meshes from footprints");
        check(sloped && tall, "pitched house roofs and tower facades");
        check(mesh.atlas > 0 && mesh.cells.size() == 3 && mesh.cells[0].west < mesh.cells[0].east && mesh.cells[0].south < mesh.cells[0].north,
              "roof atlas cells");
        check(lowest > 97.7 && lowest < 98.2, "buildings stand on the sampled ground");
        check(east <= tileM * 0.5 * 1.004 + 0.5 && !boundaryWall, "buildings clipped to the tile without walls on the tile edge");

        std::vector<std::uint8_t> canopy(512 * 512, 0);
        for (int j = 200; j < 260; ++j)
            for (int i = 300; i < 340; ++i) canopy[static_cast<std::size_t>(j) * 512 + i] = 25;
        const auto groundPoints = canopySamplePoints(14, 2598, 5759);
        const BuildingMesh trees = buildCanopyMesh(canopy, 512, 1000, 14, 2598, 5759, std::vector<double>(groundPoints.size(), 100.0), 1024);
        bool treeIndices = !trees.indices.empty() && trees.indices.size() % 3 == 0;
        for (std::uint32_t i : trees.indices) treeIndices = treeIndices && i < trees.vertices.size();
        double treeTop = -1e9, treeEast = 1e9, treeWest = -1e9;
        for (const BuildingVertex& v : trees.vertices) {
            treeTop = std::max(treeTop, static_cast<double>(v.position[2]));
            treeEast = std::min(treeEast, static_cast<double>(v.position[0]));
            treeWest = std::max(treeWest, static_cast<double>(v.position[0]));
        }
        check(groundPoints.size() == kCanopyGround * kCanopyGround && treeIndices, "tree canopy mesh from heights");
        check(treeTop > 120.0 && treeTop < 126.0, "tree canopy stands on the ground at the canopy height");
        check(treeEast > 0.0 && treeWest < tileM * 0.25, "tree canopy placed where the heights are");
        check(trees.atlas == 1024 && trees.cells.size() == 64 && trees.cells[0].north > trees.cells[63].south, "tree texture split into imagery cells");
        std::vector<std::uint8_t> bare(512 * 512, 0);
        check(buildCanopyMesh(bare, 512, 1000, 14, 2598, 5759, std::vector<double>(groundPoints.size(), 100.0), 1024).indices.empty(),
              "no tree mesh without trees");
    }

    auto kml = parseFeatures(data("styles.kml"));
    check(kml.size() == 3 && kml[0].name == "Rainier Summit" && kml[0].style.iconColor == 0xFFFF0000, "kml styles");
    auto json = parseFeatures(data("olympia.geojson"));
    check(json.size() == 4 && json[2].geometries[0].type == GeometryType::Polygon, "geojson");
    auto gpx = parseFeatures(data("track.gpx"));
    check(gpx.size() == 3, "gpx");
    auto shp = parseFeatures(data("shapes.shp"));
    check(shp.size() == 2 && shp[0].name == "Shape Point", "shapefile with dbf");
    auto kmz = parseFile(data("overlay.kmz"));
    check(kmz.overlays.size() == 1 && kmz.overlays[0].width == 64 && kmz.features.size() == 1, "kmz ground overlay");
    auto mobac = loadMobac(data("mobac_multilayer_wms.xml"));
    check(mobac.size() == 2 && mobac[1].type == SourceType::Wms && mobac[1].srid == 4326, "mobac multilayer");

    std::ifstream f(data("qm_z12_x1319_y3117.terrain"), std::ios::binary);
    std::vector<std::uint8_t> qm((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    double span = 180.0 / 4096;
    double west = -180 + 1319 * span, south = -90 + 3117 * span;
    HeightGrid grid;
    bool decoded = decodeQuantizedMesh(gunzip(qm), west, south, west + span, south + span, 64, grid);
    double expect = 1500 + 300 * std::cos((47.0 - 47) * 20) * std::cos((-122.0 + 122) * 20);
    check(decoded && std::abs(grid.sample(-122.0, 47.0) - expect) < 2.0, "quantized mesh decode");

    {
        std::vector<std::uint8_t> hm(65 * 65 * 2 + 2, 0);
        for (int r = 0; r < 65; ++r)
            for (int c = 0; c < 65; ++c) {
                std::uint16_t v = static_cast<std::uint16_t>((1000.0 + 10.0 * c + 100.0 * r) * 5.0);
                std::memcpy(hm.data() + (static_cast<size_t>(r) * 65 + c) * 2, &v, 2);
            }
        HeightGrid hg;
        bool ok = decodeHeightmapTerrain(hm, 10.0, 40.0, 11.0, 41.0, hg);
        double center = hg.sample(10.5, 40.5);
        check(ok && hg.width == 64 && std::abs(center - (10.0 * 32 + 100.0 * 32)) < 1.0, "cesium heightmap-1.0 decode");
        check(std::abs(hg.sample(10.0 + 0.5 / 64, 41.0 - 0.5 / 64) - 55.0) < 1.0, "cesium heightmap-1.0 north-west first");
    }

    {
        auto dir = std::filesystem::temp_directory_path() / "otk local terrain";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir / "12" / "1319", ec);
        std::filesystem::copy_file(data("qm_z12_x1319_y3117.terrain"), dir / "12" / "1319" / "3117.terrain", ec);
        std::ofstream(dir / "layer.json") << R"({"tilejson":"2.1.0","format":"quantized-mesh-1.0","version":"1.2.0","scheme":"tms",)"
                                             R"("tiles":["{z}/{x}/{y}.terrain?v={version}"],"projection":"EPSG:4326"})";
        for (int variant = 0; variant < 3; ++variant) {
            Engine engine;
            ElevationLayer qm;
            qm.type = ElevationType::QuantizedMesh;
            qm.maxZoom = 12;
            if (variant == 0) qm.path = dir.string();
            else if (variant == 1) qm.url = (dir / "layer.json").string();
            else {
                std::string g = (dir / "layer.json").generic_string();
                qm.url = g[0] == '/' ? "file://" : "file:///";
                for (char c : g) qm.url += c == ' ' ? std::string("%20") : std::string(1, c);
            }
            engine.setElevation({qm});
            std::vector<double> h = engine.queryElevation({{47.0, -122.0, 0}});
            check(h.size() == 1 && std::abs(h[0] - expect) < 2.0,
                  (std::string("cesium terrain from a local ") + (variant == 0 ? "folder" : variant == 1 ? "layer.json path" : "file url")).c_str());
        }
        std::filesystem::remove_all(dir, ec);
    }

    {
        Engine engine;
        engine.setViewport(1000, 500, 1.0f);
        auto path = (std::filesystem::temp_directory_path() / "otk_region.geojson").string();
        std::ofstream(path) << R"({"type":"FeatureCollection","features":[)"
                               R"({"type":"Feature","geometry":{"type":"Point","coordinates":[-122.5,47.0]},"properties":{}},)"
                               R"({"type":"Feature","geometry":{"type":"LineString","coordinates":[[-122.0,47.5],[-121.5,46.5]]},"properties":{}}]})";
        std::vector<std::uint64_t> ids = engine.loadFeatures(path);
        Region r;
        bool ok = ids.size() == 2 && engine.featuresRegion(ids, r);
        check(ok && std::abs(r.west + 122.5) < 1e-9 && std::abs(r.east + 121.5) < 1e-9 && std::abs(r.south - 46.5) < 1e-9 &&
                  std::abs(r.north - 47.5) < 1e-9,
              "region of loaded features");
        View v = engine.viewForRegion(r);
        double expectGsd = std::max(111320.0 * std::cos(47.0 * 3.14159265358979323846 / 180.0) / 1000.0, 110574.0 / 500.0) * 1.2;
        check(std::abs(v.lat - 47.0) < 1e-9 && std::abs(v.lon + 122.0) < 1e-9 && std::abs(v.gsd - expectGsd) < 0.01 && v.tilt == 0.0,
              "view that fits a region");
        Feature a, b;
        a.geometries.push_back({GeometryType::Point, {{10.0, 179.5, 0}}, {}});
        b.geometries.push_back({GeometryType::Point, {{10.5, -179.5, 0}}, {}});
        Region ar;
        ok = engine.featuresRegion({engine.addFeature(a), engine.addFeature(b)}, ar);
        View av = engine.viewForRegion(ar);
        check(ok && std::abs(ar.west - 179.5) < 1e-9 && std::abs(ar.east + 179.5) < 1e-9 && std::abs(std::abs(av.lon) - 180.0) < 1e-9 &&
                  av.gsd < 300.0,
              "region across the antimeridian");
        View pv = engine.viewForRegion({-122.0, 47.0, -122.0, 47.0});
        check(!engine.featuresRegion({}, r) && std::abs(pv.gsd - 300.0 / 500.0 * 1.2) < 1e-9, "empty and single-point regions");
        std::error_code ec;
        std::filesystem::remove(path, ec);

        OpenResult opened = engine.open(data("olympia.geojson"));
        check(opened.ok && opened.plan.target == ImportTarget::Features && opened.data.features.size() == 4 && opened.hasRegion,
              "open routes vector data to features");
        FeatureQuery q;
        q.nameContains = "capitol";
        check(engine.queryFeatures(q).size() == 2, "query features in the engine");
        OpenResult dem = engine.open(data("dem-1234-z10.pmtiles"));
        std::vector<ElevationLayer> elev = engine.elevationLayers();
        check(dem.ok && dem.hasRegion && !elev.empty() && elev[0].type == ElevationType::PMTiles, "open adds a pmtiles dem as elevation");
        check(!engine.open(data("missing.kml")).ok, "open reports a missing file");
    }

    {
        ImageryLayer l;
        l.url = "https://h/{z}/{reverseY}/{x}?bbox={bbox-epsg-4326}&w={westDegrees}&r={reverseX}&q={quadkey}&s={width}x{height}{r}";
        l.maxZoom = 10;
        TileMatrix m = TileMatrix::webMercator(0, 10, 256);
        std::string u = tileUrl(l, m, 2, 1, 0);
        check(u == "https://h/2/3/1?bbox=-90,66.51326044,0,85.05112878&w=-90&r=2&q=01&s=256x256", ("cesium and maplibre url placeholders " + u).c_str());
        l.url = "https://h/export?bbox={bbox-epsg-3857}&size={width},{height}";
        std::string e = tileUrl(l, m, 1, 0, 0);
        check(e.find("bbox=-20037508.34") != std::string::npos && e.find(",0,0,20037508.34") != std::string::npos, ("bbox-epsg-3857 placeholder " + e).c_str());
    }

    {
        auto makeArchive = [](const std::string& path, bool osmand) {
            std::filesystem::remove(path);
            db::Database d;
            d.open(path, false);
            d.exec("CREATE TABLE tiles (x int, y int, z int, s int, image blob, PRIMARY KEY (x,y,z,s))");
            if (osmand) {
                d.exec("CREATE TABLE info (minzoom, maxzoom, tilenumbering, inverted_y)");
                d.exec("INSERT INTO info VALUES (5, 5, 'simple', 1)");
            } else {
                d.exec("CREATE TABLE info (maxzoom, minzoom)");
                d.exec("INSERT INTO info VALUES (12, 12)");
            }
            auto st = d.prepare("INSERT INTO tiles VALUES (?, ?, ?, 0, ?)");
            st.bind(1, 3);
            st.bind(2, osmand ? (1 << 5) - 1 - 7 : 7);
            st.bind(3, osmand ? 5 : 17 - 5);
            st.bind(4, std::vector<std::uint8_t>{1, 2, 3, 4});
            st.exec();
        };
        auto tmp = std::filesystem::temp_directory_path();
        for (bool osmand : {false, true}) {
            std::string path = (tmp / (osmand ? "otk_osmand.sqlitedb" : "otk_rmaps.sqlite")).string();
            makeArchive(path, osmand);
            ImageryLayer l;
            l.type = SourceType::Osmdroid;
            l.path = path;
            auto p = makeProvider(l, nullptr, nullptr);
            FetchResult r = p->fetch(5, 3, 7);
            check(r.status == 200 && r.data.size() == 4 && p->matrix().minZoom == 5 && p->matrix().maxZoom == 5,
                  osmand ? "osmand sqlitedb archive" : "rmaps bigplanet sqlite archive");
        }
    }

    {
        auto dir = std::filesystem::temp_directory_path() / "otk_netlink";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "sub");
        auto write = [](const std::filesystem::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; };
        write(dir / "sub" / "data.kml",
              "<kml><Document><Placemark><name>Inner</name><Point><coordinates>10,20</coordinates></Point></Placemark></Document></kml>");
        write(dir / "root.kml",
              "<kml><Document><Placemark><name>Outer</name><Point><coordinates>1,2</coordinates></Point></Placemark>"
              "<NetworkLink><Link><href>sub/data.kml</href></Link></NetworkLink></Document></kml>");
        ImportResult local = importData((dir / "root.kml").string());
        bool inner = false;
        for (const auto& f : local.data.features) inner = inner || f.name == "Inner";
        check(local.data.features.size() == 2 && inner, "kml network link to a local file");
        std::string wrapper = "<kml><NetworkLink><Link><href>sub/data.kml</href></Link></NetworkLink></kml>";
        std::filesystem::remove(dir / "wrapper.kmz");
        mz_zip_add_mem_to_archive_file_in_place((dir / "wrapper.kmz").string().c_str(), "doc.kml", wrapper.data(), wrapper.size(), nullptr, 0,
                                                MZ_BEST_COMPRESSION);
        ImportResult kmz = importData((dir / "wrapper.kmz").string());
        check(kmz.data.features.size() == 1 && kmz.data.features[0].name == "Inner", "kmz network link to a file beside it");
        if (const char* base = std::getenv("OTK_TEST_HTTP")) {
            write(dir / "remote.kml", std::string("<kml><NetworkLink><Link><href>") + base + "/remote/doc.kml</href></Link></NetworkLink></kml>");
            ImportResult remote = importData((dir / "remote.kml").string());
            bool nested = false, icon = false;
            for (const auto& f : remote.data.features) {
                nested = nested || f.name == "Nested";
                icon = icon || (f.style.iconImage && f.style.iconImage->width == 16);
            }
            check(remote.data.features.size() == 3 && nested && icon, "kml network links over http with relative links and icons");
        }
    }

    {
        auto zipPath = (std::filesystem::temp_directory_path() / "otk_iconset.zip").string();
        std::filesystem::remove(zipPath);
        std::vector<std::uint8_t> pixels(16 * 16 * 4, 255);
        size_t pngSize = 0;
        void* png = tdefl_write_image_to_png_file_in_memory(pixels.data(), 16, 16, 4, &pngSize);
        std::string xml = "<iconset version=\"1\" name=\"Test\" uid=\"test-set-uid\"><icon name=\"pin.png\" groupName=\"Grp\"/></iconset>";
        mz_zip_add_mem_to_archive_file_in_place(zipPath.c_str(), "iconset.xml", xml.data(), xml.size(), nullptr, 0, MZ_BEST_COMPRESSION);
        mz_zip_add_mem_to_archive_file_in_place(zipPath.c_str(), "Grp/pin.png", png, pngSize, nullptr, 0, MZ_BEST_COMPRESSION);
        mz_free(png);
        check(registerIconset(zipPath) == "test-set-uid", "atak iconset registers");
        std::string cot =
            "<events>"
            "<event version=\"2.0\" uid=\"m1\" type=\"a-f-G\" time=\"2026-10-21T00:00:00Z\" start=\"2026-10-21T00:00:00Z\" stale=\"2026-10-22T00:00:00Z\" how=\"h-e\">"
            "<point lat=\"10\" lon=\"20\" hae=\"9999999\" ce=\"9999999\" le=\"9999999\"/>"
            "<detail><usericon iconsetpath=\"test-set-uid/Grp/pin.png\"/></detail></event>"
            "<event version=\"2.0\" uid=\"rb\" type=\"u-rb-a\" time=\"2026-10-21T00:00:00Z\" start=\"2026-10-21T00:00:00Z\" stale=\"2026-10-22T00:00:00Z\" how=\"h-e\">"
            "<point lat=\"0\" lon=\"0\" hae=\"9999999\" ce=\"9999999\" le=\"9999999\"/>"
            "<detail><range value=\"1000\"/><bearing value=\"90\"/><color value=\"-65536\"/></detail></event>"
            "<event version=\"2.0\" uid=\"d\" type=\"u-d-f\" time=\"2026-10-21T00:00:00Z\" start=\"2026-10-21T00:00:00Z\" stale=\"2026-10-22T00:00:00Z\" how=\"h-e\">"
            "<point lat=\"0\" lon=\"0\" hae=\"9999999\" ce=\"9999999\" le=\"9999999\"/>"
            "<detail><link point=\"0,0\"/><link point=\"0,1\"/><strokeStyle value=\"dashed\"/></detail></event>"
            "</events>";
        ImportResult r = importBytes(std::vector<std::uint8_t>(cot.begin(), cot.end()), "cot");
        bool icon = false, rb = false, dashed = false;
        for (const auto& f : r.data.features) {
            if (f.uid == "m1") icon = f.style.iconImage && f.style.iconImage->width == 16;
            if (f.uid == "rb" && !f.geometries.empty() && f.geometries[0].points.size() == 2) {
                const Coord& e = f.geometries[0].points[1];
                rb = std::abs(e.lat) < 1e-6 && std::abs(e.lon * 111319.49 - 1000.0) < 2.0 && f.style.strokeColor == 0xFFFF0000u;
            }
            if (f.uid == "d") dashed = f.style.dash == 12;
        }
        check(icon, "cot usericon from a registered iconset");
        check(rb, "cot range and bearing line");
        check(dashed, "cot dashed stroke style");
        clearIconsets();
    }

    {
        struct Case {
            const char* name;
            const char* wkt;
            double x, y, lat, lon;
        };
        const Case cases[] = {
            {"utm 10n",
             "PROJCS[\"WGS_1984_UTM_Zone_10N\",GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],PRIMEM[\"Greenwich\",0.0],"
             "UNIT[\"Degree\",0.0174532925199433]],PROJECTION[\"Transverse_Mercator\"],PARAMETER[\"False_Easting\",500000.0],PARAMETER[\"False_Northing\",0.0],"
             "PARAMETER[\"Central_Meridian\",-123.0],PARAMETER[\"Scale_Factor\",0.9996],PARAMETER[\"Latitude_Of_Origin\",0.0],UNIT[\"Meter\",1.0]]",
             647740.0771481959, 5373807.167313857, 48.5, -121.0},
            {"washington south state plane feet",
             "PROJCS[\"NAD_1983_HARN_StatePlane_Washington_South_FIPS_4602_Feet\",GEOGCS[\"GCS_North_American_1983_HARN\",DATUM[\"D_North_American_1983_HARN\","
             "SPHEROID[\"GRS_1980\",6378137.0,298.257222101]],PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],PROJECTION[\"Lambert_Conformal_Conic\"],"
             "PARAMETER[\"False_Easting\",1640416.667],PARAMETER[\"False_Northing\",0.0],PARAMETER[\"Central_Meridian\",-120.5],"
             "PARAMETER[\"Standard_Parallel_1\",47.3333333333333],PARAMETER[\"Standard_Parallel_2\",45.8333333333333],"
             "PARAMETER[\"Latitude_Of_Origin\",45.3333333333333],UNIT[\"US survey foot\",0.304800609601219]]",
             1041925.6491431455, 630715.9771515217, 47.0379, -122.9007},
            {"conus albers",
             "PROJCS[\"NAD_1983_Contiguous_USA_Albers\",GEOGCS[\"GCS_North_American_1983\",DATUM[\"D_North_American_1983\",SPHEROID[\"GRS_1980\",6378137.0,298.257222101]],"
             "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],PROJECTION[\"Albers\"],PARAMETER[\"False_Easting\",0.0],PARAMETER[\"False_Northing\",0.0],"
             "PARAMETER[\"Central_Meridian\",-96.0],PARAMETER[\"Standard_Parallel_1\",29.5],PARAMETER[\"Standard_Parallel_2\",45.5],PARAMETER[\"Latitude_Of_Origin\",23.0],"
             "UNIT[\"Meter\",1.0]]",
             -2103628.9104909617, 2863368.319785516, 46.0, -123.5},
            {"web mercator",
             "PROJCS[\"WGS_1984_Web_Mercator_Auxiliary_Sphere\",GEOGCS[\"GCS_WGS_1984\",DATUM[\"D_WGS_1984\",SPHEROID[\"WGS_1984\",6378137.0,298.257223563]],"
             "PRIMEM[\"Greenwich\",0.0],UNIT[\"Degree\",0.0174532925199433]],PROJECTION[\"Mercator_Auxiliary_Sphere\"],PARAMETER[\"False_Easting\",0.0],"
             "PARAMETER[\"False_Northing\",0.0],PARAMETER[\"Central_Meridian\",0.0],PARAMETER[\"Standard_Parallel_1\",0.0],PARAMETER[\"Auxiliary_Sphere_Type\",0.0],"
             "UNIT[\"Meter\",1.0]]",
             -13681243.342136879, 5948262.5126702115, 47.0379, -122.9007},
        };
        for (const Case& k : cases) {
            PrjTransform p = parsePrj(k.wkt);
            double lat = 0, lon = 0;
            bool ok = p.projected() && p.inverse(k.x, k.y, lat, lon) && std::abs(lat - k.lat) < 1e-7 && std::abs(lon - k.lon) < 1e-7;
            char msg[160];
            std::snprintf(msg, sizeof(msg), "prj inverse %s (%.9f, %.9f)", k.name, lat, lon);
            check(ok, msg);
        }
    }

    {
        auto glbJson = [](const std::vector<std::uint8_t>& glb) {
            if (glb.size() < 20 || std::memcmp(glb.data(), "glTF", 4) != 0) return nlohmann::json();
            std::uint32_t len = 0;
            std::memcpy(&len, glb.data() + 12, 4);
            return nlohmann::json::parse(glb.begin() + 20, glb.begin() + 20 + len, nullptr, false);
        };
        std::vector<std::uint8_t> pixels(4 * 4 * 4, 200);
        size_t pngSize = 0;
        void* png = tdefl_write_image_to_png_file_in_memory(pixels.data(), 4, 4, 4, &pngSize);
        std::vector<std::uint8_t> pngBytes(static_cast<std::uint8_t*>(png), static_cast<std::uint8_t*>(png) + pngSize);
        mz_free(png);
        std::string mtl = "newmtl skin\nKd 1 0 0\nmap_Kd tex/skin.png\n";
        std::string obj = "mtllib quad.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nusemtl skin\nf 1/1 2/2 3/3 4/4\n";
        AssetReader read = [&](const std::string& f) {
            if (f == "quad.mtl") return std::vector<std::uint8_t>(mtl.begin(), mtl.end());
            if (f == "tex/skin.png") return pngBytes;
            return std::vector<std::uint8_t>{};
        };
        std::vector<std::uint8_t> glb = objToGlb(obj, read);
        nlohmann::json j = glbJson(glb);
        CpuModel cpu;
        bool parsed = !glb.empty() && parseGltf(glb.data(), glb.size(), "quad.glb", {}, cpu);
        bool ok = parsed && j.is_object() && j["accessors"][0]["count"] == 4 && j["accessors"][j["meshes"][0]["primitives"][0]["indices"].get<int>()]["count"] == 6 &&
                  j.contains("images") && j["images"].size() == 1 && cpu.images.size() == 1;
        check(ok, "obj with mtl and texture converts to glTF");

        std::string dae =
            "<COLLADA><asset><unit meter=\"0.0254\"/><up_axis>Z_UP</up_axis></asset>"
            "<library_effects><effect id=\"fx\"><profile_COMMON><technique sid=\"c\"><lambert><diffuse><color>0 1 0 1</color></diffuse></lambert></technique>"
            "</profile_COMMON></effect></library_effects>"
            "<library_materials><material id=\"mat\"><instance_effect url=\"#fx\"/></material></library_materials>"
            "<library_geometries><geometry id=\"g\"><mesh>"
            "<source id=\"p\"><float_array count=\"9\">0 0 0 10 0 0 0 10 0</float_array><technique_common><accessor count=\"3\" stride=\"3\"/></technique_common></source>"
            "<vertices id=\"v\"><input semantic=\"POSITION\" source=\"#p\"/></vertices>"
            "<triangles count=\"1\" material=\"sym\"><input semantic=\"VERTEX\" source=\"#v\" offset=\"0\"/><p>0 1 2</p></triangles>"
            "</mesh></geometry></library_geometries>"
            "<library_visual_scenes><visual_scene id=\"s\"><node><translate>0 0 5</translate>"
            "<instance_geometry url=\"#g\"><bind_material><technique_common><instance_material symbol=\"sym\" target=\"#mat\"/></technique_common></bind_material>"
            "</instance_geometry></node></visual_scene></library_visual_scenes><scene><instance_visual_scene url=\"#s\"/></scene></COLLADA>";
        std::vector<std::uint8_t> dglb = colladaToGlb(dae, {});
        nlohmann::json dj = glbJson(dglb);
        bool dok = dj.is_object() && dj["accessors"][0]["count"] == 3;
        if (dok) {
            auto mn = dj["accessors"][0]["min"], mx = dj["accessors"][0]["max"];
            dok = std::abs(mn[0].get<double>()) < 1e-6 && std::abs(mn[1].get<double>() - 0.127) < 1e-6 && std::abs(mn[2].get<double>() + 0.254) < 1e-6 &&
                  std::abs(mx[0].get<double>() - 0.254) < 1e-6 && std::abs(mx[1].get<double>() - 0.127) < 1e-6 && std::abs(mx[2].get<double>()) < 1e-6 &&
                  std::abs(dj["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"][1].get<double>() - 1.0) < 1e-6;
        }
        check(dok, "collada z-up inches with node transform converts to glTF");

        std::string texDae =
            "<COLLADA><asset><unit meter=\"1\"/><up_axis>Z_UP</up_axis></asset>"
            "<library_images><image id=\"img\"><init_from>../images/tex.png</init_from></image></library_images>"
            "<library_effects><effect id=\"fx\"><profile_COMMON>"
            "<newparam sid=\"surf\"><surface type=\"2D\"><init_from>img</init_from></surface></newparam>"
            "<newparam sid=\"samp\"><sampler2D><source>surf</source></sampler2D></newparam>"
            "<technique sid=\"c\"><lambert><diffuse><texture texture=\"samp\" texcoord=\"UV\"/></diffuse></lambert></technique>"
            "</profile_COMMON></effect></library_effects>"
            "<library_materials><material id=\"mat\"><instance_effect url=\"#fx\"/></material></library_materials>"
            "<library_geometries><geometry id=\"g\"><mesh>"
            "<source id=\"p\"><float_array count=\"9\">0 0 0 10 0 0 0 10 0</float_array><technique_common><accessor count=\"3\" stride=\"3\"/></technique_common></source>"
            "<source id=\"t\"><float_array count=\"6\">0 0 1 0 0 1</float_array><technique_common><accessor count=\"3\" stride=\"2\"/></technique_common></source>"
            "<vertices id=\"v\"><input semantic=\"POSITION\" source=\"#p\"/></vertices>"
            "<triangles count=\"1\" material=\"sym\"><input semantic=\"VERTEX\" source=\"#v\" offset=\"0\"/>"
            "<input semantic=\"TEXCOORD\" source=\"#t\" offset=\"1\" set=\"0\"/><p>0 0 1 1 2 2</p></triangles>"
            "</mesh></geometry></library_geometries>"
            "<library_visual_scenes><visual_scene id=\"s\"><node>"
            "<instance_geometry url=\"#g\"><bind_material><technique_common><instance_material symbol=\"sym\" target=\"#mat\"/></technique_common></bind_material>"
            "</instance_geometry></node></visual_scene></library_visual_scenes><scene><instance_visual_scene url=\"#s\"/></scene></COLLADA>";
        std::string modelKml =
            "<kml><Placemark><name>Tower</name><Model><altitudeMode>relativeToGround</altitudeMode>"
            "<Location><longitude>-122.9</longitude><latitude>47.04</latitude><altitude>5</altitude></Location>"
            "<Orientation><heading>45</heading><tilt>0</tilt><roll>0</roll></Orientation><Scale><x>2</x><y>2</y><z>2</z></Scale>"
            "<Link><href>models/tower.dae</href></Link></Model></Placemark></kml>";
        auto kmzPath = (std::filesystem::temp_directory_path() / "otk_model.kmz").string();
        std::filesystem::remove(kmzPath);
        mz_zip_add_mem_to_archive_file_in_place(kmzPath.c_str(), "doc.kml", modelKml.data(), modelKml.size(), nullptr, 0, MZ_BEST_COMPRESSION);
        mz_zip_add_mem_to_archive_file_in_place(kmzPath.c_str(), "models/tower.dae", texDae.data(), texDae.size(), nullptr, 0, MZ_BEST_COMPRESSION);
        mz_zip_add_mem_to_archive_file_in_place(kmzPath.c_str(), "images/tex.png", pngBytes.data(), pngBytes.size(), nullptr, 0, MZ_BEST_COMPRESSION);
        ImportResult kmzModel = importData(kmzPath);
        bool mok = kmzModel.data.models.size() == 1;
        if (mok) {
            const ModelInstance& m = kmzModel.data.models[0];
            nlohmann::json mj = glbJson(m.glb);
            mok = m.name == "Tower" && std::abs(m.placement.position.lat - 47.04) < 1e-9 && std::abs(m.placement.position.lon + 122.9) < 1e-9 &&
                  m.placement.altitudeMode == AltitudeMode::RelativeToGround && std::abs(m.placement.heading - 45) < 1e-9 &&
                  std::abs(m.placement.scale - 2) < 1e-9 && mj.is_object() && mj.contains("images") && mj["images"].size() == 1;
        }
        check(mok, "kmz model placemark with collada and texture");
    }

    {
        std::string wms = "<customWmsMapSource><name>W</name><url>https://h/wms</url><layers>a</layers></customWmsMapSource>";
        check(detectFormat(std::vector<std::uint8_t>(wms.begin(), wms.end()), "w.xml") == "mobac", "mobac wms source detection");
        std::string lj = "{\"tilejson\":\"2.1.0\",\"format\":\"quantized-mesh-1.0\",\"tiles\":[\"{z}/{x}/{y}.terrain\"]}";
        check(detectFormat(std::vector<std::uint8_t>(lj.begin(), lj.end()), "layer.json") == "quantized-mesh", "cesium layer.json detection");
        std::string pm = std::string("PMTiles\x03", 8) + std::string(119, '\0');
        check(detectFormat(std::vector<std::uint8_t>(pm.begin(), pm.end()), "archive.bin") == "pmtiles", "pmtiles detection by content");
    }

    {
        double t = parseIsoTime("2026-10-19T12:34:56.789Z");
        check(formatIsoTime(t) == "2026-10-19T12:34:56.789Z", "iso time round trip");
        check(std::abs(parseIsoTime("2026-10-19T14:34:56.789+02:00") - t) < 1e-6, "iso time offset");
        check(std::abs(parseIsoTime("1969-12-31T23:59:59Z") + 1.0) < 1e-9, "iso time before epoch");
    }
    {
        ImportResult cot = importData(data("sample.cot"));
        bool ok = cot.ok && cot.format == "cot" && cot.data.features.size() == 2;
        if (ok) {
            const Feature& a = cot.data.features[0];
            const Feature& b = cot.data.features[1];
            ok = a.uid == "ANDROID-123" && a.type == "a-f-G-U-C" && a.name == "Rainier Team" && a.description == "Summit check-in" &&
                 a.geometries[0].type == GeometryType::Point && std::abs(a.geometries[0].points[0].alt - 4392.0) < 1e-6 &&
                 a.altitudeMode == AltitudeMode::Absolute && a.style.iconColor == 0xFFFF0000u &&
                 formatIsoTime(a.stale) == "2026-10-19T12:05:00.000Z" && b.geometries[0].type == GeometryType::Polygon &&
                 b.geometries[0].points.size() == 4 && b.style.strokeColor == 0xFF0000FFu;
        }
        check(ok, "cot import (marker and drawn polygon)");
        ExportResult er;
        std::vector<std::uint8_t> out = exportBytes(cot.data.features, "cot", &er);
        ImportResult back = importBytes(out, "cot");
        bool same = back.ok && back.data.features.size() == 2 && back.data.features[0].uid == "ANDROID-123" &&
                    back.data.features[0].type == "a-f-G-U-C" && back.data.features[1].geometries[0].points.size() == 4 &&
                    std::abs(back.data.features[0].time - cot.data.features[0].time) < 1e-3;
        check(same, "cot round trip");
        ExportResult pr;
        std::vector<std::uint8_t> pkg = exportBytes(cot.data.features, "tak-package", &pr);
        check(detectFormat(pkg, "export.zip") == "tak-package", "data package detection");
        ImportResult unpacked = importBytes(pkg, "tak-package");
        bool pkgOk = unpacked.ok && unpacked.data.features.size() == 2;
        if (pkgOk) {
            pkgOk = (unpacked.data.features[0].uid == "ANDROID-123" || unpacked.data.features[1].uid == "ANDROID-123");
        }
        check(pkgOk, "data package round trip");
    }
    {
        Feature f;
        f.uid = "uid-7";
        f.type = "a-h-G";
        f.name = "Hostile";
        f.description = "Seen <near> \"ridge\" & road";
        f.time = parseIsoTime("2026-01-02T03:04:05Z");
        f.start = f.time;
        f.stale = f.time + 600;
        f.altitudeMode = AltitudeMode::Absolute;
        setAttribute(f, "speed", "12.5");
        setAttribute(f, "unit", "A1");
        f.geometries.push_back({GeometryType::Polygon, {{46, -122, 10}, {46, -121, 10}, {47, -121, 10}, {47, -122, 10}}, {{{46.4, -121.6, 10}, {46.4, -121.4, 10}, {46.6, -121.4, 10}}}});
        std::vector<Feature> one{f};
        for (const char* fmt : {"geojson", "kml", "gml", "csv"}) {
            ExportResult r;
            std::vector<std::uint8_t> bytes = exportBytes(one, fmt, &r);
            ImportResult in = importBytes(bytes, detectFormat(bytes, std::string("x.") + fmt));
            bool ok = in.ok && in.data.features.size() == 1;
            if (ok) {
                const Feature& g = in.data.features[0];
                const std::string* unit = findAttribute(g, "unit");
                ok = g.uid == "uid-7" && g.type == "a-h-G" && g.name == "Hostile" && g.description == f.description && unit && *unit == "A1" &&
                     g.geometries.size() == 1 && g.geometries[0].type == GeometryType::Polygon && g.geometries[0].points.size() == 4 &&
                     g.geometries[0].holes.size() == 1 && std::abs(g.time - f.time) < 1e-3;
            }
            std::string what = std::string(fmt) + " round trip keeps uid, type, attributes, time, holes";
            check(ok, what.c_str());
        }
        ExportResult gr;
        std::vector<std::uint8_t> gpx = exportBytes(one, "gpx", &gr);
        check(!gr.lossy.empty() && importBytes(gpx, "gpx").data.features.size() == 1, "gpx export reports lossy conversion");
    }
    {
        ImportResult csv = importData(data("points.csv"));
        bool ok = csv.ok && csv.data.features.size() == 2 && csv.data.features[0].name == "Camp, Muir" &&
                  std::abs(csv.data.features[0].geometries[0].points[0].alt - 3100) < 1e-9;
        const std::string* unit = ok ? findAttribute(csv.data.features[0], "unit") : nullptr;
        check(ok && unit && *unit == "Alpha" && csv.data.features[0].description == "says \"hi\"", "csv import with quoted fields and attributes");
        std::vector<Geometry> g;
        bool parsed = parseWkt("MULTIPOLYGON (((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 4 2, 4 4, 2 2)), ((20 20, 30 20, 30 30, 20 20)))", g);
        check(parsed && g.size() == 2 && g[0].holes.size() == 1 && g[1].points.size() == 3, "wkt multipolygon");
        std::vector<Geometry> back;
        check(parseWkt(writeWkt(g), back) && back.size() == 2 && back[0].holes.size() == 1, "wkt round trip");
        std::vector<Geometry> empty;
        check(parseWkt("POINT EMPTY", empty) && empty.empty(), "wkt empty");
    }
    {
        ImportResult gml = importData(data("sample.gml"));
        bool ok = gml.ok && gml.format == "gml" && gml.data.features.size() == 2;
        if (ok) {
            const Coord& c = gml.data.features[0].geometries[0].points[0];
            const std::string* h = findAttribute(gml.data.features[0], "height");
            ok = std::abs(c.lat - 46.8529) < 1e-9 && std::abs(c.lon + 121.7604) < 1e-9 && h && *h == "4392" &&
                 gml.data.features[1].geometries[0].holes.size() == 1;
        }
        check(ok, "gml import with EPSG:4326 axis order and holes");
    }
    {
        std::string md = formatsMarkdown();
        if (const char* out = std::getenv("OTK_FORMATS_MD")) std::ofstream(out, std::ios::binary) << md;
        check(md.find("`tak-package`") != std::string::npos && md.find("`cot`") != std::string::npos, "compatibility matrix");
        std::string evil = "PK";
        ImportLimits tight;
        tight.maxArchiveEntries = 1;
        Feature a;
        a.name = "a";
        a.geometries.push_back({GeometryType::Point, {{1, 2, 0}}, {}});
        std::vector<Feature> two{a, a};
        two[1].uid = "b";
        std::vector<std::uint8_t> pkg = exportBytes(two, "tak-package");
        ImportResult limited = importBytes(pkg, "tak-package", tight);
        check(!limited.ok && !limited.error.empty(), "archive entry limit enforced");
    }
    {
        const std::string host = "http://unit-unreachable.invalid";
        std::uint64_t epoch = net::reachabilityEpoch();
        for (int i = 0; i < 3; ++i) net::report(host + "/1/0/0.png", 0);
        bool down = !net::hostAvailable(host + "/2/0/0.png") && net::hostsDown() == 1 && !net::admit(host + "/2/1/1.png");
        net::report(host + "/1/0/0.png", 200);
        check(down && net::hostAvailable(host + "/x") && net::hostsDown() == 0 && net::reachabilityEpoch() == epoch + 1,
              "unreachable host fails fast and recovers");
        check(net::hostKey("HTTPS://Tiles.Example.com:8443/a/b?c=d") == "https://tiles.example.com:8443", "host key");
    }
    check(stripCredentials("https://t.example/{z}/{x}/{y}.png?key=abc&style=dark&access_token=x") ==
              "https://t.example/{z}/{x}/{y}.png?style=dark" &&
              stripCredentials("https://t.example/{z}?apikey=1") == "https://t.example/{z}",
          "cache key ignores credentials");
    {
        auto layers = loadMobac("<customMapSource><name>n</name><url>http://a/{$z}/{$x}/{$y}</url><tileUpdate>0</tileUpdate></customMapSource>");
        auto hourly = loadMobac("<customMapSource><name>n</name><url>http://a/{$z}/{$x}/{$y}</url><tileUpdate>3600000</tileUpdate></customMapSource>");
        check(layers.size() == 1 && layers[0].expirySeconds < 0 && hourly.size() == 1 && hourly[0].expirySeconds == 3600,
              "mobac tileUpdate");
    }
    {
        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path() / "otk_unit_cache";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
        {
            TileCache cache;
            bool opened = cache.open((dir / "tiles.sqlite").string());
            std::vector<std::uint8_t> blob(64 * 1024, 7);
            cache.put("a", 3, 1, 1, blob, TileCache::now() + 100, true);
            for (int i = 0; i < 64; ++i) cache.put("a", 10, i, 0, blob, TileCache::now() + 100);
            cache.setLimit(1 << 20);
            std::vector<std::uint8_t> got;
            std::int64_t exp = 0;
            int left = 0;
            for (int i = 0; i < 64; ++i) left += cache.has("a", 10, i, 0) ? 1 : 0;
            check(opened && cache.get("a", 3, 1, 1, got, exp) && got.size() == blob.size() && left < 64 && cache.bytes() <= (1 << 20) + 128 * 1024,
                  "cache limit evicts oldest, keeps pinned");
            cache.put("a", 3, 1, 1, std::vector<std::uint8_t>(10, 1), TileCache::now() + 100, false);
            cache.setLimit(1);
            check(cache.get("a", 3, 1, 1, got, exp) && got.size() == 10, "refresh keeps pin");
            cache.setLimit(0);
        }
        {
            std::ofstream(dir / "garbage.sqlite", std::ios::binary) << std::string(4096, 'x');
            TileCache cache;
            bool ok = cache.open((dir / "garbage.sqlite").string());
            cache.put("b", 0, 0, 0, {1, 2, 3}, TileCache::now() + 100);
            check(ok && cache.has("b", 0, 0, 0), "corrupt cache file replaced");
        }
        {
            TileCache cache;
            cache.open((dir / "stale.sqlite").string());
            std::atomic<bool> offline{false};
            ImageryLayer layer;
            layer.url = "http://unit-stale.invalid/{z}/{x}/{y}.png";
            auto provider = makeProvider(layer, &cache, &offline);
            std::string key = layerCacheKey(layer);
            cache.put(key, 4, 2, 3, {9, 9, 9}, TileCache::now() - 10);
            FetchResult r;
            bool local = provider->fetchLocal(4, 2, 3, r);
            check(local && r.status == 200 && r.stale && r.data.size() == 3, "expired tile served immediately");
            for (int i = 0; i < 3; ++i) net::report("http://unit-stale.invalid/0/0/0.png", 0);
            FetchResult miss;
            bool fast = provider->fetchLocal(5, 0, 0, miss);
            check(fast && miss.status == -3 && !provider->available(5, 0, 0), "uncached tile fails fast while host is down");
            offline = true;
            FetchResult off;
            check(provider->fetchLocal(5, 1, 1, off) && off.status == -2, "offline miss");
            net::resetReachability();
        }
        {
            TileCache cache;
            cache.open((dir / "local.sqlite").string());
            std::atomic<bool> offline{false};
            ImageryLayer layer;
            layer.type = SourceType::Mvt;
            layer.url = "http://unit-local.invalid/{z}/{x}/{y}.pbf";
            layer.stream = false;
            layer.maxRequests = 4;
            auto provider = makeProvider(layer, &cache, &offline);
            cache.put(layerCacheKey(layer), 6, 1, 2, {4, 5, 6}, TileCache::now() - 10, true);
            FetchResult hit, miss;
            const bool cachedLocal = provider->fetchLocal(6, 1, 2, hit);
            const bool missLocal = provider->fetchLocal(6, 3, 3, miss);
            const FetchResult direct = provider->fetch(6, 3, 3);
            check(cachedLocal && hit.status == 200 && !hit.stale && missLocal && miss.status == -2 && direct.status == -2 &&
                      provider->maxConcurrent() == 4 && !provider->refresh(6, 1, 2),
                  "local-only layer reads downloads and never streams");
        }
        {
            TileCache cache;
            cache.open((dir / "export.sqlite").string());
            TileLoader loader(1, 1);
            loader.setCache(&cache);
            ImageryLayer layer;
            layer.name = "Unit";
            layer.url = "http://unit-export.invalid/{z}/{x}/{y}.png";
            loader.setLayers({layer});
            std::string key = layerCacheKey(layer);
            std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', 1, 2, 3, 4};
            int stored = 0;
            for (int z = 0; z <= 3; ++z)
                for (int n = 0; n < (1 << (2 * z)); ++n, ++stored) cache.put(key, z, n % (1 << z), n / (1 << z), png, TileCache::now() + 100);
            std::string err;
            std::int64_t mb = loader.exportTiles(0, {-180, -85, 180, 85}, 0, 3, (dir / "out.mbtiles").string(), err);
            std::int64_t gp = loader.exportTiles(0, {0, 0, 180, 85}, 0, 3, (dir / "out.gpkg").string(), err);
            TileCache other;
            other.open((dir / "import.sqlite").string());
            TileLoader target(1, 1);
            target.setCache(&other);
            target.setLayers({layer});
            std::int64_t in = target.importTiles(0, (dir / "out.mbtiles").string(), err);
            std::vector<std::uint8_t> got;
            std::int64_t exp = 0;
            bool roundtrip = other.get(key, 3, 5, 2, got, exp) && got == png;
            TileMatrix gm;
            std::int64_t gcount = 0;
            readTilePackage((dir / "out.gpkg").string(), gm, [&](int, int, int, const std::vector<std::uint8_t>&) {
                ++gcount;
                return true;
            }, err);
            check(mb == stored && in == stored && roundtrip && gp > 0 && gcount == gp && sameTiling(gm, TileMatrix::webMercator(0, 3)),
                  "export to MBTiles/GeoPackage and import into the cache");
        }
        fs::remove_all(dir, ec);
    }
    {
        auto dms = [](double d, double m, double s) { return (d < 0 ? -1 : 1) * (std::abs(d) + m / 60.0 + s / 3600.0); };
        double la1 = dms(-37, 57, 3.72030), lo1 = dms(144, 25, 29.52440), la2 = dms(-37, 39, 10.15610), lo2 = dms(143, 55, 35.38390);
        GeodesicLine g = geodesicInverse(la1, lo1, la2, lo2);
        check(std::abs(g.distance - 54972.271) < 0.01 && std::abs(g.initialBearing - dms(306, 52, 5.37)) < 1e-4 &&
                  std::abs(g.finalBearing - dms(307, 10, 25.07)) < 1e-4,
              "vincenty inverse (Flinders Peak - Buninyong)");
        double fb = 0;
        Coord d = geodesicDirect({la1, lo1, 0}, g.initialBearing, g.distance, &fb);
        check(std::abs(d.lat - la2) < 1e-8 && std::abs(d.lon - lo2) < 1e-8 && std::abs(fb - g.finalBearing) < 1e-6, "vincenty direct");
        GeodesicLine anti = geodesicInverse(0.0, 0.0, 0.5, 179.7);
        check(anti.distance > 19900000 && anti.distance < 20100000, "near-antipodal fallback");
        std::vector<Coord> square{{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}};
        double area = geodesicArea(square);
        check(std::abs(area - 1.2308e10) / 1.2308e10 < 0.01, "geodesic area of a 1 degree cell");
    }
    {
        struct T {
            double year, km, lat, lon, decl, incl;
        };
        const T tests[] = {{2025.0, 28, 89, -121, -99.77, 88.47}, {2025.0, 65, 43, 93, 0.50, 64.10},  {2025.0, 51, -33, 109, -5.49, -67.50},
                           {2025.5, 69, 38, -144, 12.93, 56.97}, {2025.5, 50, -70, -133, 57.21, -71.94}, {2026.0, 46, -24, -122, 14.01, -34.17}};
        bool ok = true;
        for (const T& t : tests) {
            MagneticField m = magneticField(t.lat, t.lon, t.km * 1000.0, t.year);
            if (std::abs(m.declination - t.decl) > 0.011 || std::abs(m.inclination - t.incl) > 0.011) {
                ok = false;
                std::printf("     wmm %.1f %g %g: D %.3f (want %.2f) I %.3f (want %.2f)\n", t.year, t.lat, t.lon, m.declination, t.decl,
                            m.inclination, t.incl);
            }
        }
        check(ok, "WMM2025 declination and inclination match NOAA test values");
        check(std::abs(gridConvergence(47.0, -123.0)) < 1e-9 && std::abs(gridConvergence(47.0, -122.9) - 0.1 * std::sin(47.0 * 3.14159265358979 / 180)) < 1e-4,
              "grid convergence");
        double t = 100.0;
        bool refs = std::abs(bearingToTrue(bearingFromTrue(t, 47, -122.9, NorthReference::MagneticNorth), 47, -122.9, NorthReference::MagneticNorth) - t) < 1e-9 &&
                    std::abs(bearingFromTrue(t, 47, -122.9, NorthReference::MagneticNorth) - (t - magneticDeclination(47, -122.9))) < 1e-9;
        check(refs, "true/magnetic/grid bearing conversion");
        check(formatBearing(90, AngleUnit::Mils, NorthReference::MagneticNorth) == "1600 mils M" && std::abs(degreesToMils(180) - 3200) < 1e-9 &&
                  formatBearing(5.4) == "005\xC2\xB0T",
              "bearing formatting in degrees and NATO mils");
        check(formatDistance(850) == "850 m" && formatDistance(12345) == "12.3 km" && formatDistance(1852, DistanceUnit::Nautical) == "1.00 NM",
              "distance formatting");
    }
    {
        check(toMgrs(90.0, 0.0, 5) == "Z AH 00000 00000" && toMgrs(-90.0, 0.0, 5) == "B AN 00000 00000", "polar MGRS at the poles");
        bool ok = true;
        for (auto [la, lo] : {std::pair{86.0, 45.0}, {88.5, -120.0}, {-85.0, -120.0}, {-81.0, 10.0}, {84.5, 179.0}}) {
            double rl = 0, ro = 0;
            std::string m = toMgrs(la, lo, 5);
            ok = ok && !m.empty() && fromMgrs(m, rl, ro) && length(geodeticToEcef(rl, ro, 0) - geodeticToEcef(la, lo, 0)) < 2.0;
            if (!ok) std::printf("     polar %s -> %f %f\n", m.c_str(), rl, ro);
        }
        check(ok, "polar MGRS (UPS) roundtrip");
        check(toGars(0.0, 0.0) == "361HN37" && toGars(-90.0, -180.0, 1) == "001AA" && toGars(89.99, 179.99, 1) == "720QZ", "GARS cells");
        bool garsOk = true;
        for (auto [la, lo] : {std::pair{47.04, -122.9}, {-33.87, 151.21}, {0.01, 0.01}, {64.2, -21.9}}) {
            double rl = 0, ro = 0;
            garsOk = garsOk && fromGars(toGars(la, lo), rl, ro) && std::abs(rl - la) <= 0.25 / 6 + 1e-9 && std::abs(ro - lo) <= 0.25 / 6 + 1e-9;
        }
        check(garsOk, "GARS roundtrip");
        struct P {
            const char* text;
            double lat, lon;
        };
        const P parses[] = {{"47.04, -122.9", 47.04, -122.9},
                            {"-33.87 151.21", -33.87, 151.21},
                            {"33.87 -151.21", 33.87, -151.21},
                            {"47.04N 122.9W", 47.04, -122.9},
                            {"N 47\xC2\xB0 02.4000' W 122\xC2\xB0 54.0000'", 47.04, -122.9},
                            {"47 02 24 N 122 54 00 W", 47.04, -122.9},
                            {"S33 52 12 E151 12 36", -33.87, 151.21},
                            {"10T ES 94499 89563", 46.8529, -121.7604},
                            {"361HN37", 0.0417, 0.0417}};
        bool parseOk = true;
        for (const P& p : parses) {
            double la = 0, lo = 0;
            bool ok = parseCoordinate(p.text, la, lo) && std::abs(la - p.lat) < 1e-3 && std::abs(lo - p.lon) < 1e-3;
            if (!ok) std::printf("     parse '%s' -> %f %f\n", p.text, la, lo);
            parseOk = parseOk && ok;
        }
        check(parseOk, "coordinate parsing (DD, DM, DMS, MGRS, GARS)");
        bool fmtOk = true;
        for (auto [pla, plo] : {std::pair{46.8529, -121.7604}, {87.5, 33.3}})
            for (CoordinateFormat cf : {CoordinateFormat::Decimal, CoordinateFormat::DegreesMinutes, CoordinateFormat::DegreesMinutesSeconds,
                                        CoordinateFormat::Mgrs, CoordinateFormat::Utm}) {
                double la = 0, lo = 0;
                std::string s = formatCoordinate(pla, plo, cf);
                bool ok = parseCoordinate(s, la, lo) && std::abs(la - pla) < 2e-5 && std::abs(lo - plo) < 2e-5 * (pla > 80 ? 20 : 1);
                if (!ok) std::printf("     format/parse '%s' -> %f %f\n", s.c_str(), la, lo);
                fmtOk = fmtOk && ok;
            }
        check(fmtOk, "coordinate formats roundtrip");
    }
    {
        std::vector<Feature> be = bullseye({47.0, -122.9, 0}, 10000, 3, 30);
        std::vector<Feature> rr = rangeRings({47.0, -122.9, 0}, 1000, 4, 8);
        std::vector<Feature> rb = rangeBearingLine({47.0, -122.9, 0}, {47.1, -122.8, 0}, NorthReference::TrueNorth);
        GeodesicLine g = geodesicInverse(47.0, -122.9, 47.1, -122.8);
        const std::string* dist = rb.empty() ? nullptr : findAttribute(rb[0], "distance_m");
        Feature circle = geodesicCircle({10, 20, 0}, 5000);
        double worst = 0;
        for (const Coord& c : circle.geometries[0].points) worst = std::max(worst, std::abs(geodesicInverse(10, 20, c.lat, c.lon).distance - 5000));
        check(be.size() == 31 && rr.size() == 16 && rb.size() == 2 && dist && std::abs(std::stod(*dist) - g.distance) < 1e-3 && worst < 1e-3,
              "bullseye, range rings, range and bearing line, geodesic circle");
    }
    {
        check(sidcFromCotType("a-f-G-U-C-I") == "SFGPUCI--------" && sidcFromCotType("a-h-A-M-F") == "SHAPMF---------" &&
                  sidcFromCotType("b-m-p-s-p-i").empty(),
              "CoT type to 2525C SIDC");
        check(cotTypeFromSidc("SFGPUCI----D---") == "a-f-G-U-C-I" && cotTypeFromSidc("10031000001211000000") == "a-f-G-U-C-I", "SIDC to CoT type");
        SymbolCode d = parseSidc("10061000161211000000");
        check(d.valid && d.affiliation == Affiliation::Hostile && d.echelon == 'F' && d.function == "UCI---", "2525D numeric SIDC");
        const char* sheet[] = {"SFGPUCI----D---", "SHGPUCA----E---", "SNGPUCR--------", "SUGPUCF--------", "SFGAUCE---F----", "SFAPMF---------",
                               "SHAPMH---------", "SFSPCLDD-------", "SHUPS----------", "SFGPUCIZ--H----", "SFGPUSM----A---", "SAGPUCD--------",
                               "SFGPUCFM-AF----", "SHGPUCAA--B----", "SFGPEVAT-------", "SFGPUUS----G---", "SPGPU----------", "SFGPIZ---------",
                               "SFFPA----------", "SJGPUCI----C---", "SFGPUCV--------", "10031000141211000000", "SSGPUCI--------", "SFGPUST----I---"};
        int n = static_cast<int>(sizeof(sheet) / sizeof(sheet[0]));
        bool drawn = true;
        const int cell = 96, cols = 6;
        std::vector<std::uint8_t> rgb(static_cast<size_t>(cols * cell) * ((n + cols - 1) / cols * cell) * 3, 255);
        for (int i = 0; i < n; ++i) {
            Image img = renderSymbolImage(sheet[i], cell);
            int opaque = 0;
            for (size_t p = 3; p < img.rgba.size(); p += 4) opaque += img.rgba[p] > 128 ? 1 : 0;
            drawn = drawn && img.width == cell && opaque > cell * cell / 20;
            for (int y = 0; y < cell; ++y)
                for (int x = 0; x < cell; ++x) {
                    const std::uint8_t* s = &img.rgba[(static_cast<size_t>(y) * cell + x) * 4];
                    std::uint8_t* t = &rgb[((static_cast<size_t>(i / cols * cell + y)) * cols * cell + i % cols * cell + x) * 3];
                    for (int k = 0; k < 3; ++k) t[k] = static_cast<std::uint8_t>((s[k] * s[3] + 255 * (255 - s[3])) / 255);
                }
        }
        if (const char* out = std::getenv("OTK_MILSYM_SHEET")) {
            std::ofstream ppm(out, std::ios::binary);
            ppm << "P6\n" << cols * cell << " " << (n + cols - 1) / cols * cell << "\n255\n";
            ppm.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        }
        check(drawn, "2525 symbols render");
        Feature unit;
        unit.type = "a-f-G-U-C-I";
        unit.geometries.push_back({GeometryType::Point, {{47, -122.9, 0}}, {}});
        applySymbol(unit);
        check(unit.style.sidc == "SFGPUCI--------" && unit.style.iconImage && unit.style.iconColor == 0xFFFFFFFF, "CoT units get 2525 icons");
    }
    {
        auto readAll = [](const std::string& p) {
            std::ifstream f(p, std::ios::binary);
            return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
        };
        ResourceReader none;
        auto content = [&](const char* name, std::vector<CpuModel>& out) {
            out.clear();
            std::vector<std::uint8_t> b = readAll(data(name));
            return !b.empty() && parseTileContent(b.data(), b.size(), name, none, Mat4::identity(), out);
        };
        std::vector<CpuModel> m;
#if defined(OTK_WITH_DRACO)
        bool mesh = content("tiles3d/draco.glb", m) && m.size() == 1 && m[0].primitives.size() == 1 &&
                    m[0].primitives[0].vertices.size() == 441 * 8 && m[0].primitives[0].indices.size() == 2400;
        if (mesh)
            for (size_t i = 0; i < m[0].primitives[0].vertices.size(); i += 8)
                mesh = mesh && std::abs(m[0].primitives[0].vertices[i]) <= 800.5f && std::abs(m[0].primitives[0].vertices[i + 1]) <= 20.5f;
        check(mesh, "draco mesh decodes");
        bool cloud = content("tiles3d/draco_points.pnts", m) && m.size() == 1 && m[0].points.size() == 6400 * 16;
        if (cloud)
            for (size_t i = 0; i < 6400; ++i) cloud = cloud && std::abs(m[0].points[i * 16 + 12] - 40) <= 2 && m[0].points[i * 16 + 14] >= 250;
        check(cloud, "draco point cloud decodes with colours");
#endif
#if defined(OTK_WITH_KTX2)
        setCompressedTextureSupport(true, false, false);
        check(content("tiles3d/etc1s.glb", m) && m.size() == 1 && m[0].images.size() == 1 && m[0].images[0].format == SG_PIXELFORMAT_BC7_RGBA &&
                  m[0].images[0].levels.size() == 7 && m[0].images[0].levels[0].size() == 16 * 16 * 16,
              "ktx2 etc1s transcodes to bc7 with mips");
        setCompressedTextureSupport(false, false, false);
        bool rgba = content("tiles3d/uastc.glb", m) && m.size() == 1 && m[0].images.size() == 1 && m[0].images[0].format == SG_PIXELFORMAT_RGBA8 &&
                    m[0].images[0].width == 64 && m[0].images[0].levels[0].size() == 64 * 64 * 4;
        if (rgba) {
            const auto& px = m[0].images[0].levels[0];
            rgba = std::abs(px[0] - 200) < 24 && px[1] < 64 && px[2] < 64 && px[8 * 4] > 220 && px[8 * 4 + 1] > 220;
        }
        check(rgba, "ktx2 uastc transcodes to rgba");
        setCompressedTextureSupport(true, true, true);
#endif
        bool i3dm = content("tiles3d/boxes.i3dm", m) && m.size() == 1 && m[0].instances.size() == 1 && !m[0].instances[0].local &&
                    m[0].instances[0].matrices.size() == 16 * 16 && !m[0].primitives.empty() && m[0].primitives[0].instances == 0;
        auto column = [&](size_t i) {
            const float* c = &m[0].instances[0].matrices[i * 16];
            return std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        };
        check(i3dm && std::abs(m[0].instances[0].matrices[12] + 600.0f) < 1e-3f && std::abs(column(0) - 1.0f) < 1e-4f && std::abs(column(1) - 1.25f) < 1e-4f,
              "i3dm instances with positions and scale");
        check(content("tiles3d/gpu_instancing.glb", m) && m.size() == 1 && m[0].instances.size() == 1 && m[0].instances[0].local &&
                  m[0].instances[0].matrices.size() == 16 * 16,
              "EXT_mesh_gpu_instancing");
        check(content("tiles3d/mixed.cmpt", m) && m.size() == 2 && !m[0].primitives.empty() && m[1].points.size() == 400 * 16, "cmpt with b3dm and pnts");

        CpuModel merged;
        CpuPrimitive a;
        a.vertices = {0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0};
        a.indices = {0, 1, 2};
        a.matrix = Mat4::identity().m;
        CpuPrimitive b = a;
        b.matrix = translation({10, 0, 0}).m;
        merged.primitives = {a, b};
        mergePrimitives(merged);
        check(merged.primitives.size() == 1 && merged.primitives[0].vertices.size() == 48 && std::abs(merged.primitives[0].vertices[24] - 10.0f) < 1e-5f &&
                  merged.primitives[0].indices.size() == 6 && merged.primitives[0].indices[3] == 3,
              "primitives sharing a material merge into one draw");

#if defined(OTK_WITH_MESHOPT)
        const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1};
        const std::vector<unsigned> idx = {0, 1, 2, 0, 2, 3};
        std::vector<unsigned char> vb(meshopt_encodeVertexBufferBound(4, 12));
        vb.resize(meshopt_encodeVertexBuffer(vb.data(), vb.size(), pos.data(), 4, 12));
        std::vector<unsigned char> ib(meshopt_encodeIndexBufferBound(6, 4));
        ib.resize(meshopt_encodeIndexBuffer(ib.data(), ib.size(), idx.data(), 6));
        std::vector<unsigned char> bin = vb;
        bin.resize((bin.size() + 3) / 4 * 4);
        const size_t ibOffset = bin.size();
        bin.insert(bin.end(), ib.begin(), ib.end());
        bin.resize((bin.size() + 3) / 4 * 4);
        nlohmann::json doc = {
            {"asset", {{"version", "2.0"}}},
            {"extensionsUsed", {"EXT_meshopt_compression"}},
            {"extensionsRequired", {"EXT_meshopt_compression"}},
            {"buffers", {{{"byteLength", bin.size()}}, {{"byteLength", 72}, {"extensions", {{"EXT_meshopt_compression", {{"fallback", true}}}}}}}},
            {"bufferViews",
             {{{"buffer", 1}, {"byteOffset", 0}, {"byteLength", 48}, {"byteStride", 12},
               {"extensions",
                {{"EXT_meshopt_compression",
                  {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", vb.size()}, {"byteStride", 12}, {"count", 4}, {"mode", "ATTRIBUTES"}}}}}},
              {{"buffer", 1}, {"byteOffset", 48}, {"byteLength", 24},
               {"extensions",
                {{"EXT_meshopt_compression",
                  {{"buffer", 0}, {"byteOffset", ibOffset}, {"byteLength", ib.size()}, {"byteStride", 4}, {"count", 6}, {"mode", "TRIANGLES"}}}}}}}},
            {"accessors",
             {{{"bufferView", 0}, {"componentType", 5126}, {"count", 4}, {"type", "VEC3"}, {"min", {0, 0, 0}}, {"max", {1, 0, 1}}},
              {{"bufferView", 1}, {"componentType", 5125}, {"count", 6}, {"type", "SCALAR"}}}},
            {"meshes", {{{"primitives", {{{"attributes", {{"POSITION", 0}}}, {"indices", 1}}}}}}},
            {"nodes", {{{"mesh", 0}}}},
            {"scenes", {{{"nodes", {0}}}}},
            {"scene", 0}};
        std::string js = doc.dump();
        js.resize((js.size() + 3) / 4 * 4, ' ');
        std::vector<std::uint8_t> glb;
        auto u32 = [&glb](std::uint32_t v) {
            for (int k = 0; k < 4; ++k) glb.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        };
        glb.insert(glb.end(), {'g', 'l', 'T', 'F'});
        u32(2);
        u32(static_cast<std::uint32_t>(12 + 8 + js.size() + 8 + bin.size()));
        u32(static_cast<std::uint32_t>(js.size()));
        glb.insert(glb.end(), {'J', 'S', 'O', 'N'});
        glb.insert(glb.end(), js.begin(), js.end());
        u32(static_cast<std::uint32_t>(bin.size()));
        glb.insert(glb.end(), {'B', 'I', 'N', 0});
        glb.insert(glb.end(), bin.begin(), bin.end());
        CpuModel opt;
        bool decoded = parseGltf(glb.data(), glb.size(), "", none, opt) && opt.primitives.size() == 1 && opt.primitives[0].indices.size() == 6 &&
                       opt.primitives[0].vertices.size() == 32;
        for (size_t i = 0; decoded && i < 4; ++i)
            for (int k = 0; k < 3; ++k) decoded = decoded && opt.primitives[0].vertices[i * 8 + k] == pos[i * 3 + k];
        for (size_t i = 0; decoded && i < 6; ++i) decoded = opt.primitives[0].indices[i] == idx[i];
        check(decoded, "meshopt-compressed glTF decodes");
#endif
    }
    {
        Vec3 s = sunDirectionEcef(1718916660.0);
        check(std::abs(std::asin(s.z) * kRadToDeg - 23.44) < 0.02, "sun declination at the June 2024 solstice");
        SunMoonTimes london = sunMoonTimes(1718928000.0, 51.5074, -0.1278);
        check(std::abs(london.sunrise - 1718941380.0) < 180 && std::abs(london.sunset - 1719001260.0) < 180, "London sunrise and sunset 2024-06-21");
        check(london.nauticalDawn < london.civilDawn && london.civilDawn < london.sunrise && london.sunset < london.civilDusk &&
                  london.civilDusk < london.nauticalDusk && std::isnan(london.astronomicalDawn),
              "London twilight order, no astronomical night at midsummer");
        SunMoonTimes dc = sunMoonTimes(1718856000.0, 38.9072, -77.0369);
        check(std::abs(dc.sunrise - 1718876520.0) < 180 && std::abs(dc.sunset - 1718930220.0) < 180, "Washington sunrise and sunset 2024-06-20");
        MoonInfo full = moonPosition(1706205240.0, 0, 0), dark = moonPosition(1704974220.0, 0, 0), quarter = moonPosition(1705549980.0, 0, 0);
        check(full.illuminatedFraction > 0.99 && dark.illuminatedFraction < 0.01 && std::abs(quarter.illuminatedFraction - 0.5) < 0.03 && quarter.waxing,
              "moon phases January 2024");
        CelestialPosition es = sunPosition(1712601720.0, 32.7767, -96.797);
        MoonInfo em = moonPosition(1712601720.0, 32.7767, -96.797);
        double sep = std::acos(std::sin(es.elevation * kDegToRad) * std::sin(em.position.elevation * kDegToRad) +
                               std::cos(es.elevation * kDegToRad) * std::cos(em.position.elevation * kDegToRad) *
                                   std::cos((es.azimuth - em.position.azimuth) * kDegToRad)) * kRadToDeg;
        check(sep < 0.5 && es.elevation > 60.0, "sun and moon coincide over Dallas at the 2024-04-08 eclipse");
        Illuminance noon = groundIlluminance(1718971200.0, 51.5, 0.0), night = groundIlluminance(1704938400.0, 51.5, 0.0);
        check(noon.sunLux > 50000 && night.totalLux < 0.01 && night.sunLux == 0.0, "ground illuminance day and moonless night");
    }
    {
        UasMetadata m;
        m.time = 1712601720.25;
        m.missionId = "MISSION01";
        m.platformTail = "AF-101";
        m.platformHeading = 159.9744;
        m.platformPitch = -0.4315;
        m.platformRoll = 3.4058;
        m.sensorLat = 60.1768229;
        m.sensorLon = 128.4267590;
        m.sensorAlt = 14190.7;
        m.hfov = 144.571;
        m.vfov = 152.643;
        m.sensorRelAzimuth = 160.719;
        m.sensorRelElevation = -168.792;
        m.sensorRelRoll = 176.865;
        m.slantRange = 68590.98;
        m.targetWidth = 722.82;
        m.frameCenterLat = -10.5423886;
        m.frameCenterLon = 29.1578899;
        m.frameCenterElevation = 3216.04;
        std::vector<std::uint8_t> packet = encodeUasKlv(m);
        std::vector<UasMetadata> back = parseUasKlv(packet.data(), packet.size());
        bool same = back.size() == 1 && back[0].checksumOk && back[0].missionId == "MISSION01" && back[0].platformTail == "AF-101" &&
                    std::abs(back[0].time - m.time) < 1e-6 && std::abs(back[0].platformHeading - m.platformHeading) < 0.01 &&
                    std::abs(back[0].platformPitch - m.platformPitch) < 0.001 && std::abs(back[0].sensorLat - m.sensorLat) < 1e-6 &&
                    std::abs(back[0].sensorLon - m.sensorLon) < 1e-6 && std::abs(back[0].sensorAlt - m.sensorAlt) < 0.5 &&
                    std::abs(back[0].sensorRelElevation - m.sensorRelElevation) < 1e-6 && std::abs(back[0].slantRange - m.slantRange) < 0.01 &&
                    std::abs(back[0].frameCenterLat - m.frameCenterLat) < 1e-6 && std::abs(back[0].frameCenterElevation - m.frameCenterElevation) < 0.5;
        check(same, "MISB ST 0601 encode and parse round trip");
        std::vector<std::uint8_t> bad = packet;
        bad[30] ^= 0x10;
        back = parseUasKlv(bad.data(), bad.size());
        check(back.empty(), "MISB ST 0601 corrupt packet is rejected");
        std::vector<std::uint8_t> noChecksum = packet;
        noChecksum.resize(noChecksum.size() - 4);
        std::size_t lengthAt = (noChecksum[16] & 0x80) ? 16 + (noChecksum[16] & 0x7F) : 16;
        noChecksum[lengthAt] = static_cast<std::uint8_t>(noChecksum[lengthAt] - 4);
        back = parseUasKlv(noChecksum.data(), noChecksum.size());
        check(back.empty(), "MISB ST 0601 packet without a checksum is rejected");

        std::vector<std::uint8_t> ts;
        int cc[0x2000] = {};
        auto tsPackets = [&](int pid, std::vector<std::uint8_t> payload) {
            bool first = true;
            size_t pos = 0;
            do {
                std::uint8_t p[188];
                std::memset(p, 0xFF, sizeof(p));
                p[0] = 0x47;
                p[1] = static_cast<std::uint8_t>((first ? 0x40 : 0) | ((pid >> 8) & 0x1F));
                p[2] = static_cast<std::uint8_t>(pid & 0xFF);
                p[3] = static_cast<std::uint8_t>(0x10 | (cc[pid]++ & 0x0F));
                size_t n = std::min<size_t>(184, payload.size() - pos);
                std::memcpy(p + 4, payload.data() + pos, n);
                pos += n;
                first = false;
                ts.insert(ts.end(), p, p + 188);
            } while (pos < payload.size());
        };
        tsPackets(0, {0x00, 0x00, 0xB0, 13, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xE1, 0x00, 0, 0, 0, 0});
        tsPackets(0x100, {0x00, 0x02, 0xB0, 18, 0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0, 0x00, 0x15, 0xE1, 0x01, 0xF0, 0x00, 0, 0, 0, 0});
        tsPackets(0x200, std::vector<std::uint8_t>(300, 0x55));
        for (int k = 0; k < 3; ++k) {
            UasMetadata f = m;
            f.time = m.time + k;
            f.sensorLat = m.sensorLat + 0.01 * k;
            f.hfov = 10.0;
            f.vfov = 8.0;
            f.sensorRelElevation = -60.0;
            f.frameCenterElevation = 0.0;
            std::vector<std::uint8_t> klv = encodeUasKlv(f);
            std::vector<std::uint8_t> pes = {0x00, 0x00, 0x01, 0xFC, 0x00, 0x00, 0x84, 0x80, 0x05, 0x21, 0x00, 0x01, 0x00, 0x01};
            pes.insert(pes.end(), klv.begin(), klv.end());
            tsPackets(0x101, pes);
        }
        std::vector<UasMetadata> demuxed = demuxUasMetadata(ts.data(), ts.size());
        check(demuxed.size() == 3 && demuxed[2].checksumOk && std::abs(demuxed[2].sensorLat - (m.sensorLat + 0.02)) < 1e-6,
              "MISB KLV demuxed from MPEG-TS via PAT/PMT");
        ImportResult imported = importBytes(ts, "klv");
        bool hasTrack = false, hasFootprint = false;
        for (const Feature& f : imported.data.features) {
            hasTrack = hasTrack || f.type == "uas-sensor-track";
            hasFootprint = hasFootprint || f.type == "uas-footprint";
        }
        check(imported.data.features.size() >= 3 && hasTrack && hasFootprint && detectFormat(ts, "flight.ts") == "klv", "FMV import gives track and footprint");

        UasMetadata nadir;
        nadir.sensorLat = 47.0;
        nadir.sensorLon = -122.0;
        nadir.sensorAlt = 1000.0;
        nadir.platformHeading = 0;
        nadir.platformPitch = 0;
        nadir.platformRoll = 0;
        nadir.sensorRelAzimuth = 0;
        nadir.sensorRelElevation = -90;
        nadir.sensorRelRoll = 0;
        nadir.hfov = 10;
        nadir.vfov = 10;
        nadir.frameCenterElevation = 0;
        double la[4], lo[4];
        bool fp = uasFootprint(nadir, la, lo);
        double mlat = 0, mlon = 0;
        for (int c = 0; c < 4 && fp; ++c) {
            double d = length(geodeticToEcef(la[c], lo[c], 0) - geodeticToEcef(47.0, -122.0, 0));
            fp = fp && std::abs(d - 1000.0 * std::tan(5.0 * kDegToRad) * std::sqrt(2.0)) < 1.0;
            mlat += la[c] / 4;
            mlon += lo[c] / 4;
        }
        check(fp && std::abs(mlat - 47.0) < 1e-5 && std::abs(mlon + 122.0) < 1e-5, "sensor footprint from pose and field of view");
        if (const char* out = std::getenv("OTK_FMV_SAMPLE")) {
            ts.clear();
            tsPackets(0, {0x00, 0x00, 0xB0, 13, 0x00, 0x01, 0xC1, 0x00, 0x00, 0x00, 0x01, 0xE1, 0x00, 0, 0, 0, 0});
            tsPackets(0x100, {0x00, 0x02, 0xB0, 18, 0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0, 0x00, 0x15, 0xE1, 0x01, 0xF0, 0x00, 0, 0, 0, 0});
            for (int k = 0; k < 240; ++k) {
                double th = k * 1.5 * kDegToRad;
                UasMetadata f;
                f.time = 1712601720.0 + k;
                f.missionId = "ORBIT";
                f.platformTail = "UAV-7";
                f.sensorLat = 47.04 + 2000.0 * std::cos(th) / 111180.0;
                f.sensorLon = -122.90 + 2000.0 * std::sin(th) / (111320.0 * std::cos(47.04 * kDegToRad));
                f.sensorAlt = 1500.0;
                f.platformHeading = std::fmod(th * kRadToDeg + 90.0, 360.0);
                f.platformPitch = 0;
                f.platformRoll = 0;
                f.sensorRelAzimuth = 90.0;
                f.sensorRelElevation = -std::atan2(1500.0, 2000.0) * kRadToDeg;
                f.sensorRelRoll = 0;
                f.hfov = 12.0;
                f.vfov = 9.0;
                f.frameCenterLat = 47.04;
                f.frameCenterLon = -122.90;
                f.frameCenterElevation = 0.0;
                std::vector<std::uint8_t> klv = encodeUasKlv(f);
                std::vector<std::uint8_t> pes = {0x00, 0x00, 0x01, 0xFC, 0x00, 0x00, 0x84, 0x80, 0x05, 0x21, 0x00, 0x01, 0x00, 0x01};
                pes.insert(pes.end(), klv.begin(), klv.end());
                tsPackets(0x101, pes);
            }
            std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(ts.data()), static_cast<std::streamsize>(ts.size()));
        }
    }

    {
        check(PmTilesArchive::tileId(0, 0, 0) == 0 && PmTilesArchive::tileId(1, 0, 0) == 1 && PmTilesArchive::tileId(1, 0, 1) == 2 &&
                  PmTilesArchive::tileId(1, 1, 1) == 3 && PmTilesArchive::tileId(1, 1, 0) == 4 && PmTilesArchive::tileId(2, 0, 0) == 5,
              "pmtiles hilbert tile ids");
        for (const char* name : {"dem-1234-webp.pmtiles", "dem-1234-png.pmtiles", "dem-1234-z10.pmtiles"}) {
            std::string error;
            auto archive = PmTilesArchive::open(data(name), &error);
            check(archive && archive->header().maxZoom >= 8 && !archive->vector(), (std::string("pmtiles header ") + name).c_str());
            Engine engine;
            ElevationLayer dem;
            dem.type = ElevationType::PMTiles;
            dem.path = data(name);
            engine.setElevation({dem});
            std::vector<Coord> pts{{44.428, -110.5885, 0}, {44.928, -111.0885, 0}, {45.2, -111.9, 0}, {-33.87, 151.2, 0}};
            std::vector<double> res;
            std::vector<double> h = engine.queryElevation(pts, 100.0, &res);
            bool ok = h.size() == pts.size() && std::isnan(h[3]);
            for (int i = 0; i < 3 && ok; ++i) {
                double msl = h[i] - geoidOffset(pts[i].lat, pts[i].lon);
                ok = std::abs(msl - 1234.0) < 0.05 && res[i] > 0;
                if (!ok) std::printf("  point %d: %.4f (geoid %.4f) res %.2f\n", i, h[i], geoidOffset(pts[i].lat, pts[i].lon), res[i]);
            }
            check(ok, (std::string("terrarium heights from ") + name).c_str());
            std::vector<double> after = engine.queryElevation({{44.428, -110.5885, 0}, {-33.87, 151.2, 0}, {44.928, -111.0885, 0}}, 100.0);
            check(after.size() == 3 && std::abs(after[2] - geoidOffset(44.928, -111.0885) - 1234.0) < 0.05,
                  (std::string("a hole does not leave a coarse tile behind in ") + name).c_str());
            std::vector<double> top = engine.queryElevation({{44.928, -111.0885, 0}});
            check(top.size() == 1 && std::abs(top[0] - geoidOffset(44.928, -111.0885) - 1234.0) < 0.05,
                  (std::string("deepest level heights from ") + name).c_str());
        }
    }
    {
        struct Checker : HostTileSource {
            int tile(int z, int, int, TileData& out, const UrlFetch&) override {
                if (z > 3) return 404;
                out.width = 2;
                out.height = 2;
                out.rgba.assign(16, 200);
                return 200;
            }
            bool remote() const override { return false; }
        };
        ImageryLayer host;
        host.type = SourceType::Host;
        host.source = std::make_shared<Checker>();
        host.maxZoom = 5;
        auto provider = makeProvider(host, nullptr, nullptr);
        FetchResult r = provider->fetch(2, 1, 1);
        FetchResult missing = provider->fetch(4, 1, 1);
        check(r.status == 200 && r.raw && r.width == 2 && r.data.size() == 16 && missing.status == 404, "host tile source");
        check(layerIdentity(host) != layerIdentity(ImageryLayer{}), "layer identity includes the host source");
    }
    {
        const labels::Dataset& d = labels::Dataset::get();
        if (d.available()) {
            auto sideName = [&](std::int32_t idx) {
                return idx >= 0 && static_cast<size_t>(idx) < d.places().size() ? std::string(d.name(d.places()[static_cast<size_t>(idx)])) : std::string();
            };
            auto isCounty = [&](std::int32_t idx) {
                return idx >= 0 && static_cast<size_t>(idx) < d.places().size() && d.places()[static_cast<size_t>(idx)].kind == labels::Kind::County;
            };
            labels::Lines lines;
            const int cell = static_cast<int>(std::floor((47.4 + 90.0) / labels::kFineCell)) * labels::kFineCols +
                             static_cast<int>(std::floor((-122.6 + 180.0) / labels::kFineCell));
            const bool decoded = d.fine(cell, lines);
            int county = 0, named = 0;
            double best = 1e9;
            std::string north, south;
            bool thurstonNorth = false;
            for (const labels::Run& r : lines.runs) {
                if (r.level != labels::Level::County) continue;
                ++county;
                if (isCounty(r.left) && isCounty(r.right)) ++named;
                const bool thurston = sideName(r.left) == "Thurston County" || sideName(r.right) == "Thurston County";
                for (std::uint32_t i = 0; i + 1 < r.count; ++i) {
                    const std::uint32_t v = r.first + i;
                    const double x0 = lines.xy[2 * v] * labels::kUnit, y0 = lines.xy[2 * v + 1] * labels::kUnit;
                    const double x1 = lines.xy[2 * v + 2] * labels::kUnit, y1 = lines.xy[2 * v + 3] * labels::kUnit;
                    if (thurston && std::max(y0, y1) > 47.25) thurstonNorth = true;
                    const double ax = (x0 + 122.6) * 0.68, ay = y0 - 47.4034, dx = (x1 - x0) * 0.68, dy = y1 - y0;
                    const double len2 = dx * dx + dy * dy;
                    const double t = len2 > 0.0 ? std::clamp(-(ax * dx + ay * dy) / len2, 0.0, 1.0) : 0.0;
                    const double dist = std::hypot(ax + dx * t, ay + dy * t);
                    if (dist < best) {
                        best = dist;
                        north = sideName(x1 > x0 ? r.left : r.right);
                        south = sideName(x1 > x0 ? r.right : r.left);
                    }
                }
            }
            check(decoded && county > 50 && named == county, "county lines carry the county on each side");
            check(best < 0.01 && north == "Kitsap County" && south == "Pierce County", "county sides at the Kitsap and Pierce line near Gig Harbor");
            check(!thurstonNorth, "no Thurston County side north of Thurston County");
        }
    }
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
