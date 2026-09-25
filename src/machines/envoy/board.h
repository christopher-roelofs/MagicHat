#ifndef MRC_ENVOY_BOARD_H
#define MRC_ENVOY_BOARD_H
/* Envoy ROM vectors execute at 02400216; its reset code programs CS0 base
 * 004000F9. MC68349UM 4.3.4.2 allows noncontiguous address masks/aliases.
 * The 4 MiB ROM is visible at both addresses. Other CS0 mask combinations
 * and function-code-qualified windows are not yet fully modeled. */
#define ENVOY_ROM_BASE 0x02400000u
#define ENVOY_ROM_ALIAS 0x00400000u
#endif
