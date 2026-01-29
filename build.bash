#!/usr/bin/env bash
set -eu

CUR_DIR="${PWD}"
cd "$(dirname "${BASH_SOURCE:-$0}")"

# Load environment variables from .env file if it exists
if [ -f ".env" ]; then
  set -a
  source .env
  set +a
fi

INSTALL_TOOLS=1
REBUILD=0
SKIP_TESTS=0
CREATE_DOCS=0
CREATE_ZIP=0
CREATE_INSTALLER=0
CMAKE_BUILD_TYPE=Release
ARCHS="x86_64"
USE_ADDRESS_SANITIZER=OFF
while [[ $# -gt 0 ]]; do
  case $1 in
    -d|--debug)
      CMAKE_BUILD_TYPE=Debug
      shift
      ;;
    -a|--arch)
      ARCHS="$2"
      shift 2
      ;;
    -r|--rebuild)
      REBUILD=1
      shift
      ;;
    -s|--skip-tests)
      SKIP_TESTS=1
      shift
      ;;
    -z|--zip)
      CREATE_ZIP=1
      CREATE_DOCS=1
      shift
      ;;
    -i|--installer)
      CREATE_INSTALLER=1
      shift
      ;;
    --asan)
      USE_ADDRESS_SANITIZER=ON
      shift
      ;;
    -*|--*)
      echo "Unknown option $1"
      exit 1
      ;;
    *)
      shift
      ;;
  esac
done

if [ "${INSTALL_TOOLS}" -eq 1 ]; then
  mkdir -p "build/tools"
  . "${PWD}/src/c/3rd/ovbase/setup-llvm-mingw.sh" --dir "${PWD}/build/tools"

  case "$(uname -s)" in
    MINGW64_NT* | MINGW32_NT*)
      SEVENZIP_URL="https://www.7-zip.org/a/7z2501-extra.7z"
      SEVENZIP_DIR="${PWD}/build/tools/7z2501-windows"
      SEVENZIP_ARCHIVE="${PWD}/build/tools/$(basename "${SEVENZIP_URL}")"
      if [ ! -d "${SEVENZIP_DIR}" ]; then
        if [ ! -f "${SEVENZIP_ARCHIVE}" ]; then
          echo "Downloading: ${SEVENZIP_URL}"
          curl -o "${SEVENZIP_ARCHIVE}" -sOL "$SEVENZIP_URL"
        fi
        mkdir -p "${SEVENZIP_DIR}"
        (cd "${SEVENZIP_DIR}" && cmake -E tar xf "${SEVENZIP_ARCHIVE}")
      fi
      export PATH="${SEVENZIP_DIR}:$PATH"
      ;;
    *)
      ;;
  esac
fi

# Run command with progress indicator (prints . every second)
run_with_progress() {
  local message="$1"
  local log_file="$2"
  shift 2
  printf "%s" "${message}"
  local start_time=$(date +%s.%N)
  "$@" > "${log_file}" 2>&1 &
  local pid=$!
  while kill -0 "${pid}" 2>/dev/null; do
    sleep 1
    printf "."
  done
  local end_time=$(date +%s.%N)
  local elapsed=$(awk "BEGIN {printf \"%.3f\", ${end_time} - ${start_time}}")
  wait "${pid}"
  local exit_code=$?
  if [ ${exit_code} -eq 0 ]; then
    echo " done (${elapsed}s)"
  fi
  return ${exit_code}
}

