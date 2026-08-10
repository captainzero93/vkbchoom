project('smaa_dxvk_layer', ['c', 'cpp'],
    default_options: [
        'c_std=c11',
        'cpp_std=c++20',
        # Static CRT: the release build otherwise links /MD by default
        # (Meson's b_vscrt default is 'from_buildtype', which maps
        # release -> md), making smaa_layer.dll depend on the MSVC
        # Redistributable (vcruntime140.dll etc.) being installed on
        # the target machine. Nothing about a normal Steam+MGSV
        # install guarantees that's present, and a missing dependency
        # makes LoadLibrary fail -- which the Vulkan loader treats as
        # "implicit layer unavailable, skip it" with zero surfaced
        # error. Static linking removes the dependency entirely so the
        # layer works on a bare Windows install with just GPU drivers.
        'b_vscrt=static_from_buildtype',
    ])

subdir('src')
subdir('tools')
# Windows layer registration is a registry entry, not a filesystem
# install like on Linux -- see config/vkbchoom.json and README.md
# for the manual registration step. install_vkbchoom.exe (built from
# tools/) automates writing that registry entry.
