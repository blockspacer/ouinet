function(_parse_openssl_version version_string output_var)
    if (${version_string} MATCHES "^([0-9]+)(\\.([0-9]+)(\\.([0-9]+)([a-zA-Z])?)?)?\$")
        set(major ${CMAKE_MATCH_1})
        if ("${CMAKE_MATCH_2}" STREQUAL "")
            set(minor 0)
            set(patch 0)
            set(tweak 0)
        else()
            set(minor ${CMAKE_MATCH_3})
            if ("${CMAKE_MATCH_4}" STREQUAL "")
                set(patch 0)
                set(tweak 0)
            else()
                set(patch ${CMAKE_MATCH_5})
                if ("${CMAKE_MATCH_6}" STREQUAL "")
                    set(tweak 0)
                else()
                    string(TOLOWER "${CMAKE_MATCH_6}" tweak_lower)
                    string(FIND "abcdefghijklmnopqrstuvwxyz" ${tweak_lower} tweak)
                endif()
            endif()
        endif()
        string(CONCAT normalized_version
            ${major} "."
            ${minor} "."
            ${patch} "."
            ${tweak}
        )
    else()
        set(normalized_version "")
    endif()
    set(${output_var} ${normalized_version} PARENT_SCOPE)
endfunction(_parse_openssl_version)

_parse_openssl_version("${BUILT_OPENSSL_VERSION}" built_version)
_parse_openssl_version("${PACKAGE_FIND_VERSION}" requested_version)

set(PACKAGE_VERSION "${BUILT_OPENSSL_VERSION}")
set(PACKAGE_VERSION_UNSUITABLE "false")
if ("${PACKAGE_FIND_VERSION}" STREQUAL "")
    set(PACKAGE_VERSION_COMPATIBLE "true")
    set(PACKAGE_VERSION_EXACT "false")
elseif ("${requested_version}" STREQUAL "")
    set(PACKAGE_VERSION_COMPATIBLE "false")
    set(PACKAGE_VERSION_EXACT "false")
elseif ("${built_version}" VERSION_EQUAL "${requested_version}")
    set(PACKAGE_VERSION_COMPATIBLE "true")
    set(PACKAGE_VERSION_EXACT "true")
elseif ("${built_version}" VERSION_GREATER "${requested_version}")
    set(PACKAGE_VERSION_COMPATIBLE "true")
    set(PACKAGE_VERSION_EXACT "false")
else()
    set(PACKAGE_VERSION_COMPATIBLE "false")
    set(PACKAGE_VERSION_EXACT "false")
endif()
