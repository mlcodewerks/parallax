# GLRDP controls

Select `wtfn64_renderer=glrdp`. All renderer options take effect when content
restarts. GL3.3 and GLES3 share one RDP renderer and draw ubershader, with runtime cycle,
coverage and arithmetic controls. Optional shader specialization also prepares
a program for the selected draw settings at startup; the generic program remains
available for later configuration changes. VI presentation uses separate scanout shaders.
There are no per-cycle or per-coverage shader compilations during drawing.
GPU resources and shaders are prepared during context reset, before emulation.

Both APIs automatically batch conservative primitive quads with independent
uniform-buffer records. Disjoint triangles, texture rectangles and fill
rectangles can share a draw even when their RDP state differs. Overlapping
bounds, target changes, readbacks and synchronization points finish the batch.
At native resolution with coverage disabled, integer row footprints distinguish
adjacent triangles whose bounding boxes overlap but whose surviving pixels do
not. Other coverage levels and upscaled draws retain conservative box checks.
The shader evaluates the N64 edge mask, shade, texture and depth planes. A
bounded 2048x2048 atlas caches immutable decoded TMEM tiles; framebuffer tiles
retain standalone textures. Tile wrapping, mirroring and filtering happen before
the atlas offset, preserving tile boundaries. Neither optimization has a user
toggle. GLES3 uses core uniform buffers and instanced drawing with GLSL ES 3.00
high-precision integer/float arithmetic. It replaces the former GLES2 renderer.
Depth batches use ping-pong integer attachments and a bounded colour feedback
copy when texture barriers are unavailable; no desktop extension is required.

At native resolution, advertised
ARB/NV texture-barrier support permits in-place colour and combined Z/DZ updates.
Each fragment reads each source texel once before writing it. Upscaled/separate
depth passes and drivers without the extension use framebuffer snapshots and
depth ping-pong instead. Switching between these paths retains accumulated depth
damage and CPU readback behavior.

Tile extents use the RDP's unsigned 10-bit endpoint differences. Wrapped tile
bounds, including Sin and Punishment's scrolling intensity tiles, remain valid
texture sources. Maximum-coordinate clamping precedes masking and mirroring.
See [batching validation](../../../../../benchmarks/GLRDP_BATCHING.md) for the
baseline comparisons, Angrylion reference checks and performance measurements.

| Option (`wtfn64_glrdp_` prefix) | Values; default first | Effect |
| --- | --- | --- |
| `upscale` | `1`, `2`, `4`, `8` | Internal resolution multiplier. |
| `coverage` | `off`, `2x`, `reduced`, `full` | Full coverage for surviving pixels, two, four or eight native samples. The UI labels the sampled modes 2x/4x/8x. Lower counts can lose thin edges. Alpha, depth and AA-off first-sample rejection remain active. |
| `blending` | `accurate`, `fast` | RDP restoring divider, or arithmetic division with the same mux sources and quantized factors. Fast can change translucent edges. |
| `combiner` | `accurate`, `fast` | Signed nine-bit arithmetic and rounding, or saturating arithmetic with normalized eight-bit factors. Fast can change lighting/sharpening. Both retain both cycles and alpha tests. |
| `texture_filter` | `native`, `point` | Follow the game's multi-tap RDP filtering, or point sample for lower GPU cost and a pixelated image. Independent of VI resampling. |
| `texcoords` | `accurate`, `fast` | Native fixed-point span/perspective calculation at 1x where supported, or floating-point coordinates. Fast can change texture alignment and wobble. |
| `shading` | `accurate`, `fast` | Native span/coverage correction for vertex colours, or floating-point colour planes. Fast can change gradients and alpha thresholds independently of texture coordinates and combiner arithmetic. |
| `shader_specialization` | `disabled`, `enabled` | Compile the selected draw-quality settings into a second shader at startup to remove unused paths. Retains the selected accuracy; increases startup compilation time. No gameplay compilation. |
| `texture_lod` | `enabled`, `disabled` | Dynamic mip/LOD calculation, or base tiles. Disabling can cause distant shimmer and change detail/sharpening effects. |
| `color_dither` | `enabled`, `disabled` | RGB dithering before storage. Disabling can expose colour banding. |
| `alpha_dither` | `enabled`, `disabled` | Dither added to pixel/shade alpha. Disabling can change transparency thresholds and fades; alpha and depth tests remain active. Independent of RGB dithering. |
| `vi_filter` | `enabled`, `disabled` | Master switch for **all** VI filters below. Individual choices are retained. Does not change RDP coverage/arithmetic. |
| `deinterlace` | `weave`, `bob` | Weave retains alternating VI fields. Bob duplicates the current field, reducing motion combing at half vertical detail. Independent of VI filters. |
| `vi_aa` | `off`, `reduced`, `full` | Skip reconstruction, four neighbours, or six neighbours. |
| `vi_restore` | `disabled`, `enabled` | Dither restoration. |
| `vi_divot` | `disabled`, `enabled` | Median filtering of partially covered edges. |
| `vi_resample` | `enabled`, `disabled` | Native VI bilinear resampling, or point sampling. |
| `vi_gamma` | `enabled`, `disabled` | Allow the game's gamma correction. |
| `vi_gamma_dither` | `enabled`, `disabled` | Allow the game's gamma dithering independently of gamma correction. |

Enabled individual filters still obey the game's VI register requests. The
shared `wtfn64_vi_filter`, `wtfn64_vi_dedither` and `wtfn64_vi_blur` options apply
to Angrylion and paraLLEl-RDP; use the GLRDP-specific options for this renderer.

Interlaced output uses VI_CURRENT parity, per-field origins, half-line timing
and a persistent 640x480 (NTSC) or 640x576 (PAL) scanout texture. Frontend scaling
happens after field assembly. The first field seeds both line parities; later
fields replace only their own rows in weave mode. Blank/invalid output, renderer
reset and changes in format, stride, scale, active span or TV standard invalidate
history. The complete TV raster has a 4:3 canvas; the active VI window already
contains game letterboxing and does not set a second presentation aspect ratio.

VI image selection considers the displayed address span, including fractional
vertical offsets and the final resampling tap. A framebuffer whose tail merely
contains VI_ORIGIN cannot displace another view that covers the picture. This
handles WDC's selection menus, where 482-row render targets start 480 rows apart.
Equally complete views retain the usual preference for the newest image.

Supported RSP tasks use HLE automatically; undetected tasks, including custom
homebrew programs, execute on the native RSP. The CPU selector
and per-cycle timing are independent of GLRDP. Use 1x resolution when measuring
native rendering performance. See [intro benchmark instructions](../../../../../benchmarks/README.md).

[Diddy Kong Racing performance options](../../../../../benchmarks/glrdp-dkr-fast.opt)
provide a GL3.3, dynarec CPU, graphics/audio HLE, untimed, native-resolution
configuration with two-sample coverage, specialized shaders, fast shading,
fast arithmetic/coordinates, point textures, base tiles, dithering off and all
VI filters disabled. Apply these as
libretro core options and restart content. Accuracy defaults remain unchanged.

GLRDP selects GL 3.3 or GLES3 from the frontend video driver at content load.
No context core option is needed. Older frontends use the platform default
with fallback to the other GL API if the first context request is rejected.

Embedded systems need an OpenGL ES 3.0 or newer driver (for example Raspberry
Pi 4/5 with a suitable Mesa driver). GLES2-only hardware is no longer supported.
Batch capacity follows the driver's uniform-block limit, including the ES3
minimum of 16 KiB. Use native 1x resolution for initial performance comparisons;
physical Raspberry Pi performance must be measured on the device.
