// This code contains NVIDIA Confidential Information and is disclosed to you
// under a form of NVIDIA software license agreement provided separately to you.
//
// Notice
// NVIDIA Corporation and its licensors retain all intellectual property and
// proprietary rights in and to this software and related documentation and
// any modifications thereto. Any use, reproduction, disclosure, or
// distribution of this software and related documentation without an express
// license agreement from NVIDIA Corporation is strictly prohibited.
//
// ALL NVIDIA DESIGN SPECIFICATIONS, CODE ARE PROVIDED "AS IS.". NVIDIA MAKES
// NO WARRANTIES, EXPRESSED, IMPLIED, STATUTORY, OR OTHERWISE WITH RESPECT TO
// THE MATERIALS, AND EXPRESSLY DISCLAIMS ALL IMPLIED WARRANTIES OF NONINFRINGEMENT,
// MERCHANTABILITY, AND FITNESS FOR A PARTICULAR PURPOSE.
//
// Information and code furnished is believed to be accurate and reliable.
// However, NVIDIA Corporation assumes no responsibility for the consequences of use of such
// information or for any infringement of patents or other rights of third parties that may
// result from its use. No license is granted by implication or otherwise under any patent
// or patent rights of NVIDIA Corporation. Details are subject to change without notice.
// This code supersedes and replaces all information previously supplied.
// NVIDIA Corporation products are not authorized for use as critical
// components in life support devices or systems without express written approval of
// NVIDIA Corporation.
//
// Copyright (c) 2020-2021 NVIDIA Corporation. All rights reserved.

//
// ===============================================================================================
//
// Common declarations for GFN SDK APIs
//
// ===============================================================================================



#ifndef GFN_SDK_CAPI_H
#define GFN_SDK_CAPI_H

// Required for streamer / DLL download step in StartStream
#ifdef _WIN32
#   pragma comment(lib, "ws2_32.lib")
#   pragma comment(lib, "crypt32.lib")
#   pragma comment(lib, "Wldap32.lib")
#   pragma comment(lib, "version.lib")
#   pragma comment(lib, "shlwapi.lib")
#   pragma comment(lib, "wintrust")
#   define GFN_CALLBACK __stdcall
#   define NVGFNSDK_EXPORT __declspec(dllexport)
#   define NVGFNSDKApi __cdecl
#   include <stddef.h>
#else
// Support to be added in a later release
/// Future support
#   define GFN_CALLBACK
/// Future support
#   define NVGFNSDK_EXPORT
/// Future support
#   define NVGFNSDKApi
#endif

#ifndef __cplusplus
/// Simple aliasing for bool support
typedef char bool;
/// Simple aliasing for Boolean false
#define false 0
/// Simple aliasing for Boolean true
#define true 1
#endif

// Version info
#define NVGFNSDK_VERSION_MAJOR 1
#define NVGFNSDK_VERSION_MINOR 6
#define NVGFNSDK_VERSION_SHORT 1.6

#define NVGFNSDK_VERSION_PATCH 3004
#define NVGFNSDK_VERSION_BUILD 0364
#define NVGFNSDK_VERSION_LONG 1.6.3004.0364

#define NVGFNSDK_VERSION_STR   "1.6.3004.0364"

