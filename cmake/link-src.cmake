# SPDX-License-Identifier: BSD-3-Clause
#
# Points Chromium's //crnet at this tree's src/ directory.
#
# Runs at configure time (include()) and before every build
# (cmake -P), with CR_SRC, CR_OUT and CRNET_SRC set. When CR_GN is set
# too, a re-pointed link also regenerates the GN build graph here;
# otherwise the caller must run `gn gen` when CRNET_SRC_RELINKED is set.
#
# A checkout copied from another crnet tree, or a Chromium checkout
# shared between trees, can hold a link to another tree's src/, and
# building through it would silently compile that tree's sources.
# Re-pointing the link is not enough on its own: ninja compares
# timestamps, not paths, so objects compiled from the other tree can
# look newer than this tree's sources and be kept. Drop them, and
# regenerate the build graph in case the two BUILD.gn files differ.

set(CRNET_SRC_RELINKED FALSE)
set(_crnet_link "${CR_SRC}/crnet")
get_filename_component(_crnet_want "${CRNET_SRC}" REALPATH)
get_filename_component(_crnet_root "${CR_SRC}" REALPATH)

if(IS_SYMLINK "${_crnet_link}")
  get_filename_component(_crnet_have "${_crnet_link}" REALPATH)
  if(NOT _crnet_have STREQUAL _crnet_want)
    message(STATUS
      "crnet: //crnet pointed at ${_crnet_have}, re-pointing it at "
      "${_crnet_want}")
    file(REMOVE "${_crnet_link}")
    set(CRNET_SRC_RELINKED TRUE)
  endif()
elseif(EXISTS "${_crnet_link}")
  message(FATAL_ERROR
    "${_crnet_link} exists and is not a symlink; move it out of the way.")
else()
  set(CRNET_SRC_RELINKED TRUE)
endif()

if(CRNET_SRC_RELINKED)
  # Relative, so that copying the whole tree keeps a valid link.
  file(RELATIVE_PATH _crnet_rel "${_crnet_root}" "${_crnet_want}")
  file(CREATE_LINK "${_crnet_rel}" "${_crnet_link}" SYMBOLIC)
  # Nothing tells which tree the existing objects were compiled from.
  file(REMOVE_RECURSE "${CR_OUT}/obj/crnet/crnet")
  if(CR_GN AND EXISTS "${CR_OUT}/build.ninja")
    execute_process(COMMAND "${CR_GN}" gen "${CR_OUT}"
                    WORKING_DIRECTORY "${CR_SRC}"
                    RESULT_VARIABLE _crnet_gn_rc)
    if(NOT _crnet_gn_rc EQUAL 0)
      message(FATAL_ERROR "gn gen failed")
    endif()
  endif()
endif()
