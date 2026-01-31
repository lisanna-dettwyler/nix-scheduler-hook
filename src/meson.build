project(
    'nix-scheduler-hook',
    'cpp',
    version: '0.4.0',
    default_options : ['cpp_std=c++23']
)
cmake = import('cmake')

rectclient_proj = cmake.subproject('restclient-cpp')
restclient_dep = rectclient_proj.dependency('restclient-cpp')

json_dep = dependency('nlohmann_json')

boost_dep = dependency('boost', modules : ['fusion'])

pbs_dep = dependency('pbs')

nix_util_dep = dependency('nix-util', method: 'pkg-config')
nix_store_dep = dependency('nix-store', method: 'pkg-config')
nix_main_dep = dependency('nix-main', method: 'pkg-config')

sources = files(
    'main.cpp',
    'settings.cpp',
    'slurm.cpp',
    'pbs.cpp',
)

executable('nsh', sources, dependencies : [
    restclient_dep,
    json_dep,
    boost_dep,
    pbs_dep,
    nix_util_dep,
    nix_store_dep,
    nix_main_dep
])
