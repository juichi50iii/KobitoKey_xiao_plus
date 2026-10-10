zephyr_library()
zephyr_library_include_directories(include)

zephyr_library_sources_ifdef(CONFIG_KOBITOKEY_HAPTIC src/kobitokey_haptic.c)
zephyr_library_sources_ifdef(CONFIG_KOBITOKEY_HAPTIC src/input_processor_haptic_right.c)
zephyr_library_sources_ifdef(CONFIG_KOBITOKEY_HAPTIC_HOLD_TICK src/kobitokey_hold_tick.c)

# 左右どちらにも積む。長押しを判断するのは central だけだが、鳴らす先が
# peripheral のときは向こうの behavior を名前で呼ぶので、向こうにも
# 実体が要る。
zephyr_library_sources_ifdef(CONFIG_KOBITOKEY_HAPTIC src/behavior_haptic_tick.c)
zephyr_library_sources_ifdef(CONFIG_KOBITOKEY_VBUS_SENSE src/kobitokey_vbus.c)
zephyr_library_sources_ifdef(CONFIG_KOBITOKEY_VBUS_SENSE src/kobitokey_rgbled_hook.c)
zephyr_library_sources_ifdef(CONFIG_KOBITOKEY_FOLD_SENSE src/kobitokey_fold.c)
zephyr_library_sources_ifdef(CONFIG_KOBITOKEY_HEALTH src/kobitokey_health.c)
# ジェスチャーパッドだけは、モジュールのライブラリではなく app ターゲットに積む。
#
# ZMK のイベントは、どれかのリスナーがエラー(負の値)を返すとそこで打ち切られ、
# 後ろのリスナーに届かない。リスナーの順番はリンクされた順で、ライブラリは
# app の後ろに並ぶ。左手のジェスチャーが送ってくる位置は、キーマップに
# キー指定が無いのでキーマップのリスナーがエラーを返し、ライブラリ側の
# リスナー(gesturepad_left)まで届かなかった。app に積むと、ZMK 本体の
# ソースより前に並ぶ(zmk-rgbled-widget も同じやり方で本体より前にいる)。
if(CONFIG_KOBITOKEY_GESTUREPAD)
  target_sources(app PRIVATE src/kobitokey_gesturepad.c)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
endif()

zephyr_include_directories(${APPLICATION_SOURCE_DIR}/include)
