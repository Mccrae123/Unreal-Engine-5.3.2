#!/bin/sh
# Copyright Epic Games, Inc. All Rights Reserved.

##
## Most of the following script is intended to be consistent for building all Mac
## third-party source. The sequence of steps are -
## 1) Set up constants, create temp dir, checkout files, save file info
## 2) lib-specific build steps
## 3) Check files were updated

ENABLE_CHECKOUT_FILES="0"

##
# Common library constants

# Drops from the location of this script to where libfiles are relative to
#  e.g.
#  {DROP_TO_LIBROOT}/README
#  {DROP_TO_LIBROOT}/include)
#  ${DROP_TO_LIBROOT}/$LIBFILES[0])
DROP_TO_LIBROOT=../..

# Drops from the location of LIBROOT to Engine/Source/ThirdParrty
DROP_TO_THIRDPARTY=..

ZLIB_VERSION="v1.2.8"
ZLIB_BRANCH="${ZLIB_VERSION}"

OSSL_VERSION="1.1.1"
OSSL_BRANCH="OpenSSL_1_1_1-stable"

## TODO: Enable when/if needed for macOS
#LIBCURL_VERSION=curl-7_65_3
#LIBCURL_BRANCH="${LIBCURL_VERSION}"

pushd . > /dev/null 2>&1

SCRIPT_DIR="`dirname "${BASH_SOURCE[0]}"`"

source ${SCRIPT_DIR}/${DROP_TO_LIBROOT}/${DROP_TO_THIRDPARTY}/BuildScripts/Mac/Common/Common.sh

##
# Build zlib
#
# Note, zlib is built first, as a dependency for OpenSSL.
#
build_zlib()
{
	LIB_NAME="zlib"

	pushd "${SCRIPT_DIR}/${DROP_TO_LIBROOT}/${DROP_TO_THIRDPARTY}/${LIB_NAME}" > /dev/null 2>&1

	DEPLOYED_LIBS="${ZLIB_VERSION}/lib/Mac"
	DEPLOYED_INCS="${ZLIB_VERSION}/include/Mac"

	LIBFILES=( "${DEPLOYED_LIBS}/libz.a" "${DEPLOYED_LIBS}/libz.1.2.8.dylib" )
	INCFILES=( "${DEPLOYED_INCS}/zconf.h" "${DEPLOYED_INCS}/zlib.h" )

	SRCROOT="/tmp/${LIB_NAME}"
	DSTROOT="`pwd`"

	# Save these for later use (when building OpenSSL).
	ZLIB_LIB_ROOT="${DSTROOT}/${DEPLOYED_LIBS}"
	ZLIB_INC_ROOT="${DSTROOT}/${DEPLOYED_INCS}"

	if [ "${ENABLE_CHECKOUT_FILES}" == "1" ]; then
		checkoutFiles ${LIBFILES[@]} ${INCFILES[@]}
	fi
	saveFileStates ${LIBFILES[@]} ${INCFILES[@]}

	PREFIX_ROOT="${SRCROOT}/Deploy"

	echo "================================================================================"
	echo "Building ${LIB_NAME}"
	#echo "--------------------------------------------------------------------------------"
	#echo "	Common env.:"
	#echo "	  - DEPLOYED_LIBS: ${DEPLOYED_LIBS}"
	#echo "	  - DEPLOYED_INCS: ${DEPLOYED_INCS}"
	#echo "	  - LIBFILES     : ${LIBFILES[@]}"
	#echo "	  - INCFILES     : ${INCFILES[@]}"
	#echo "	  - SRCROOT      : ${SRCROOT}"
	#echo "	  - DSTROOT      : ${DSTROOT}"
	#echo "--------------------------------------------------------------------------------"
	#echo "	Function export env. vars.:"
	#echo "	  - ZLIB_LIB_ROOT: ${ZLIB_LIB_ROOT}"
	#echo "	  - ZLIB_INC_ROOT: ${ZLIB_INC_ROOT}"
	#echo "--------------------------------------------------------------------------------"
	#echo "	Local build env.:"
	#echo "	  - PREFIX_ROOT  : ${PREFIX_ROOT}"
	echo "================================================================================"

	rm -rf "${SRCROOT}"
	mkdir -p "${SRCROOT}"/{Deploy,Intermediate}

	pushd "${SRCROOT}" > /dev/null 2>&1

	git clone https://github.com/madler/zlib.git Source

	cd Source
	git checkout "${ZLIB_BRANCH}" -b "${ZLIB_BRANCH}"

	cd ../Intermediate
	cmake -G 'Unix Makefiles' \
		-DCMAKE_INSTALL_PREFIX:PATH="${PREFIX_ROOT}" \
		-DCMAKE_OSX_DEPLOYMENT_TARGET="10.13" \
		-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
		"${SRCROOT}/Source"
	make -j$(get_core_count) && make install

	cd ..

	ditto Deploy/lib "${DSTROOT}/${DEPLOYED_LIBS}"
	ditto Deploy/include "${DSTROOT}/${DEPLOYED_INCS}"

	popd > /dev/null

	echo "================================================================================"
	echo "Checking built file status:"
	checkFilesWereUpdated ${LIBFILES[@]} ${INCFILES[@]}
	echo "================================================================================"
	checkFilesAreFatBinaries ${LIBFILES[@]}
	echo "================================================================================"
	echo "${LIB_NAME} was successfully built and updated."
	echo "================================================================================"
	echo ""

	popd > /dev/null
}

