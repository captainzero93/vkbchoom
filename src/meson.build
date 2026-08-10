subdir('shader')

vulkan_dep = dependency('vulkan')

smaa_layer_src = [
    'basalt.cpp',
    'buffer.cpp',
    'command_buffer.cpp',
    'config.cpp',
    'descriptor_set.cpp',
    'effect.cpp',
    'effect_bwr.cpp',
    'effect_depth_outline.cpp',
    'effect_posterize.cpp',
    'effect_rcas.cpp',
    'effect_smaa.cpp',
    'effect_transfer.cpp',
    'fake_swapchain.cpp',
    'format.cpp',
    'framebuffer.cpp',
    'graphics_pipeline.cpp',
    'image.cpp',
    'image_view.cpp',
    'logger.cpp',
    'logical_swapchain.cpp',
    'memory.cpp',
    'platform_win32.cpp',   # NEW: DllMain breadcrumb + path resolution
    'renderpass.cpp',
    'sampler.cpp',
    'screenshot.cpp',
    'shader.cpp',
    'util.cpp',
    'vkdispatch.cpp',
]

shared_library('smaa_layer',
    smaa_layer_src, shader_include,
    include_directories : include_directories('.'),
    dependencies : [vulkan_dep],
    gnu_symbol_visibility: 'hidden',
    # Explicit export table. Guarantees vkNegotiateLoaderLayerInterfaceVersion
    # is exported regardless of toolchain -- the old build got that symbol out
    # via #pragma comment(linker, "/export:..."), which MSVC honours but GCC
    # ignores, and gnu_symbol_visibility:'hidden' would then have hidden it.
    # Meson passes this to /DEF: on MSVC and to the linker on MinGW.
    vs_module_defs : 'smaa_layer.def',
    install : true)
