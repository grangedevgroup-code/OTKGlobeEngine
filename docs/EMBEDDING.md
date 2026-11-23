# Embedding OTKGlobeEngine

The engine never owns a window. A host app creates a GPU surface with its own toolkit, creates the engine once for that device, and calls a frame function whenever the surface needs drawing. `otk_needs_redraw` tells the host when tiles are still arriving, so a host can render on demand instead of continuously.

To save battery, ask `otk_next_frame_delay` how long you can wait before the next frame:

- `0`: draw now.
- A small number of seconds: the engine is waiting for tiles or a timer. Draw again after that long. Frames while waiting are cheap, because the engine shows its last frame again.
- Infinity: nothing changes until the next input or API call.

Packages:

- Android: `python scripts/android_aar.py` builds `build/otkglobeengine-<version>.aar` (arm64-v8a, armeabi-v7a, x86_64) containing `com.otkglobeengine.OTKGlobeView`, a ready-made `GLSurfaceView` with ATAK-style gestures, and `OTKGlobeEngine` with the full native API.
- iOS, iPadOS, macOS: `bash scripts/apple_xcframework.sh` (on a Mac; CI publishes it as an artifact) builds `OTKGlobeEngine.xcframework` with the `OTKGlobeEngineC` module. Add `bindings/apple/OTKGlobeView.swift` for a ready-made `MTKView` with gestures, or add this repository as a Swift package and use its `OTKGlobeView` library next to the xcframework.
- .NET: `bindings/dotnet/OTKGlobeEngine.csproj` wraps the C ABI (`Engine`, `Native`); ship `otkglobeengine_c.dll`/`.so`/`.dylib` next to the app.

Two APIs are available:

- C++: `include/otk/engine.hpp` (`otk::Engine`), linked against the `otkglobeengine` static library. The host calls `sg_setup` itself with `otk::configureGfx(desc)` applied, as `apps/viewer` does.
- C ABI: `include/otk/otk.h`, in the static library and in the `otkglobeengine_c` shared library. It sets up the GPU layer from native handles and is the entry point for Kotlin/JNI, Swift, C# and other languages.

The graphics backend is chosen at build time: Direct3D 11 on Windows, Metal on macOS and iOS, OpenGL 4.1 on Linux, OpenGL ES 3 on Android.

## Web pages

The `otkglobeengine` npm package is the same engine compiled to WebAssembly. It draws into a `<canvas>` with WebGL 2 and handles input itself:

```js
import { OTKGlobe } from 'otkglobeengine';
const globe = await OTKGlobe.create('#map', { view: { lat: 47.61, lon: -122.35, metersPerPixel: 2, tilt: 45 } });
```

The page must be served with `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`. [bindings/web/README.md](../bindings/web/README.md) has the full guide; `python scripts/web_build.py` builds the package into `bindings/web/dist`.

## Kernel only (no GPU)

Servers, converters and other headless programs link the `otk_kernel` target (add the repository with `add_subdirectory` or `FetchContent`, optionally with `-DOTK_ENGINE=OFF`). Everything in it works without a window or graphics driver:

```cpp
#include "otk/elevation.hpp"
#include "otk/feature_query.hpp"
#include "otk/formats.hpp"
#include "otk/importer.hpp"

otk::ImportPlan plan = otk::planImport("survey.kmz");
otk::ImportResult data = otk::importData(plan.path);

otk::FeatureQuery near;
near.useRadius = true;
near.center = {47.0359, -122.9049, 0};
near.radiusMeters = 500;
for (std::size_t i : otk::queryFeatures(data.data.features, near)) { /* ... */ }

otk::ElevationService elevation;
elevation.setCacheDirectory(otk::defaultCacheDirectory());
elevation.setSources(otk::defaultElevation());
double msl = elevation.heightMsl(46.8529, -121.7604);
otk::LineOfSight los = elevation.lineOfSight({46.85, -121.76, 0}, 2.0, {46.90, -121.70, 0}, 2.0);
```

## Android (GLSurfaceView, Kotlin + JNI)

```kotlin
override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
    engine = Native.create()
}
override fun onDrawFrame(gl: GL10?) {
    Native.frame(engine, width, height, density)
}
```

