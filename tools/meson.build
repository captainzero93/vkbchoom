cpp_compiler = meson.get_compiler('cpp')

# advapi32 : registry (RegOpenKeyEx / RegSetValueEx / RegEnumValue)
# version  : GetFileVersionInfo / VerQueryValue, used to report the versions
#            of DXVK's d3d11.dll and of the Vulkan loader
# user32   : SendMessageTimeout, to broadcast WM_SETTINGCHANGE after writing
#            the debug environment variables
advapi32_dep = cpp_compiler.find_library('advapi32')
version_dep  = cpp_compiler.find_library('version')
user32_dep   = cpp_compiler.find_library('user32')

executable('install_vkbchoom', 'install_vkbchoom.cpp',
    dependencies : [advapi32_dep, version_dep, user32_dep],
    install : true)