#ifdef __cplusplus
    extern "C"
    {
#endif
        // ============================================================================================
        // Constants/Enums
        // ============================================================================================

        /// @brief Returned by InitializeGfnRuntime and GfnRuntime API methods
        typedef enum GfnError
        {
            gfnSuccess = 0,
            gfnInitSuccessClientOnly = 1, ///< SDK initialized, but only cloud independent functionality available (such as gfnStartStream).
            gfnInitFailure = -1, ///< SDK initialization failure for any reason other than memory allocation failure.
            gfnDllNotPresent = -2,
            gfnComError = -3, ///< Geforce NOW SDK internal component communication error.
            gfnLibraryCallFailure = -4, ///< Geforce NOW SDK components were reachable, but could not serve the request.
            gfnIncompatibleVersion = -5,
            gfnUnableToAllocateMemory = -6,
            gfnInvalidParameter = -7,
            gfnInternalError = -8, ///< Generic Geforce NOW SDK internal error.
            gfnUnsupportedAPICall = -9,
            gfnInvalidToken = -10,
            gfnTimedOut = -11,
            gfnSetupTitleFailure = -12,
            gfnClientDownloadFailed = -13, ///< Failed to download the Geforce NOW client.
            gfnCallWrongEnvironment = -14, ///< Function limited to specific environment called in wrong environment
            gfnWebApiFailed = -15, ///< A call to a NVIDIA Web API failed to return valid data
            gfnStreamFailure = -16, ///< GeForceNOW Streamer hit a failure while starting a stream
            gfnAPINotFound = -17, ///< Library API call not found
            gfnAPINotInit = -18, ///< API not initialized
            gfnStreamStopFailure = -19, ///< Failed to stop active streaming session
            gfnUnhandledException = -20, ///< Unhandled exceptions
            gfnIPCFailure = -21, ///< Messagebus IPC failures
            gfnCanceled = -22, ///< Activity was canceled, for example, user canceled the download of GFN client
            gfnElevationRequired = -23, ///< API call required to be run from an elevated process
            gfnThrottled = -24, ///< API call throttled
            gfnInputExpected = -25 ///< API call was expecting input param to have a value
        } GfnError;

        ///
        /// @par Description
        /// GfnRuntimeError success function
        ///
        /// @par Usage
        /// Use to determine if GfnRuntimeError value translates to success
        ///
        /// @param r      - GfnRuntimeError type value
        ///
        /// @retval true  - GfnRuntimeError value indicates success
        /// @retval false - GfnRuntimeError value indicates failure
        inline bool GFNSDK_SUCCEEDED(GfnError r)
        {
            return r >= 0;
        }

        ///
        /// @par Description
        /// GfnRuntimeError failure function
        ///
        /// @par Usage
        /// Use to determine if GfnRuntimeError value translates to failure
        ///
        /// @param r      - GfnRuntimeError type value
        ///
        /// @retval true  - GfnRuntimeError value indicates failure
        /// @retval false - GfnRuntimeError value indicates success
        inline bool GFNSDK_FAILED(GfnError r)
        {
            return r < 0;
        }

        /// @brief Values for languages supported by the GFN SDK, used to define which language any SDK dialogs should be displayed in.
        typedef enum GfnDisplayLanguage
        {
            gfnDefaultLanguage = 0,         /// Uses the default system language
            gfn_bg_BG = 1,
            gfn_cs_CZ = 2,
            gfn_nl_NL = 3,
            gfn_de_DE = 4,
            gfn_el_GR = 5,
            gfn_en_US = 6,
            gfn_en_UK = 7,
            gfn_es_ES = 8,
            gfn_es_MX = 9,
            gfn_fi_FI = 10,
            gfn_fr_FR = 11,
            gfn_hu_HU = 12,
            gfn_it_IT = 13,
            gfn_ja_JP = 14,
            gfn_ko_KR = 15,
            gfn_nb_NO = 16,
            gfn_po_PO = 17,
            gfn_pt_BR = 18,
            gfn_pt_PT = 19,
            gfn_ro_RO = 20,
            gfn_ru_RU = 21,
            gfn_sv_SE = 22,
            gfn_th_TH = 23,
            gfn_tr_TR = 24,
            gfn_uk_UA = 25,
            gfn_zh_CN = 26,
            gfn_zh_TW = 27,
            gfn_en_GB = 28,
            gfn_hr_HR = 29,
            gfn_sk_SK = 30,
            gfn_sl_SI = 31,
            gfn_da_DK = 32,
            gfnMaxLanguage = gfn_da_DK
        } GfnDisplayLanguage;

        /// @brief Formats to specify a rect with top-left as origin
        typedef enum GfnRectFormat
        {
            /// value1 : Left
            /// value2 : Top
            /// value3 : Right
            /// value4 : Bottom
            gfnRectLTRB = 0,

            /// value1 : Top Left Corner's X coordinate
            /// value2 : Top Left Corner's Y coordinate
            /// value3 : Width
            /// value4 : Height
            gfnRectXYWH,

            /// Sentinel value, do not use
            gfnRectMAX
        } GfnRectFormat;

        /// @brief struct to reference a rect
        typedef struct GfnRect
        {
            float value1;         ///< value1 as per format
            float value2;         ///< value2 as per format
            float value3;         ///< value3 as per format
            float value4;         ///< value4 as per format
            bool normalized;      ///< true : coordinates are normalized between 0.0-1.0, false : absolute coordinates
            GfnRectFormat format; ///< rect format as listed in GfnRectFormat
        } GfnRect;

#ifdef __cplusplus
    } // extern "C"
#endif


#endif // GFN_SDK_CAPI_H
