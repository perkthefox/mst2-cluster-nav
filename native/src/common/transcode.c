/*
 * mst2-cluster-nav - phone navigation on the MQB instrument cluster.
 * Copyright (c) 2026 Anton Perk. CC BY-NC-SA 4.0, see LICENSE and NOTICE.
 *
 * Geometry of the cluster transcode (mcn_transcode.h): what to ask the phone for and which
 * rectangle of its decoded frame becomes the 1010x376 cluster picture. Arithmetic only, no VPU;
 * host-tested in native/test/route_check.py.
 */
#include "mcn_transcode.h"

/* Round v up to a multiple of a (a a power of two). */
static int align_up(int v, int a)
{
    return (v + a - 1) & ~(a - 1);
}

int mcn_geom(enum mcn_tc_mode mode, int factor_milli, struct mcn_geom *g)
{
    int w, h;

    g->mode = mode;
    g->margin_w = g->margin_h = 0;
    switch (mode) {
    case MCN_TC_AA:
        /*
         * The phone encodes only fixed sizes; 1280x720 is the smallest that holds the cluster's
         * 1024x384 coded frame. Margins centre that content at a VPU-aligned origin (16 across,
         * 2 down): (1280-1024)/2 = 128, (720-384)/2 = 168. The encoder crops {14,8} to 1010x376.
         */
        g->req_w = 1280;
        g->req_h = 720;
        g->margin_w = g->req_w - MCN_TC_CODED_W; /* 256 */
        g->margin_h = g->req_h - MCN_TC_CODED_H; /* 336 */
        g->src_x = g->margin_w / 2;              /* 128 */
        g->src_y = g->margin_h / 2;              /* 168 */
        g->src_w = MCN_TC_CODED_W;
        g->src_h = MCN_TC_CODED_H;
        g->scale = 0;
        return 0;
    case MCN_TC_CARPLAY_ZOOM:
        /*
         * A screen factor_milli/1000 larger than 1010x376 makes the phone draw a wider view
         * (confirmed in the CarPlay Simulator: Yandex shows more map). Clamp to the i.MX6 VPU's
         * ~1920 width; keep the width a multiple of 16 and the height even. The whole frame is
         * scaled down into the 1010x376 visible area of the 1024x384 coded buffer.
         */
        if (factor_milli < 1000) {
            factor_milli = 1000;
        }
        if (factor_milli > 1900) {
            factor_milli = 1900;
        }
        w = align_up(MCN_TC_VIS_W * factor_milli / 1000, 16);
        h = align_up(MCN_TC_VIS_H * factor_milli / 1000, 2);
        if (w > 1920) {
            w = 1920;
        }
        if (h > 1088) {
            h = 1088;
        }
        g->req_w = w;
        g->req_h = h;
        g->src_x = g->src_y = 0;
        g->src_w = w;
        g->src_h = h;
        g->scale = 1;
        return 0;
    default:
        return -1;
    }
}
