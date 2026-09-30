#!/usr/bin/env bash
set -euo pipefail

rid="${1:?usage: restore-current-native-sdks.sh <rid>}"
: "${GH_TOKEN:?${GITHUB_TOKEN:+GH_TOKEN is required; set it from GITHUB_TOKEN}}"

root="${RUNNER_TEMP:-/tmp}/turboscript-current-native-sdks"
mkdir -p "$root"

restore_sdk() {
  local repo="$1"
  local pattern="$2"
  local name="$3"
  local config_rel="$4"
  local root_env="$5"
  local release_env="$6"
  local dir="$root/$name"
  local tag
  local nupkg
  local sdk

  tag="$(gh release view --repo "$repo" --json tagName --jq .tagName)"
  test -n "$tag"

  rm -rf "$dir"
  mkdir -p "$dir/download" "$dir/package"
  gh release download "$tag" --repo "$repo" --pattern "$pattern" \
    --dir "$dir/download" --clobber

  nupkg="$(find "$dir/download" -maxdepth 1 -type f -name '*.nupkg' -print -quit)"
  test -n "$nupkg"
  unzip -q "$nupkg" -d "$dir/package"

  sdk="$dir/package/sdk/$rid"
  test -f "$sdk/$config_rel"

  if [[ -n "${GITHUB_ENV:-}" ]]; then
    echo "$root_env=$sdk" >> "$GITHUB_ENV"
    echo "$release_env=$tag" >> "$GITHUB_ENV"
  else
    printf '%s=%q\n' "$root_env" "$sdk"
    printf '%s=%q\n' "$release_env" "$tag"
  fi
}

restore_sdk qigao/salts 'Salts.Native.*.nupkg' salts \
  'lib/cmake/Salts/SaltsConfig.cmake' SALTS_ROOT SALTS_SDK_RELEASE
restore_sdk qigao/salts-utils 'SaltsUtils.Native.*.nupkg' salts-utils \
  'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake' SALTS_UTILS_ROOT SALTS_UTILS_SDK_RELEASE
restore_sdk qigao/chttp 'CHttp.Native.*.nupkg' chttp \
  'lib/cmake/Chttp/ChttpConfig.cmake' CHTTP_ROOT CHTTP_SDK_RELEASE
restore_sdk qigao/TurboDB 'TurboDB.Native.*.nupkg' turbodb \
  'lib/cmake/TurboDB/TurboDBConfig.cmake' TURBODB_ROOT TURBODB_SDK_RELEASE
