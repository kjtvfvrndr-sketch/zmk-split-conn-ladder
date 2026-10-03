# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_SPLIT_CONN_LADDER OR CONFIG_ZMK_USB_RESEND_KEYS_AFTER_WAKE OR
   CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_USB_POWER_REPORT OR
   CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP OR
   CONFIG_ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP)
  zephyr_library_named(zmk_split_conn_ladder)

  # ZMK keeps its own headers (zmk/*.h, drivers/behavior.h) private to the
  # `app` target, so a module library has to ask for them. Without this line
  # the module only compiles when some other module in the same build happens
  # to export them globally.
  zephyr_library_include_directories(
    include
    ${APPLICATION_SOURCE_DIR}/include
  )

  # ... and the other way round: <zmk_split_conn_ladder/ladder.h> is the public
  # API (the mode getter/setter), so anything else in the build -- the keymap's
  # own C, another module -- can reach it. The line above is PRIVATE to this
  # library and would not do that.
  zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

  # central half: the ladder, and what the other half says about its USB
  # power -- ACTIVE's latency and the USB power steps depend on it
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_CONN_LADDER
    src/split_conn_ladder.c
    src/peripheral_usb_power_report_client.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_CONN_LADDER_LOW_POWER_PERIPHERAL_SEARCH
    src/low_power_search.c)
  # both halves
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_CONN_LADDER_EARLY_SLEEP
    src/early_sleep.c)
  # peripheral half: its USB power, for the ladder
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_USB_POWER_REPORT
    src/peripheral_usb_power_report.c)
  # peripheral half: close the link before sleeping, so the central knows at once
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_CLOSE_LINK_BEFORE_SLEEP
    src/peripheral_close_link_before_sleep.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SPLIT_CONN_LADDER
    src/behaviors/behavior_split_conn_ladder.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_USB_RESEND_KEYS_AFTER_WAKE
    src/usb_resend_keys_after_wake.c)
endif()
