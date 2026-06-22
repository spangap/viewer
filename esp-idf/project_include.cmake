# viewer — contribute read-only /fixed files (the webroot help docs WELCOME.md /
# ABOUT.md) to the factory image. spangap-core's spangap_create_factory_image()
# merges every dir appended to SPANGAP_EXTRA_DATA_DIRS after the core defaults
# and before the consumer's own data/ — so the buildable can still override.
#
# data/ mirrors the /fixed layout: data/webroot/WELCOME.md -> /fixed/webroot/
# WELCOME.md, which the "/" web mapping serves at /WELCOME.md. CMAKE_CURRENT_LIST_DIR
# is this staged component dir; its data/ is a symlink to the straddle's esp-idf/data/.
set_property(GLOBAL APPEND PROPERTY SPANGAP_EXTRA_DATA_DIRS
            "${CMAKE_CURRENT_LIST_DIR}/data")
