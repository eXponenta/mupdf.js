#!/bin/bash

MUPDF_DIR=libmupdf
EMSDK_DIR=${EMSDK_DIR:-/opt/emsdk}

MUPDF_OPTS="-Os -mno-nontrapping-fptoint -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -DTOFU -DTOFU_CJK_EXT -DFZ_ENABLE_XPS=0 -DFZ_ENABLE_SVG=0 -DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0 -DFZ_ENABLE_HTML=0 -DFZ_ENABLE_EPUB=0 -DFZ_ENABLE_JS=0 -DFZ_ENABLE_OCR_OUTPUT=0 -DFZ_ENABLE_DOCX_OUTPUT=0 -DFZ_ENABLE_ODT_OUTPUT=0 -DMEMENTO_STACKTRACE_METHOD=0"

MUPDF_PATCH=patches/libmupdf-font-resolve.patch

export EMSDK_QUIET=1
source $EMSDK_DIR/emsdk_env.sh
echo

BUILD=${1:-release}

if [ -f "$MUPDF_PATCH" ]; then
	if git -C "$MUPDF_DIR" apply --reverse --check "../$MUPDF_PATCH" >/dev/null 2>&1; then
		echo MUPDF PATCH ALREADY APPLIED
	else
		echo APPLYING MUPDF PATCH
		git -C "$MUPDF_DIR" apply --check "../$MUPDF_PATCH" || exit 1
		git -C "$MUPDF_DIR" apply "../$MUPDF_PATCH" || exit 1
		MUPDF_PATCH_APPLIED=1
	fi
	echo
fi

cleanup_mupdf_patch() {
	if [ "$MUPDF_PATCH_APPLIED" = 1 ]; then
		git -C "$MUPDF_DIR" apply --reverse "../$MUPDF_PATCH"
	fi
}
trap cleanup_mupdf_patch EXIT

echo BUILDING MUPDF CORE
make -j4 -C $MUPDF_DIR build=$BUILD OS=wasm XCFLAGS="$MUPDF_OPTS" libs
echo

echo BUILDING MUPDF WASM
mkdir -p dist
emcc -o dist/mupdf-wasm.js -I $MUPDF_DIR/include \
   -O3\
   -mno-nontrapping-fptoint \
   -fwasm-exceptions \
   --no-entry \
   -sSUPPORT_LONGJMP=wasm \
   -sABORTING_MALLOC=0 \
   -sALLOW_MEMORY_GROWTH=1 \
   -sMAXIMUM_MEMORY=4GB \
   -sNODEJS_CATCH_EXIT=0 \
   -sMODULARIZE=1 \
   -sEXPORT_ES6=1 \
   -sEXPORT_NAME='"libmupdf_wasm"' \
   -sTEXTDECODER=2 \
   -sFILESYSTEM=0 \
   -sEXPORTED_RUNTIME_METHODS='["ccall","UTF8ToString","lengthBytesUTF8","stringToUTF8","HEAP8","HEAPU8","HEAPU32","HEAPF32"]' \
   src/mupdf.c \
    $MUPDF_DIR/build/wasm/$BUILD/libmupdf.a \
    $MUPDF_DIR/build/wasm/$BUILD/libmupdf-third.a
echo

echo BUILDING TYPESCRIPT
cat src/mupdf.c | sed '/#include/d' | emcc -E - | node src/gen-wasm-type.js > src/mupdf-wasm.d.ts
npx tsc -p .
