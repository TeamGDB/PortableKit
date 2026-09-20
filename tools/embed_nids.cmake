# Turns NID tables into initializers for the table main.cpp compiles in:
#   cmake -DINPUT=<a.csv>|<b.csv> -DOUTPUT=<nid_table.inc> -P embed_nids.cmake
# INPUT is one path or several separated by "|": the framework's own
# configs/nids.csv, then a profile's extra rows. Each `library,nid,name` line
# becomes {"library", 0xNIDu, "name"}, and the file is only rewritten when its
# content changes.
string(REPLACE "|" ";" inputs "${INPUT}")
set(content "// Generated from the NID tables by tools/embed_nids.cmake.\n")
foreach(input IN LISTS inputs)
    file(STRINGS "${input}" lines)
    set(line_number 0)
    foreach(line IN LISTS lines)
        math(EXPR line_number "${line_number} + 1")
        string(STRIP "${line}" line)
        if(line STREQUAL "" OR line MATCHES "^#")
            continue()
        endif()
        if(NOT line MATCHES "^([A-Za-z0-9_]+),(0[xX])?([0-9A-Fa-f]+),([A-Za-z0-9_]+)$")
            message(FATAL_ERROR "${input}:${line_number}: expected library,nid,name")
        endif()
        string(APPEND content "{\"${CMAKE_MATCH_1}\", 0x${CMAKE_MATCH_3}u, \"${CMAKE_MATCH_4}\"},\n")
    endforeach()
endforeach()
set(previous "")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" previous)
endif()
if(NOT previous STREQUAL content)
    file(WRITE "${OUTPUT}" "${content}")
endif()