# Extract and display relevant error information from build log
show_error_log() {
  local log_file="$1"
  local error_block
  error_block=$(awk '
    /^FAILED:/ { printing=1; next }
    /^\[[0-9]+\/[0-9]+\]/ { printing=0 }
    /^ninja: build stopped/ { printing=0 }
    printing && /\.(exe|clang|gcc) / { next }
    printing { print }
  ' "${log_file}" 2>/dev/null | head -n 50)

  if [ -n "${error_block}" ]; then
    echo "${error_block}"
  else
    tail -n 30 "${log_file}"
  fi
  echo ""
  echo "(Full log: ${log_file})"
}

# Extract and display relevant test failure information from JUnit XML
show_test_log() {
  local log_file="$1"
  local xml_file="$(dirname "${log_file}")/testlog.xml"

  if [ -f "${xml_file}" ]; then
    awk '
      /<testsuite/,/>/ {
        if (/tests="/) { match($0, /tests="([0-9]+)"/, t); tests = t[1] }
        if (/failures="/) { match($0, /failures="([0-9]+)"/, f); failures = f[1] }
      }
      /<testcase / {
        match($0, /name="([^"]*)"/, arr)
        testname = arr[1]
        has_failure = 0
        system_out = ""
      }
      /<failure/ { has_failure = 1 }
      /<system-out>/ {
        in_system_out = 1
        gsub(/.*<system-out>/, "")
      }
      in_system_out {
        if (/<\/system-out>/) {
          gsub(/<\/system-out>.*/, "")
          system_out = system_out $0
          in_system_out = 0
        } else {
          system_out = system_out $0 "\n"
        }
      }
      /<\/testcase>/ {
        if (has_failure) {
          print "=== " testname " ==="
          print system_out
        }
      }
      END {
        if (failures > 0) {
          printf "%d/%d tests failed\n", failures, tests
        }
      }
    ' "${xml_file}"
    echo ""
    echo "(Full log: ${log_file})"
  else
    tail -n 30 "${log_file}"
    echo ""
    echo "(Full log: ${log_file})"
  fi
}

# Skip normal build process if only zip or installer is requested
if [ "${CREATE_INSTALLER}" -eq 0 ] && [ "${CREATE_ZIP}" -eq 0 ]; then
  for arch in $ARCHS; do
    builddir="${PWD}/build/${CMAKE_BUILD_TYPE}/${arch}"
    build_log="${builddir}/build.log"
    test_log="${builddir}/test.log"

    if [ "${REBUILD}" -eq 1 ] || [ ! -e "${builddir}/CMakeCache.txt" ]; then
      rm -rf "${builddir}"
      cmake -S . -B "${builddir}" --preset debug \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}" \
        -DCMAKE_TOOLCHAIN_FILE="src/c/3rd/ovbase/cmake/llvm-mingw.cmake" \
        -DCMAKE_C_COMPILER="${arch}-w64-mingw32-clang" \
        -DUSE_ADDRESS_SANITIZER="${USE_ADDRESS_SANITIZER}" > /dev/null 2>&1
    fi

    # Build with output captured
    if ! run_with_progress "Building ${arch}" "${build_log}" cmake --build "${builddir}"; then
      echo ""
      echo "Build failed for ${arch}:"
      show_error_log "${build_log}"
      exit 1
    fi

    # Run tests with output captured
    if [ "${SKIP_TESTS}" -eq 0 ]; then
      if ! run_with_progress "Testing ${arch}" "${test_log}" ctest --test-dir "${builddir}" --output-on-failure --output-junit testlog.xml; then
        echo ""
        echo "Tests failed for ${arch}:"
        show_test_log "${test_log}"
        exit 1
      fi
    fi

    echo "Build successful for ${arch}."
  done
fi

if [ "${CREATE_ZIP}" -eq 1 ] || [ "${CREATE_INSTALLER}" -eq 1 ]; then
  # Generate version.env
  builddir="${PWD}/build/${CMAKE_BUILD_TYPE}/x86_64"
  version_env="${builddir}/src/c/version.env"
  cmake \
    -Dlocal_dir="${PWD}" \
    -Dinput_file="${PWD}/src/c/version.h.in" \
    -Doutput_file="${builddir}/src/c/version.h" \
    -P "${PWD}/src/cmake/version.cmake"
  if [ ! -f "${version_env}" ]; then
    echo "Error: Failed to generate version.env"
    exit 1
  fi
  source "${version_env}"
  echo "Version: ${PTK_VERSION}"

  distdir="${PWD}/build/${CMAKE_BUILD_TYPE}/dist"
  if [ "${REBUILD}" -eq 1 ]; then
    rm -rf "${distdir}"
  fi
  mkdir -p "${distdir}"
fi

if [ "${CREATE_ZIP}" -eq 1 ]; then
  builddir="${PWD}/build/${CMAKE_BUILD_TYPE}/x86_64"
  zipname="psdtoolkit_${PTK_VERSION}.au2pkg.zip"
  (cd "${builddir}/bin" && cmake -E tar cf "${distdir}/${zipname}" --format=zip .)
fi

if [ "${CREATE_INSTALLER}" -eq 1 ]; then
  builddir="${PWD}/build/${CMAKE_BUILD_TYPE}/x86_64"
  installer_iss="${builddir}/installer.iss"

  # Generate installer script using CMake
  cmake \
    -Dlocal_dir="${PWD}" \
    -Dinput_file="${PWD}/installer.iss.in" \
    -Doutput_file="${installer_iss}" \
    -Dbuild_output_dir="${builddir}/bin" \
    -Dwork_dir="${builddir}" \
    -Doutput_dir="${distdir}" \
    -Dversion="${PTK_VERSION}" \
    -P "${PWD}/src/cmake/installer.cmake"

  # Find Inno Setup compiler
  ISCC_PATH=""
  for path in \
    "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
    "/c/Program Files/Inno Setup 6/ISCC.exe" \
    "${USERPROFILE}/AppData/Local/Programs/Inno Setup 6/ISCC.exe" \
    "$(command -v iscc 2>/dev/null || true)"; do
    if [ -f "$path" ]; then
      ISCC_PATH="$path"
      break
    fi
  done

  if [ -z "${ISCC_PATH}" ]; then
    echo "Warning: Inno Setup compiler (ISCC.exe) not found."
    echo "Installer script generated at: ${installer_iss}"
    echo "You can compile it manually with Inno Setup."
  else
    echo "Building installer with: ${ISCC_PATH}"
    "${ISCC_PATH}" "${installer_iss}"
  fi
fi

echo "Build script completed."
