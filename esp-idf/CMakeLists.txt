file(GLOB VIEWER_SRCS "src/*.cpp")
# Vendored MD4C (github.com/mity/md4c) — single-purpose Markdown→HTML, no deps.
# Kept verbatim under md4c/src/ and compiled with warnings off (-w) so the
# repo's -Werror doesn't trip on upstream code we don't maintain, exactly as
# spangap-lcd vendors libvterm.
file(GLOB MD4C_SRCS "md4c/src/*.c")

include(${CMAKE_CURRENT_LIST_DIR}/spangap_requires.cmake)
# Conditional sources: esp-idf/conditional/<name>/ is compiled only when <name>
# is staged. The LCD viewer lives in conditional/spangap-lcd/ and the http(s)
# fetcher in conditional/spangap-net/ — both register/are-gated via the when:
# hooks / #if CONFIG_SPANGAP_NET, no other #if needed.
#
# esp_http_client is listed unconditionally (small; an IDF component): the
# fetcher in conditional/spangap-net/ uses it + esp_crt_bundle (pulled in
# transitively via esp-tls), mirroring acme/duckdns.
idf_component_register(
    SRCS
        ${VIEWER_SRCS}
        ${MD4C_SRCS}
        ${SPANGAP_CONDITIONAL_SRCS}
    INCLUDE_DIRS "include" ${SPANGAP_CONDITIONAL_INCLUDE_DIRS}
    PRIV_INCLUDE_DIRS "md4c/src"
    REQUIRES
        ${SPANGAP_REQUIRES}
        esp_http_client
)

set_source_files_properties(${MD4C_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
