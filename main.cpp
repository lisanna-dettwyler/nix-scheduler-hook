#include <iostream>

#include <restc-cpp/restc-cpp.h>
using namespace restc_cpp;

#include <nix/util/configuration.hh>

int main(int argc, char **argv) {
    std::cout << "Hello" << std::endl;

    auto rest_client = RestClient::Create();
}
