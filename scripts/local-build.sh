#!/usr/bin/env bash
set -euo pipefail

target="${1:-right}"
case "$target" in
  right|left|reset|all) ;;
  *)
    echo "Usage: $0 [right|left|reset|all]" >&2
    exit 2
    ;;
esac

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
output_dir="$repo_dir/../../../outputs/local-docker-build"
image="zmkfirmware/zmk-build-arm:3.5"
workspace_volume="kobitokey-xiao-plus-zmk-workspace"

mkdir -p "$output_dir"
docker volume create "$workspace_volume" >/dev/null

docker run --rm \
  --platform linux/amd64 \
  --mount "type=volume,src=$workspace_volume,dst=/workspace" \
  --mount "type=bind,src=$repo_dir/config,dst=/workspace/config,readonly" \
  --mount "type=bind,src=$output_dir,dst=/output" \
  --env "BUILD_TARGET=$target" \
  "$image" \
  bash -lc '
    set -euo pipefail

    cd /workspace
    if [ ! -d /workspace/.west ]; then
      west init -l /workspace/config
    fi

    west update --fetch-opt=--filter=tree:0
    west zephyr-export

    build_one() {
      name="$1"
      shield="$2"
      snippet="${3:-}"
      build_dir="/workspace/build/$name"

      cmake_args=("-DZMK_CONFIG=/workspace/config" "-DSHIELD=$shield")
      if [ -n "$snippet" ]; then
        cmake_args+=("-DSNIPPET=$snippet")
      fi

      west build -p always \
        -s /workspace/zmk/app \
        -d "$build_dir" \
        -b seeeduino_xiao_ble \
        -- "${cmake_args[@]}"

      cp "$build_dir/zephyr/zmk.uf2" "/output/$name.uf2"
    }

    case "$BUILD_TARGET" in
      right)
        build_one KobitoKey_right "KobitoKey_right rgbled_adapter" studio-rpc-usb-uart
        ;;
      left)
        build_one KobitoKey_left "KobitoKey_left rgbled_adapter"
        ;;
      reset)
        build_one settings_reset settings_reset
        ;;
      all)
        build_one KobitoKey_right "KobitoKey_right rgbled_adapter" studio-rpc-usb-uart
        build_one KobitoKey_left "KobitoKey_left rgbled_adapter"
        build_one settings_reset settings_reset
        ;;
    esac
  '

echo "Built firmware: $output_dir"