build_openssl()
{
	LIB_NAME="OpenSSL"

	pushd "${SCRIPT_DIR}/${DROP_TO_LIBROOT}/${DROP_TO_THIRDPARTY}/${LIB_NAME}" > /dev/null 2>&1

	DEPLOYED_LIBS="1.1.1/Lib/Mac"
	DEPLOYED_INCS="1.1.1/Include/Mac"

	LIBFILES=( "`find "${DEPLOYED_LIBS}" -type f -print0 | xargs -0 echo`" )
	INCFILES=( "`find "${DEPLOYED_INCS}" -type f -print0 | xargs -0 echo`" )

	SRCROOT="/tmp/${LIB_NAME}"
	DSTROOT="`pwd`"

	OSSL_ARCHS=( "x86_64" "arm64" )

	# Save these for later use (when building libcurl).
	OSSL_LIB_ROOT="${DSTROOT}/${DEPLOYED_LIBS}"
	OSSL_INC_ROOT="${DSTROOT}/${DEPLOYED_INCS}"

	if [ "${ENABLE_CHECKOUT_FILES}" == "1" ]; then
		checkoutFiles ${LIBFILES[@]} ${INCFILES[@]}
	fi
	saveFileStates ${LIBFILES[@]} ${INCFILES[@]}

	PREFIX_ROOT="${SRCROOT}/Deploy"

	echo "================================================================================"
	echo "Building ${LIB_NAME}"
	#echo "--------------------------------------------------------------------------------"
	#echo "	Common env.:"
	#echo "	  - DEPLOYED_LIBS: ${DEPLOYED_LIBS}"
	#echo "	  - DEPLOYED_INCS: ${DEPLOYED_INCS}"
	#echo "	  - LIBFILES     : ${LIBFILES[@]}"
	#echo "	  - INCFILES     : ${INCFILES[@]}"
	#echo "	  - SRCROOT      : ${SRCROOT}"
	#echo "	  - DSTROOT      : ${DSTROOT}"
	#echo "--------------------------------------------------------------------------------"
	#echo "	Imported env. vars.:"
	#echo "	  - ZLIB_LIB_ROOT: ${ZLIB_LIB_ROOT}"
	#echo "	  - ZLIB_INC_ROOT: ${ZLIB_INC_ROOT}"
	#echo "--------------------------------------------------------------------------------"
	#echo "	Local build env.:"
	#echo "	  - PREFIX_ROOT  : ${PREFIX_ROOT}"
	echo "================================================================================"

	rm -rf "${SRCROOT}"
	mkdir -p "${PREFIX_ROOT}"/Universal/{bin,lib}

	pushd "${SRCROOT}" > /dev/null 2>&1

	git clone https://github.com/openssl/openssl.git Source

	cd Source
	git checkout "${OSSL_BRANCH}"
	patch -p1 --no-backup-if-mismatch < "${DSTROOT}/Patches/darwin64-arm64-cc.patch"

	for OSSL_ARCH in "${OSSL_ARCHS[@]}"; do
		make clean > clean_${OSSL_ARCH}_log.txt 2>&1
		make distclean >> clean_${OSSL_ARCH}_log.txt 2>&1
		./Configure shared threads zlib \
			--with-zlib-lib="${ZLIB_LIB_ROOT}" \
			--with-zlib-include="${ZLIB_INC_ROOT}" \
			--prefix="${PREFIX_ROOT}/${OSSL_ARCH}" \
			--openssldir="${PREFIX_ROOT}/${OSSL_ARCH}" \
			darwin64-${OSSL_ARCH}-cc
		make -j$(get_core_count) > build_${OSSL_ARCH}_log.txt 2>&1
		make install > install_${OSSL_ARCH}_log.txt 2>&1
	done

	cd ../Deploy

	# All architectures will have the same built products, so just look at one of
	# them for the list.
	BINLIBS=$(for i in `cd "${PREFIX_ROOT}/${OSSL_ARCHS[0]}" && find {bin,lib} \( -type f -and \! -name ".DS_Store" -and \! -name "*.pc" -and \! -name "c_rehash" \)`; do echo $i; done)
	BINLNKS=$(for i in `cd "${PREFIX_ROOT}/${OSSL_ARCHS[0]}" && find {bin,lib} \( -type l \)`; do echo $i; done)

	cd Universal
	for i in ${BINLIBS}; do
		mkdir -p `dirname $i`
		lipo -create "${PREFIX_ROOT}/${OSSL_ARCHS[0]}/$i" "${PREFIX_ROOT}/${OSSL_ARCHS[1]}/$i" -output "$i"
	done
	for i in ${BINLNKS}; do
		cp -pPR "${PREFIX_ROOT}/${OSSL_ARCHS[0]}/$i" "$i"
	done

	ditto "${PREFIX_ROOT}/Universal/lib" "${DSTROOT}/${DEPLOYED_LIBS}"
	ditto "${PREFIX_ROOT}/${OSSL_ARCHS[0]}/include" "${DSTROOT}/${DEPLOYED_INCS}"

	popd > /dev/null

	echo "================================================================================"
	echo "Checking built file status:"
	checkFilesWereUpdated ${LIBFILES[@]} ${INCFILES[@]}
	echo "================================================================================"
	checkFilesAreFatBinaries ${LIBFILES[@]}
	echo "================================================================================"
	echo "${LIB_NAME} was successfully built and updated."
	echo "================================================================================"
	echo ""

	popd > /dev/null
}

##
#TODO: Build libcurl as universal when/if needed for macOS
#build_libcurl()
#{
#}

##
#TODO: Build WebRTC as universal when/if needed for macOS
#build_webrtc()
#{
#}

build_zlib
build_openssl

## TODO: Enable when/if needed for macOS
#build_libcurl
#build_webrtc

popd > /dev/null
