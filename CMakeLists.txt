# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_SPLIT_CONN_LADDER OR CONFIG_ZMK_USB_RESUME_RESEND OR
   CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER)
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

  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_CONN_LADDER
    src/split_conn_ladder.c)
  # central half: what the other half says about its USB power
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_CONN_LADDER_USB_STEPS
    src/peripheral_power_client.c)
  # peripheral half: saying it
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_CONN_LADDER_PERIPHERAL_POWER
    src/peripheral_power.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_BEHAVIOR_SPLIT_CONN_LADDER
    src/behaviors/behavior_split_conn_ladder.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_USB_RESUME_RESEND
    src/usb_resume_resend.c)
endif()
