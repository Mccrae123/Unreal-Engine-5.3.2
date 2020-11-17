#
# Build LowLevel
#

SET(LL_SOURCE_DIR ${PHYSX_SOURCE_DIR}/LowLevel)

SET(LOWLEVEL_PLATFORM_INCLUDES
	$ENV{SCE_ORBIS_SDK_DIR}/target/include
)

# Use generator expressions to set config specific preprocessor definitions
SET(LOWLEVEL_COMPILE_DEFS 

	# Common to all configurations
	${PHYSX_PS4_COMPILE_DEFS};

	$<$<CONFIG:debug>:${PHYSX_PS4_DEBUG_COMPILE_DEFS};>
	$<$<CONFIG:checked>:${PHYSX_PS4_CHECKED_COMPILE_DEFS};>
	$<$<CONFIG:profile>:${PHYSX_PS4_PROFILE_COMPILE_DEFS};>
	$<$<CONFIG:release>:${PHYSX_PS4_RELEASE_COMPILE_DEFS};>
)

# include common low level settings
INCLUDE(${PHYSX_ENGINE_COMMON}/LowLevel.cmake)
