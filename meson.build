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

nix_util_dep = dependency('nix-util', method: 'pkg-config')
nix_store_dep = dependency('nix-store', method: 'pkg-config')
nix_main_dep = dependency('nix-main', method: 'pkg-config')

sources = files(
    'main.cpp',
    'settings.cpp',
    'slurm.cpp',
)

executable('nsh', sources, dependencies : [
    restclient_dep,
    json_dep,
    boost_dep,
    nix_util_dep,
    nix_store_dep,
    nix_main_dep
])
