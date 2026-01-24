#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef SNAPI_ASSETPIPELINE_EXPORTS
        #define SNAPI_ASSETPIPELINE_API __declspec(dllexport)
    #else
        #define SNAPI_ASSETPIPELINE_API __declspec(dllimport)
    #endif
    #define SNAPI_ASSETPIPELINE_PLUGIN_API __declspec(dllexport)
#else
    #if __GNUC__ >= 4
        #define SNAPI_ASSETPIPELINE_API __attribute__((visibility("default")))
        #define SNAPI_ASSETPIPELINE_PLUGIN_API __attribute__((visibility("default")))
    #else
        #define SNAPI_ASSETPIPELINE_API
        #define SNAPI_ASSETPIPELINE_PLUGIN_API
    #endif
#endif
