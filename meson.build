project(
    'nix-scheduler-hook',
    'cpp',
    default_options : ['cpp_std=c++23']
)
cmake = import('cmake')

# restcpp_proj = cmake.subproject('restc-cpp')
# restcpp = restcpp_proj.dependency('restc-cpp')

rectclient_proj = cmake.subproject('restclient-cpp')
restclient_dep = rectclient_proj.dependency('restclient-cpp')

json_proj = subproject('nlohmann-json')
json_dep = json_proj.get_variable('nlohmann_json_dep')

boost_dep = dependency('boost', modules : ['fusion'])

nix_proj = subproject('nix_top')
nix_util_proj = subproject('libutil')
nix_util_headers = nix_util_proj.get_variable('include_dirs')
nix_util_lib = nix_util_proj.get_variable('this_library')
nix_util = declare_dependency(include_directories: nix_util_headers, link_with: nix_util_lib)
nix_store_proj = subproject('libstore')
nix_store_headers = nix_store_proj.get_variable('include_dirs')
nix_store_lib = nix_store_proj.get_variable('this_library')
nix_store = declare_dependency(include_directories: nix_store_headers, link_with: nix_store_lib)
nix_main_proj = subproject('libmain')
nix_main_headers = nix_main_proj.get_variable('include_dirs')
nix_main_lib = nix_main_proj.get_variable('this_library')
nix_main = declare_dependency(include_directories: nix_main_headers, link_with: nix_main_lib)

sources = files(
    'main.cpp',
    'settings.cpp',
    'slurm.cpp',
)

executable('nsh', sources, dependencies : [restclient_dep, json_dep, boost_dep, nix_util, nix_store, nix_main])
