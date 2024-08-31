# Automatically generated by scripts/boost/generate-ports.ps1

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO boostorg/property_map_parallel
    REF boost-1.77.0
    SHA512 e564ab747b9885002d983fd42196f2a84655ebf74a0fd42647067dca075b8a2462d5b88d65faa9e91284018a373c5b689749c42a9812922be222ebfb24064a91
    HEAD_REF master
)

include(${CURRENT_INSTALLED_DIR}/share/boost-vcpkg-helpers/boost-modular-headers.cmake)
boost_modular_headers(SOURCE_PATH ${SOURCE_PATH})
