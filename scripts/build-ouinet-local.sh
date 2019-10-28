#!/bin/bash

#
# Builds ouinet from the local source containing this file
#
# If BOOST_ROOT env variable is set it adds -DBOOST_ROOT=$BOOST_ROOT
# to cmake args

set -e

DIR=$(pwd)
SOURCEDIR=$(dirname "$(realpath "${BASH_SOURCE}")")/..
BINDIR="${DIR}"/ouinet-local-bin
BUILDDIR="${DIR}"/ouinet-local-build

if [[ ! -e ${BUILDDIR}/Makefile ]]; then
	rm -rf "${BUILDDIR}"
	mkdir "${BUILDDIR}"
	cd "${BUILDDIR}"
	cmake "${SOURCEDIR}" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX="${BINDIR}"
fi

cd "${BUILDDIR}"
make -j`nproc`

# Not supported yet
#make install

cd "${DIR}"
