# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "C:/Users/Mikiilis/Documents/code/DelugeFirmware/tests/unit/build_tests/_deps/cpputest-src"
  "C:/Users/Mikiilis/Documents/code/DelugeFirmware/tests/unit/build_tests/_deps/cpputest-build"
  "C:/Users/Mikiilis/Documents/code/DelugeFirmware/tests/unit/build_tests/_deps/cpputest-subbuild/cpputest-populate-prefix"
  "C:/Users/Mikiilis/Documents/code/DelugeFirmware/tests/unit/build_tests/_deps/cpputest-subbuild/cpputest-populate-prefix/tmp"
  "C:/Users/Mikiilis/Documents/code/DelugeFirmware/tests/unit/build_tests/_deps/cpputest-subbuild/cpputest-populate-prefix/src/cpputest-populate-stamp"
  "C:/Users/Mikiilis/Documents/code/DelugeFirmware/tests/unit/build_tests/_deps/cpputest-subbuild/cpputest-populate-prefix/src"
  "C:/Users/Mikiilis/Documents/code/DelugeFirmware/tests/unit/build_tests/_deps/cpputest-subbuild/cpputest-populate-prefix/src/cpputest-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/Users/Mikiilis/Documents/code/DelugeFirmware/tests/unit/build_tests/_deps/cpputest-subbuild/cpputest-populate-prefix/src/cpputest-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/Users/Mikiilis/Documents/code/DelugeFirmware/tests/unit/build_tests/_deps/cpputest-subbuild/cpputest-populate-prefix/src/cpputest-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
