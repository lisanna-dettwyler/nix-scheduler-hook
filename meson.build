project(
    'nix-scheduler-hook',
    'cpp',
    default_options : ['cpp_std=c++23']
)
cmake = import('cmake')

restcpp_proj = cmake.subproject('restc-cpp')
restcpp = restcpp_proj.dependency('restc-cpp')

nix_proj = subproject('nix_top')
nix_util_proj = subproject('libutil')
nix_util_headers = nix_util_proj.get_variable('include_dirs')
nix_util_lib = nix_util_proj.get_variable('this_library')
nix_util = declare_dependency(include_directories: nix_util_headers, link_with: nix_util_lib)

executable('demo', 'main.cpp', dependencies : [restcpp, nix_util])
