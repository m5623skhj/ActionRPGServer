# Based on vcpkg port ed87e2b37a5c5fafdd012bdf4194dd1a9283182b (0.40.0).
# CMake 3.31.10 cannot extract the tarball's Japanese test paths on CP949 Windows.
# The ZIP contains the same source files and preserves upstream build settings.
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/yhirose/cpp-httplib/archive/refs/tags/v${VERSION}.zip"
    FILENAME "yhirose-cpp-httplib-v${VERSION}.zip"
    SHA512 105aced6ae8ca22d868aed19a41a56463be12850726e9d80f87dc7a717ed1d09007ec8c24bb100898ede93ef488ef5f6323837c9ecaf091ac8e041b31da83b84
)
vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    SOURCE_BASE "v${VERSION}"
    PATCHES fix-find-brotli.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        brotli  HTTPLIB_REQUIRE_BROTLI
        openssl HTTPLIB_REQUIRE_OPENSSL
        zlib    HTTPLIB_REQUIRE_ZLIB
        zstd    HTTPLIB_REQUIRE_ZSTD
)

set(VCPKG_BUILD_TYPE release) # header-only port

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
    ${FEATURE_OPTIONS}
    -DHTTPLIB_USE_OPENSSL_IF_AVAILABLE=OFF
    -DHTTPLIB_USE_ZLIB_IF_AVAILABLE=OFF
    -DHTTPLIB_USE_BROTLI_IF_AVAILABLE=OFF
    -DHTTPLIB_USE_ZSTD_IF_AVAILABLE=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME httplib CONFIG_PATH lib/cmake/httplib)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
