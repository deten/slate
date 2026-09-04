# Example plugins.
#
# These build as part of the normal build rather than being documentation-only,
# so the plugin ABI cannot quietly rot: a breaking change to the header breaks
# the build here first.

add_subdirectory(plugins/boltcircle)
