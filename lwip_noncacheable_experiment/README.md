# Network buffers outside the cache - the files

Section 12 of the top-level README has the reasoning and the numbers. Each
variant is a replacement for two BSP files plus the CPU1 `main.c`:

```
<bsp>/ps7_cortexa9_1/libsrc/lwip211_v1_0/src/contrib/ports/xilinx/netif/xemacpsif_dma.c
<bsp>/ps7_cortexa9_1/libsrc/lwip211_v1_0/src/contrib/ports/xilinx/include/lwipopts.h
```

The lwIP build copies that `lwipopts.h` into `include/`, so edit this one, not
the copy. Then `make` in the BSP directory and relink `app_cpu1`, which the
managed makefile will not do on its own after a library change.

| variant | driver | lwipopts.h | main.c | result |
|---|---|---|---|---|
| whole lwIP data area non-cacheable | `xemacpsif_dma_whole_lwip.c` | stock | `cpu1_main.c` | works, slowest: lwIP bookkeeping uncached too |
| received frames only | `xemacpsif_dma_rx_payload_only.c` | stock | stock | works; invalidate gone, flush kept |
| no maintenance at all | `xemacpsif_dma_no_cache_maintenance.c` | `lwipopts_heap_noncacheable.h` | `cpu1_main_heap_noncacheable.c` | works, 551 us round trip |
| **no maintenance, padded** | `xemacpsif_dma_no_cache_maintenance_padded.c` | `lwipopts_heap_noncacheable_padded.h` | `cpu1_main_heap_noncacheable.c` | works, 506 us; **what the board runs now** |

`cpu1/reference/main.c` is the last of these. With a stock BSP the extra
`Xil_SetTlbAttributes` line in it only marks an unused section non-cacheable.

The stock driver to go back to is in the SDK install:

```
<SDK>/data/embeddedsw/ThirdParty/sw_services/lwip211_v1_0/src/contrib/ports/xilinx/netif/xemacpsif_dma.c
```
