project('nix-scheduler-hook', 'cpp')
cmake = import('cmake')

restcpp_proj = cmake.subproject('restc-cpp')
restcpp = restcpp_proj.dependency('restc-cpp')


executable('demo', 'main.cpp', dependencies : restcpp)
