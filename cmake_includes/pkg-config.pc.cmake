Name: ${PACKAGE_NAME}
Description: ${CPACK_PACKAGE_DESCRIPTION_SUMMARY}
Version: ${SDK_VERSION_MAJOR}.${SDK_VERSION_MINOR}.${SDK_VERSION_PATCH}
prefix=${CMAKE_INSTALL_PREFIX}
includedir=${includedir}
libdir=${libdir}
Libs: ${CPACK_PACKAGE_CONFIG_LIBS}
Cflags: ${CPACK_PACKAGE_CONFIG_CFLAGS}