```cpp
JNIEXPORT jlong JNICALL Java_app_Native_create(JNIEnv* env, jclass) {
    JavaVM* vm;
    env->GetJavaVM(&vm);
    otk_set_android_vm(vm);
    otk_engine* e = otk_create_gl(1, 1);
    otk_set_cache_directory(e, "/data/data/app/files");
    return reinterpret_cast<jlong>(e);
}
JNIEXPORT void JNICALL Java_app_Native_frame(JNIEnv*, jclass, jlong e, jint w, jint h, jfloat dpi) {
    otk_frame_gl(reinterpret_cast<otk_engine*>(e), w, h, 0, dpi);
}
```

`otk_set_android_vm` is required: tiles are fetched through the platform `HttpURLConnection`, so no TLS library ships in the APK. `scripts/android_apk.py` builds a standalone NativeActivity viewer APK without Gradle.

## iOS, iPadOS and macOS (MTKView, Swift)

```swift
let engine = otk_create_metal(Unmanaged.passUnretained(view.device!).toOpaque(), 1)
view.depthStencilPixelFormat = .depth32Float_stencil8
view.colorPixelFormat = .bgra8Unorm

func draw(in view: MTKView) {
    guard let drawable = view.currentDrawable, let depth = view.depthStencilTexture else { return }
    otk_frame_metal(engine, Int32(view.drawableSize.width), Int32(view.drawableSize.height),
                    Unmanaged.passUnretained(drawable).toOpaque(),
                    Unmanaged.passUnretained(depth).toOpaque(), nil, Float(view.contentScaleFactor))
}
```

## Windows (Direct3D 11)

```cpp
otk_engine* e = otk_create_d3d11(device, context, 1);
otk_frame_d3d11(e, width, height, backbufferRtv, nullptr, depthDsv, dpiScale);
swapChain->Present(1, 0);
```

The back buffer must be `DXGI_FORMAT_B8G8R8A8_UNORM` and the depth view `DXGI_FORMAT_D24_UNORM_S8_UINT`.

## C# (.NET P/Invoke)

```csharp
[DllImport("otkglobeengine_c")] static extern IntPtr otk_create_d3d11(IntPtr device, IntPtr context, int samples);
[DllImport("otkglobeengine_c")] static extern void otk_frame_d3d11(IntPtr e, int w, int h, IntPtr rtv, IntPtr resolve, IntPtr dsv, float dpi);
[DllImport("otkglobeengine_c")] static extern int otk_load_features(IntPtr e, [MarshalAs(UnmanagedType.LPUTF8Str)] string path);
```

## Qt (QOpenGLWidget on Linux)

```cpp
void initializeGL() override { engine = otk_create_gl(0, 1); }
void paintGL() override {
    otk_frame_gl(engine, width() * devicePixelRatio(), height() * devicePixelRatio(), defaultFramebufferObject(), devicePixelRatio());
    const double delay = otk_next_frame_delay(engine);
    if (delay <= 0.0) update();
    else if (std::isfinite(delay)) QTimer::singleShot(int(delay * 1000.0) + 1, this, [this] { update(); });
}
```

## Common calls

```c
otk_view v = {47.04, -122.90, 20.0, 0.0, 45.0};
otk_set_view(e, &v);
otk_add_layer(e, OTK_SOURCE_MBTILES, "Offline", "/maps/area.mbtiles", 0, 18, 1.0f);
otk_load_mobac(e, "/maps/source.xml");
otk_add_elevation_directory(e, "/dted");
otk_load_features(e, "/data/overlay.kmz");
otk_open_result opened;
if (otk_open(e, "/data/anything.pmtiles", &opened) && opened.has_region) {
    otk_view_for_region(e, opened.west, opened.south, opened.east, opened.north, &v);
    otk_fly_to(e, &v, 2.0);
}
otk_set_grid(e, 1);
otk_set_labels(e, 1);
otk_set_labels_source(e, "/maps/roads.pmtiles");
otk_set_contours(e, 1, 0.0, OTK_DISTANCE_METRIC);
int roads = otk_download_labels(e, -122.96, 47.00, -122.84, 47.08, 14);
otk_set_controls(e, OTK_CONTROL_ALL & ~OTK_CONTROL_TILT);
otk_set_buildings(e, 1);
otk_set_basemap(e, 2);
char mgrs[32];
otk_to_mgrs(47.04, -122.90, 5, mgrs, sizeof mgrs);
```
