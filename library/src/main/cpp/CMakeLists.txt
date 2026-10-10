cmake_minimum_required(VERSION 3.16)
project(pull_decoder)

set(CMAKE_CXX_STANDARD 17)

add_library(pull_decoder SHARED pull_decoder.cpp)

find_library(ace_napi_lib ace_napi.z)
find_library(media_vdec_lib native_media_vdec)
find_library(media_adec_lib native_media_adec)
find_library(media_acodec_lib native_media_acodec)
find_library(media_core_lib native_media_core)
find_library(media_codecbase_lib native_media_codecbase)
find_library(native_window_lib native_window)
find_library(hilog_lib hilog_ndk.z)

target_link_libraries(pull_decoder PUBLIC
    ${ace_napi_lib}
    ${media_vdec_lib}
    ${media_adec_lib}
    ${media_acodec_lib}
    ${media_core_lib}
    ${media_codecbase_lib}
    ${native_window_lib}
    ${hilog_lib}
)
