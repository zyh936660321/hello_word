# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "F:/2025.3_exercitation/Espressif/frameworks/esp-idf-v5.3.1/components/bootloader/subproject"
  "F:/2025.3_exercitation/Espressif/WorkSpace_VSCode/station/station/build/bootloader"
  "F:/2025.3_exercitation/Espressif/WorkSpace_VSCode/station/station/build/bootloader-prefix"
  "F:/2025.3_exercitation/Espressif/WorkSpace_VSCode/station/station/build/bootloader-prefix/tmp"
  "F:/2025.3_exercitation/Espressif/WorkSpace_VSCode/station/station/build/bootloader-prefix/src/bootloader-stamp"
  "F:/2025.3_exercitation/Espressif/WorkSpace_VSCode/station/station/build/bootloader-prefix/src"
  "F:/2025.3_exercitation/Espressif/WorkSpace_VSCode/station/station/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "F:/2025.3_exercitation/Espressif/WorkSpace_VSCode/station/station/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "F:/2025.3_exercitation/Espressif/WorkSpace_VSCode/station/station/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
