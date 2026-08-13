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
workspace_volume="${ZMK_WORKSPACE_VOLUME:-kobitokey-xiao-plus-zmk-workspace}"

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

    # The persistent Docker workspace may still contain patches from an older
    # experiment. Restore tracked files from their own reversible diff before
    # applying the patches selected by this build.
    for repo in /workspace/zmk /workspace/zmk-driver-paw3222; do
      if ! git -C "$repo" diff --quiet; then
        git -C "$repo" diff --binary | git -C "$repo" apply --reverse
      fi
    done

    # Skipped (2026-08-13): comparison with the PMW3610-based build showed
    # it uses stock ZMK mouse-report send (K_MSEC(100) blocking, discard
    # oldest only after a real timeout) with no custom patch at all. Our
    # nonblocking+fold-into-newest behavior merges deltas into a single
    # bigger jump whenever the queue is briefly full, which reads as a
    # visible kaku snap. Now that sensor pacing (16ms) is reasonably
    # close to the real BLE interval, blocking should rarely engage for
    # long, so this reverts to stock to test whether it is smoother.
    # zmk_mouse_patch=/workspace/config/patches/zmk-ble-mouse-nonblocking.patch
    # if git -C /workspace/zmk apply --recount --check "$zmk_mouse_patch"; then
    #   git -C /workspace/zmk apply --recount "$zmk_mouse_patch"
    # elif ! git -C /workspace/zmk apply --recount --reverse --check "$zmk_mouse_patch"; then
    #   echo "ZMK nonblocking mouse patch does not apply cleanly" >&2
    #   exit 1
    # fi

    paw_patch=/workspace/config/patches/zmk-driver-paw3222-late-init.patch
    if git -C /workspace/zmk-driver-paw3222 apply --check "$paw_patch"; then
      git -C /workspace/zmk-driver-paw3222 apply "$paw_patch"
    elif ! git -C /workspace/zmk-driver-paw3222 apply --reverse --check "$paw_patch"; then
      echo "PAW3222 late-init patch does not apply cleanly" >&2
      exit 1
    fi
    motion_patch=/workspace/config/patches/zmk-driver-paw3222-motion-stability.patch
    if git -C /workspace/zmk-driver-paw3222 apply --recount --check "$motion_patch"; then
      git -C /workspace/zmk-driver-paw3222 apply --recount "$motion_patch"
    elif ! git -C /workspace/zmk-driver-paw3222 apply --recount --reverse --check "$motion_patch"; then
      echo "PAW3222 motion-stability patch does not apply cleanly" >&2
      exit 1
    fi
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
