# Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.
# All rights reserved.
#
# For the licensing terms see $ROOTSYS/LICENSE.
# For the list of contributors see $ROOTSYS/README/CREDITS.

include(ExternalProject)
include(FindPackageHandleStandardArgs)

# find_package(OpenSSL) may not have found it,
# clear parent scope variables set to NOTFOUND
foreach(suffix FOUND INCLUDE_DIR INCLUDE_DIRS CRYPTO_LIBRARY SSL_LIBRARY LIBRARY LIBRARIES VERSION)
  unset(OPENSSL_${suffix} PARENT_SCOPE)
endforeach()

set(OPENSSL_VERSION "3.5.0")
set(OPENSSL_URL "http://lcgpackages.web.cern.ch/lcgpackages/tarFiles/sources/openssl-${OPENSSL_VERSION}.tar.gz")
set(OPENSSL_URLHASH "SHA256=344d0a79f1a9b08029b0744e2cc401a43f9c90acd1044d09a530b4885a8e9fc0")
set(OPENSSL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/OPENSSL-prefix)

foreach(lib ssl crypto)
  string(TOUPPER ${lib} libupper)
  set(libname ${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${CMAKE_STATIC_LIBRARY_SUFFIX})
  set(OPENSSL_${libupper}_LIBRARY ${OPENSSL_PREFIX}/lib/${libname})
endforeach()

if(APPLE)
  if(${ROOT_ARCHITECTURE} MATCHES "macosxarm64")
    set(OPENSSL_CONFIG_CMD ./Configure darwin64-arm64-cc)
  else()
    set(OPENSSL_CONFIG_CMD ./Configure darwin64-x86_64-cc)
  endif()
  if (CMAKE_OSX_SYSROOT)
    set(OSX_SYSROOT "-isysroot ${CMAKE_OSX_SYSROOT}")
  endif()
else()
  set(OPENSSL_CONFIG_CMD ./config)
endif()

ExternalProject_Add(OPENSSL
  URL ${OPENSSL_URL} URL_HASH ${OPENSSL_URLHASH}
  CONFIGURE_COMMAND ${OPENSSL_CONFIG_CMD} no-shared --prefix=<INSTALL_DIR>
  BUILD_COMMAND make -j1 CC=${CMAKE_C_COMPILER}\ -fPIC\ ${OSX_SYSROOT}
  INSTALL_COMMAND make install_sw
  BUILD_IN_SOURCE 1
  LOG_BUILD 1 LOG_CONFIGURE 1 LOG_DOWNLOAD 1 LOG_INSTALL 1
  BUILD_BYPRODUCTS ${OPENSSL_CRYPTO_LIBRARY} ${OPENSSL_SSL_LIBRARY}
  TIMEOUT 600)

set(OPENSSL_FOUND TRUE CACHE BOOL "" FORCE)
set(OPENSSL_VERSION ${OPENSSL_VERSION} CACHE INTERNAL "" FORCE)
set(OPENSSL_VERSION_STRING "${OPENSSL_VERSION}" CACHE INTERNAL "" FORCE)
set(OPENSSL_PREFIX ${OPENSSL_PREFIX} CACHE INTERNAL "" FORCE) # needed by Davix
set(OPENSSL_INCLUDE_DIR ${OPENSSL_PREFIX}/include CACHE INTERNAL "" FORCE)
set(OPENSSL_INCLUDE_DIRS ${OPENSSL_PREFIX}/include CACHE INTERNAL "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_CRYPTO_LIBRARY} CACHE INTERNAL "" FORCE)
set(OPENSSL_SSL_LIBRARY ${OPENSSL_SSL_LIBRARY} CACHE INTERNAL "" FORCE)
set(OPENSSL_LIBRARIES ${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY} ${CMAKE_DL_LIBS} CACHE INTERNAL "" FORCE)
set(OPENSSL_ROOT ${OPENSSL_PREFIX} CACHE INTERNAL "Location of the builtin OpenSSL installation" FORCE)

# Dependent libraries might check for the existence of the include directories
file(MAKE_DIRECTORY ${OPENSSL_INCLUDE_DIR})

add_library(builtin_crypto INTERFACE)
target_include_directories(builtin_crypto INTERFACE $<BUILD_INTERFACE:${OPENSSL_INCLUDE_DIR}>)
target_link_libraries(builtin_crypto INTERFACE $<BUILD_INTERFACE:${OPENSSL_CRYPTO_LIBRARY}>)
add_dependencies(builtin_crypto OPENSSL)

add_library(builtin_ssl INTERFACE)
target_include_directories(builtin_ssl INTERFACE $<BUILD_INTERFACE:${OPENSSL_INCLUDE_DIR}>)
target_link_libraries(builtin_ssl INTERFACE $<BUILD_INTERFACE:${OPENSSL_LIBRARIES}>)
add_dependencies(builtin_ssl OPENSSL)

add_library(OpenSSL::Crypto ALIAS builtin_crypto)
add_library(OpenSSL::SSL ALIAS builtin_ssl)

find_package_handle_standard_args(OpenSSL
  FOUND_VAR
    OPENSSL_FOUND
  REQUIRED_VARS
    OPENSSL_INCLUDE_DIR
    OPENSSL_CRYPTO_LIBRARY
    OPENSSL_SSL_LIBRARY
    OPENSSL_LIBRARIES
  VERSION_VAR
    OPENSSL_VERSION_STRING
)

set_property(GLOBAL APPEND PROPERTY ROOT_BUILTIN_TARGETS OPENSSL)
