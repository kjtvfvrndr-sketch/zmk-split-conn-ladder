# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_SPLIT_CONN_LADDER)
  zephyr_library_named(zmk_split_conn_ladder)
  zephyr_library_sources(src/split_conn_ladder.c)
endif()
