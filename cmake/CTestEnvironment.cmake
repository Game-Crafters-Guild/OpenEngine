# Read by ctest before it runs any test (the top directory's TEST_INCLUDE_FILES); every test
# process inherits the environment set here.
#
# OBS Studio installs an implicit Vulkan layer (VK_LAYER_OBS_HOOK) that the Vulkan loader adds
# to every Vulkan process on the machine. Tests run without it; the layer's manifest names this
# variable as its off switch.
set(ENV{DISABLE_VULKAN_OBS_CAPTURE} 1)
