if(CONFIG_G0N_HUD)
  set(g0n_commit "$ENV{GITHUB_SHA}")
  if(g0n_commit STREQUAL "")
    execute_process(
      COMMAND git -c safe.directory=* rev-parse --short=7 HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      OUTPUT_VARIABLE g0n_commit
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
  string(SUBSTRING "${g0n_commit}" 0 7 g0n_commit)
  if(g0n_commit STREQUAL "")
    set(g0n_commit "local")
  endif()
  zephyr_library()
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_compile_definitions(G0N_HUD_COMMIT="${g0n_commit}")
  zephyr_library_sources(src/hud_fw.c src/hud_anim_frames.c src/hud_zmk.c)
endif()
