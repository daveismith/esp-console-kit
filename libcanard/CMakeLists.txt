# OpenCyphal/libcanard, the Cyphal/CAN transport, from the submodule at third_party/libcanard, as an
# ESP-IDF component (MIT; the licence is third_party/libcanard/LICENSE). Used unmodified: this file
# is the packaging. The kit's cyphal component is its one user.
#
# Pinned to the v4.0.0 tag: the current major for Cyphal/CAN v1.0 that still sends anonymous
# transfers. v5.0.0 (2026-10-21) is a rework aimed at the Cy stack that can no longer transmit
# anonymous messages, which the plug-and-play allocatee (uavcan.pnp.NodeIDAllocationData.1.0, an
# anonymous request) needs, and replaces that allocation with a passive scheme of its own.
#
# Upstream's own ESP packaging (esp_metadata/) expects a flattened copy, so it is not used.
# canard.c includes the AVL tree header it vendors in lib/cavl2.
set(upstream "${CMAKE_CURRENT_LIST_DIR}/../third_party/libcanard")

idf_component_register(SRCS "${upstream}/libcanard/canard.c"
                       INCLUDE_DIRS "${upstream}/libcanard"
                       PRIV_INCLUDE_DIRS "${upstream}/lib/cavl2")
