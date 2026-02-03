#
# Copyright 2024 Free Software Foundation, Inc.
#
# This file is part of VOLK
#
# SPDX-License-Identifier: LGPL-3.0-or-later
#

add_executable(volk_gen volk_gen.cpp)
target_compile_features(volk_gen PRIVATE cxx_std_17)

# volk_gen needs to be built before the main library since it generates source files
# It will be available at configure time via execute_process
