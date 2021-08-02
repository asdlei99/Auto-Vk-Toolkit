# How-To Update ImGui for Gears-Vk
- Checkout [ImGui](https://github.com/ocornut/imgui)
- Copy every `.cpp` file from `imgui/` into `external/universal/src/`
- Copy `imgui/backends/imgui_impl_glfw.cpp` and `imgui/backends/imgui_impl_vulkan.cpp` into `external/universal/src/`
- Copy every `.h` file from `imgui/` into `external/universal/include/`
- Copy `imgui/backends/imgui_impl_glfw.h` and `imgui/backends/imgui_impl_vulkan.h` into `external/universal/include/`
- Open PowerShell, change directory to `external/tools/` and execute the script `modify_imgui_for_gearsvk.ps1`
- Ensure that the script reports that
	- One modification has been made to `imgui_impl_vulkan.h`
	- Three modifications have been made to `imgui_impl_vulkan.cpp`
- Check if every `.cpp` file in `external/universal/source/` is included in the Visual Studio framework library project `gears-vk`