if(NOT CPACK_GENERATOR STREQUAL "DEB")
  return()
endif()

find_program(SERIALSPARK_SHLIBDEPS dpkg-shlibdeps REQUIRED)
find_program(SERIALSPARK_PYTHON python3 REQUIRED)
foreach(package IN LISTS CPACK_PACKAGE_FILES)
  execute_process(
    COMMAND "${SERIALSPARK_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/../scripts/check_deb.py" "${package}"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "SerialSpark Debian 包验收失败：${package}")
  endif()
endforeach()
