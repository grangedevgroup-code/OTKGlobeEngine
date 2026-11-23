# Third-party components

OTKGlobeEngine's own code is under the Apache License 2.0 ([LICENSE](../LICENSE)): use it for anything, change it and sell it, in open or closed products, without having to share your changes. Copies you pass on keep the license text, and files you changed say so.

The libraries and data built into the engine keep their own licenses. Most are public domain or zlib and ask for nothing. The MIT, ISC, BSD and Apache ones ask that their license text ships with copies of the software. Putting this file and the linked license texts in a product's "licenses" screen or folder covers that. Nothing here requires naming OTKGlobeEngine.

| Component | Used for | License | In a binary you ship |
| --- | --- | --- | --- |
| [sokol](https://github.com/floooh/sokol) | Graphics, app shell of the demo viewer | zlib | Nothing required |
| [stb](https://github.com/nothings/stb) | PNG/JPEG decoding, font rasterising | Public domain (or MIT) | Nothing required |
| [SQLite](https://sqlite.org) | Tile cache, MBTiles, GeoPackage | Public domain | Nothing required |
| [pugixml](https://github.com/zeux/pugixml) | KML, GPX, GML, CoT, MOBAC | MIT | License text |
| [nlohmann/json](https://github.com/nlohmann/json) | GeoJSON, 3D Tiles, TileJSON | MIT | License text |
| [miniz](https://github.com/richgel999/miniz) | KMZ, data packages, .3tz | MIT | License text |
| [earcut.hpp](https://github.com/mapbox/earcut.hpp) | Polygon triangulation | ISC | License text |
| [cgltf](https://github.com/jkuhlmann/cgltf) | glTF parsing | MIT | License text |
| [misb](https://github.com/Mattbusel/misb) | MISB ST 0601 KLV decoding | MIT or Apache-2.0 | License text |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) decoder | `EXT_meshopt_compression` | MIT | License text; leave out with `-DOTK_WITH_MESHOPT=OFF` |
| [Draco](https://github.com/google/draco) decoder | `KHR_draco_mesh_compression`, Draco point clouds | Apache-2.0 | License text; leave out with `-DOTK_WITH_DRACO=OFF` |
| [Basis Universal](https://github.com/BinomialLLC/basis_universal) transcoder and its Zstandard decoder | KTX2 / `KHR_texture_basisu` | Apache-2.0, Zstandard BSD-3-Clause | License texts; leave out with `-DOTK_WITH_KTX2=OFF` |
| Noto Sans Bold (subset) | Labels | SIL Open Font License 1.1 ([OFL.txt](../assets/fonts/OFL.txt)) | OFL text with the font |
| Roboto SemiBold (subset) | Built-in map labels | SIL Open Font License 1.1 ([OFL-Roboto.txt](../assets/fonts/OFL-Roboto.txt)) | OFL text with the font |
| [geoBoundaries](https://www.geoboundaries.org) CGAZ 6.0 (William & Mary geoLab), simplified and re-encoded | Built-in country, state and county lines and county names | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | Credit "geoBoundaries"; the engine lists it in `credits()` while labels are on. Leave out with `-DOTK_LABELS=OFF` |
| [GeoNames](https://www.geonames.org) cities500 and admin codes, selected and re-encoded | Built-in city, town and village names, state and county names | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) | Credit "GeoNames"; listed in `credits()` while labels are on. Leave out with `-DOTK_LABELS=OFF` |
| [Natural Earth](https://www.naturalearthdata.com) admin 0, admin 1 and populated places | Built-in country names and label positions | Public domain | Nothing required |
| [OpenStreetMap](https://www.openstreetmap.org/copyright) data in the [OpenMapTiles](https://openmaptiles.org) schema, served by [OpenFreeMap](https://openfreemap.org) | Roads, street names, neighbourhoods, parks and water names, and 3D building footprints and heights, downloaded per area on request (not built in) | Data ODbL 1.0, schema CC BY 4.0 | Credit "OpenFreeMap, © OpenMapTiles, © OpenStreetMap contributors"; the engine lists it in `credits()` while the road layer is on. Change or turn off with `setLabelsSource` |
| [High Resolution Canopy Height Maps](https://registry.opendata.aws/dataforgood-fb-forests/) by Meta and the World Resources Institute (Tolan et al. 2024, CHMv2 Brandt et al. 2026), read from the [Source Cooperative copy](https://source.coop/tge-labs/meta-chm-v2) by Taylor Geospatial Engine Labs | 3D trees (currently switched off): tree heights at about 1.2 m, downloaded per area as you view it (not built in) | CC BY 4.0 | Credit "Meta and World Resources Institute canopy height"; the engine lists it in `credits()` while 3D buildings are on |
| [ambientCG](https://ambientcg.com) materials (Facade001, 005, 006, 014, 018A, 019A, 020A, WoodSiding005, PaintedPlaster017, Concrete034, CorrugatedSteel005, RoofingTiles013A), resized into `assets/buildings/facades.jpg` | 3D building walls and roofs | CC0 1.0 (public domain) | Nothing required |
| NASA Blue Marble | Offline backdrop | Public domain | Nothing required; leave out with `-DOTK_EMBED_BACKDROP=OFF` |
| EGM96 geoid grid (NGA) | Height conversion | Public domain | Nothing required |
| WMM2025 coefficients (NOAA/NCEI) | Magnetic declination | Public domain | Nothing required |
| [GDAL](https://gdal.org) | Optional raster/vector module, off by default | MIT/X11 | License text when `-DOTK_WITH_GDAL=ON` |
| libcurl (Linux only) | HTTP | curl (MIT-style) | License text |

Build tools that do not end up in the binary (CMake, Ninja, sokol-shdc) are not listed.

The online map services that the demo presets point at (Esri World Imagery and World Topographic Map, OpenStreetMap, NASA GIBS, AWS Terrain Tiles) have their own terms of use, for example OpenStreetMap's attribution requirement. Those apply to using the services, not to the engine's code; an airgapped deployment with its own data is not affected.
