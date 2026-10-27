/*
 * mcn_vpu_abi.h - ABI of the stock QNX libmxcvpu.so.1 (MST2 0480, i.MX6Q CODA960)
 * as used by the stock MMF plug-ins imx6_video_decoder.so / imx6_video_encoder.so.
 *
 * Binaries (the unit's CPU file system, MST2 0480):
 *   usr/lib/libmxcvpu.so.1            sha256 781e10c2...40a7e6cb  ("lib")
 *   lib/dll/mmedia/imx6_video_decoder.so 69084cfa...0e0b43        ("dec plug-in")
 *   lib/dll/mmedia/imx6_video_encoder.so aa0ec4d7...def8156b      ("enc plug-in")
 *   usr/sbin/mxcvpu                    73206ff0...947ba28d        (resource manager)
 *
 * Every offset below was read in the code; "lib 0xNNNN" = instruction address in
 * libmxcvpu.so.1, "dec 0xNNNN"/"enc 0xNNNN" = address in the plug-in.  Evidence IDs
 * (REA, rizin pD dumps; listings in the mQbEMU research notes):
 *   EV_LIB_IO   ev_a07c297f07bf8485a41ab7f6525f8d1d0c5ac903ef62d6454ccc2027f2c992e0 (lib 0x4794-0x8394)
 *   EV_LIB_ENC  ev_f9ca0e1f0d6572726debefa1be9ce7d8e934d89f1bccf8c56d6de074ffd1bd56 (lib 0x9560-0xde60)
 *   EV_LIB_D1   ev_146049472f551aed47a573b49be5976dd690edddcdb7a5f5d28e4bd9c54c6ea1 (lib 0xeda0-0x10da0)
 *   EV_LIB_D2   ev_eb4ab97df7c1cadf47b1b31a5b2e1c3f964dac37641827f4f5e82009651c0596 (lib 0x10da0-0x13430)
 *   EV_LIB_D3   ev_4e172bdf7f884951857691cc9d12a6e2d4d4b0564c0fcb8d33c7a2567e9a325a (lib 0x13430-0x14430)
 *   EV_LIB_D4   ev_030efcad1ed9c56f369f07765d5efc874207166ebfbec086d9f0ff5ba7155cd9 (lib 0x14430-0x16d30)
 *   EV_LIB_UTIL ev_1dcc31f93f0e5f35a62e22e8df6d7dd4360e02786ed09ee2882f5d6dd7646c78 (lib 0x181b0.., 0x18a30-0x1a560, 0x1b080..)
 *   EV_LIB_PLT  ev_c3ed8ac08ce4ba644a4569e18bcbab56ed0c7ce07108899c7561b25f863bc94a (PLT -> symbol map)
 *   EV_LIB_IMM  ev_80e4a33ca06b590d7fdf0705f4b7bcfa1764e15e3afb52f4d1001af9d35614d9 (0x114 / 0x80000 immediates)
 *   EV_MUTEX    ev_7e041df9cbe8c61fc6f68dd0bf9de4ca0e02738a2a155e59e20d8293534c298b,
 *               ev_6c2fdcfaa5c6e7290a90a809292a856e08c0146003d83626d0bebf216b2d4bd0,
 *               ev_f67352f627516c764979474ef5113fdbdd4e48738aadc900d5e450bb49480e95 (VPU_MUTEX_TIMEOUT)
 *   EV_DEC_A    ev_9771345d0b62285098b40260479e818cc3a7c40ff479e14479785b80d686831e (dec 0x1f28-0x2f28)
 *   EV_DEC_B    ev_08984eef399a4ff457c8f0ace526a7a563ce8f210e89a2d20e7cf429a941f80e (dec 0x2f28-0x3f28)
 *   EV_DEC_C    ev_fac036d5b4534298775a5082777d88d96aa34c6e51c937d161c14346a30d1fc9 (dec 0x3f28-0x4f28)
 *   EV_DEC_IMM  ev_a95180d08d8ec737f436af2ef94ddb872938d7dcdea50c1c68c8cc8cab75749a (0x80000 sizes)
 *   EV_ENC      ev_94507f55dfafcd390a7c1f2caaadc93ecca63d05949a59af5a9eb496f1ea573c (enc 0x1800-0x2b00)
 *   EV_MXCVPU   ev_1e54553a25759131fbbcd4138614ba9ef05f59745b85a6c3f1654d178688f581 (io_close_ocb, peer alloc/free)
 *   EV_DEC_START ev_5fdb745c07e3216daab3a03863676e2abddc30c40f9411f12a4f9d5d19ac8b76 (lib 0x13430)
 *   EV_ENC_SRC  ev_2d1dce90181e38fcdb3bbfa5cba708c819e18a464bdb1485fd666104bd2bbaed,
 *               ev_7a0724cf597c94aad7769f8c00f610df465c4ae37e5726c2a8a9066ff9be93ea (enc 0x1d80..)
 *   EV_MMAN     ev_54bc1dbe69c33fe3832707e01edd2c9c2f15d7df9dcf7f4e729ac32753a23a09 (showmem MAP_*)
 *   EV_FW_PATH  ev_3c4d97fe3b99ffd33715ec047445ab085895011d0c344d7f654a1d050892d31a (VPU_FW_PATH)
 *
 * Labels: [OBS] read in code, [INF] inference, [UNK] not established,
 *         [EXT] name taken from public NXP imx-lib (hypothesis for the NAME only).
 * Not tested on the unit or the bench.
 */
#ifndef MCN_VPU_ABI_H
#define MCN_VPU_ABI_H

#include <stdint.h>

typedef uint32_t vpu_phys_t; /* 32-bit physical address */
typedef void *vpu_handle_t;  /* DecHandle/EncHandle = CodecInst* in shared memory (see 5.) */

/* ------------------------------------------------------------------------------------ */
/* 0. Return codes [OBS numbers; names EXT].  Port numbering == newer imx-lib, where       */
/*    ROTATOR_STRIDE_NOT_SET is -11 out of order:                                          */
/*    -6 lib 0xfb04 (DecClose while frame pending; dec plug-in tests it: dec 0x1fb4 cmn r0,6) */
/*    -10 lib 0x1041c (bitstream empty at GetInitialInfo), 0x115e8, 0x13560                 */
/*    -12 lib 0x10304 / 0x115d0, -13 lib 0xeeec, -17 lib 0xee80, -18 lib 0x10b0c          */
/*    (the plug-in's retcode_str table dec 0x1ce8 uses the old order and mislabels -6..-11) */
enum {
    VPU_RET_SUCCESS = 0,
    VPU_RET_FAILURE = -1,
    VPU_RET_INVALID_HANDLE = -2,     /* also: GetOutputInfo on a handle that is not pending */
    VPU_RET_INVALID_PARAM = -3,
    VPU_RET_INVALID_COMMAND = -4,
    VPU_RET_FRAME_NOT_COMPLETE = -6, /* Dec/EncClose while this handle has a frame pending */
    VPU_RET_INVALID_FRAME_BUFFER = -7,
    VPU_RET_INSUFFICIENT_FRAME_BUFFERS = -8,
    VPU_RET_INVALID_STRIDE = -9,
    VPU_RET_WRONG_CALL_SEQUENCE = -10,
    VPU_RET_CALLED_BEFORE = -12,
    VPU_RET_NOT_INITIALIZED = -13,
    VPU_RET_FAILURE_TIMEOUT = -17, /* cross-process VPU mutex not obtained in VPU_MUTEX_TIMEOUT s */
    VPU_RET_MEMORY_ACCESS_VIOLATION = -18,
};

/* bitstreamFormat (CodStd) [OBS lib 0xf0c4 switch, dec 0x256c/0x2910: 'AVC1'/'H264' -> 2] */
enum { VPU_STD_MPEG4 = 0, VPU_STD_AVC = 2, VPU_STD_MJPG = 7 };

/* ------------------------------------------------------------------------------------ */
/* 1. vpu_mem_desc  (IOGetPhyMem / IOFreePhyMem / IOGetVirtMem / IOFreeVirtMem)            */
/*    [OBS] lib _IOGetPhyMem 0x56dc: ioctl(fd, 0xC0105600 VPU_IOC_PHYMEM_ALLOC, desc);       */
/*      on error +4 and +8 are zeroed, returns -1.  mxcvpu io_devctl_alloc_phymem 0x101b84 + */
/*      vpu_alloc_peer_phymem 0x10320c: mmap64_peer(our pid, size, PROT 0xB00 =             */
/*      READ|WRITE|NOCACHE, MAP 0x94000 = ANON|PHYS|NOINIT) + mem_offset64_peer ->           */
/*      (QNX MAP_NOINIT 0x4000, MAP_PHYS 0x10000, MAP_ANON 0x80000: showmem's table, EV_MMAN) */
/*      reply {size, phys, phys, virt-in-OUR-process}.  So the block is physically           */
/*      contiguous, UNCACHED and already mapped into the caller; no cache maintenance.       */
/*    [OBS] IOGetVirtMem 0x5ebc only returns desc->virt_uaddr (no mmap).                     */
/*    [OBS] IOFreeVirtMem 0x6014: munmap(virt_uaddr, size), virt_uaddr = 0.                  */
/*    [OBS] IOFreePhyMem 0x5c94/_IOFreePhyMem 0x5acc: if phy_addr != 0 ioctl(0x80105601      */
/*      VPU_IOC_PHYMEM_FREE) -> mxcvpu munmap_peer; then memset(desc, 0, 16).                */
/*    [OBS] mxcvpu io_close_ocb 0x101aa8 frees every block allocated through that fd when    */
/*      the fd is closed (vpu_UnInit of the last user, or process death).                    */
/*    Alignment: [INF] page (4 KiB) from mmap64 MAP_PHYS.                                     */
typedef struct {
    int32_t size;        /* +0x0 in: bytes */
    vpu_phys_t phy_addr; /* +0x4 out */
    uint32_t cpu_addr;   /* +0x8 out: = phy_addr on this port */
    uint32_t virt_uaddr; /* +0xC out: virtual address in the calling process (uncached) */
} vpu_mem_desc;          /* 0x10 */

/* ------------------------------------------------------------------------------------ */
/* 2. FrameBuffer  [OBS] array stride 0x1C (lib 0x11914 i*28, enc 0x1e08 calloc(n,0x1c))   */
/*    Dec RegisterFrameBuffer reads +0xC,+0x10,+0x14 (Cr only if !chromaInterleave),       */
/*    +0x18 for AVC (lib 0x11eec-0x12150).  Enc RegisterFrameBuffer reads +0xC..+0x14       */
/*    (lib 0xb304-0xb4cc).  EncStartOneFrame reads +0x0,+0x4,+0x8,+0xC..+0x14 of the        */
/*    SOURCE frame (lib 0xcd1c-0xce18).                                                     */
typedef struct {
    int32_t strideY;     /* +0x00 luma stride, bytes */
    int32_t strideC;     /* +0x04 chroma stride (only used by Enc for encTop/LeftOffset)    */
    int32_t myIndex;     /* +0x08 index; Enc writes it to CMD_ENC_PIC_SRC_INDEX (0x180)     */
    vpu_phys_t bufY;     /* +0x0C */
    vpu_phys_t bufCb;    /* +0x10 NV12: CbCr plane */
    vpu_phys_t bufCr;    /* +0x14 NV12: ignored by Dec (lib 0x11e84), stock still fills it */
    vpu_phys_t bufMvCol; /* +0x18 AVC decoder: co-located MV buffer (required); Enc: 0     */
} FrameBuffer;           /* 0x1C */

/* Stock decoder frame layout (dec imxmmf_init 0x2f34-0x3178) [OBS]:
 *   stride   = align16(picWidth)                       (ctx+0xa4)
 *   alignedH = align16(picHeight); align32 if interlace && std in {AVC,VC1,MPEG2,VP8}
 *   lumaSz = stride*alignedH, chromaSz = lumaSz/4
 *   frame bytes = lumaSz + 2*chromaSz + chromaSz(MvCol)  = stride*alignedH*7/4
 *   bufY = base, bufCb = base+lumaSz, bufCr = bufCb+chromaSz, bufMvCol = bufCr+chromaSz
 *     (for NV12 this puts MvCol right after the full CbCr plane)
 *   count = minFrameBufferCount + 2 (+4 if interlace), asserted <= 20 (dec 0x2fa0/0x2fa8)
 *   strideY = stride, strideC = stride/2, myIndex = i
 *   The stock buffers come from the MMF output pool (screen buffers), NOT IOGetPhyMem;
 *   the lib only "checks" addresses with IOPhyMemCheck, which always returns 1
 *   (lib 0x5d68), so any physically contiguous memory is accepted.
 */

/* ------------------------------------------------------------------------------------ */
/* 3. Decoder                                                                              */

/* DecOpenParam, 0x58 bytes, copied whole into the instance (memcpy 0x58, lib 0xf080).
 * Field use: lib vpu_DecOpen 0xeda0-0xf9e0, CheckDecOpenParam 0x196d4.  Stock values:
 * dec imxmmf_open 0x27c8-0x2858 (struct at sp+0x2c, memset 0x58 at 0x2768). */
typedef struct {
    int32_t bitstreamFormat;       /* +0x00 VPU_STD_AVC (2) -> codecMode 0 (lib 0xf11c) */
    vpu_phys_t bitstreamBuffer;    /* +0x04 ring buffer phys; mx6: 512-byte aligned (lib 0x19730) */
    uint32_t pBitStream;           /* +0x08 stock: desc.virt_uaddr (only MJPEG uses it) */
    int32_t bitstreamBufferSize;   /* +0x0C multiple of 1024, 1 KiB..16 MiB-1 KiB; stock 0x200000 */
    int32_t qpReport;              /* +0x10 stock 0 */
    int32_t mp4DeblkEnable;        /* +0x14 stock 0 */
    int32_t reorderEnable;         /* +0x18 stock = !lowLatency (dec 0x27f0: 1 - ctx+0x54) */
    int32_t chromaInterleave;      /* +0x1C 1 = NV12 output (stock: output fourcc 'NV12') */
    int32_t filePlayEnable;        /* +0x20 must be 0 on mx6 (lib 0x19998 -> -3) */
    int32_t picWidth;              /* +0x24 stock: stream width hint (only used if filePlay) */
    int32_t picHeight;             /* +0x28 */
    int32_t avcExtension;          /* +0x2C -> auxMode for AVC (lib 0xf158); 0 = plain AVC */
    int32_t dynamicAllocEnable;    /* +0x30 stock 0 */
    int32_t streamStartByteOffset; /* +0x34 stock = bitstreamBuffer & 7 (asserted 0, dec 0x280c) */
    int32_t mjpg_thumbNailDecEnable; /* +0x38 stock 0 */
    vpu_phys_t psSaveBuffer;       /* +0x3C stock: 2nd IOGetPhyMem 0x80000; not read on mx6 [OBS] */
    int32_t psSaveBufferSize;      /* +0x40 stock 0x80000 */
    int32_t mp4Class;              /* +0x44 stock 0 */
    int32_t mapType;               /* +0x48 0 = linear (stock); !=0 sets tiled bits (lib 0xf854) */
    int32_t tiled2LinearEnable;    /* +0x4C stock 0 */
    int32_t bitstreamMode;         /* +0x50 stock 1 -> BIT_STREAM_PARAM bit3 (lib 0xf830) [EXT: rollback] */
    int32_t jpgLineBufferMode;     /* +0x54 stock 0 */
} DecOpenParam;                    /* 0x58 */

/* DecInitialInfo, 0x98 bytes (lib memcpy 0x98 to instance, 0x11388; dec memset 0x98 0x2e10).
 * Writers: lib vpu_DecGetInitialInfo 0xff60-0x114e0 (mx6 = system_rev>>12 == 0x63 path). */
typedef struct {
    int32_t picWidth;           /* +0x00 0x1C4>>16 */
    int32_t picHeight;          /* +0x04 0x1C4&0xffff */
    uint32_t frameRateInfo;     /* +0x08 mx27 only */
    uint32_t frameRateRes;      /* +0x0C 0x1E4 */
    uint32_t frameRateDiv;      /* +0x10 0x1E8 */
    int32_t cropLeft;           /* +0x14 AVC: 0x1D8>>16 (0 if no crop) */
    int32_t cropTop;            /* +0x18 0x1DC>>16 */
    int32_t cropRight;          /* +0x1C picWidth  - (0x1D8&0xffff)  (absolute coordinate) */
    int32_t cropBottom;         /* +0x20 picHeight - (0x1DC&0xffff) */
    int32_t mp4_dataPartitionEnable;  /* +0x24 */
    int32_t mp4_reversibleVlcEnable;  /* +0x28 */
    int32_t mp4_shortVideoHeader;     /* +0x2C */
    int32_t h263_annexJEnable;        /* +0x30 */
    int32_t minFrameBufferCount; /* +0x34 0x1CC; stock allocates this + 2 (dec 0x2f40) */
    int32_t nextDecodedIdxNum;   /* +0x38 0x1D0 */
    int32_t reserved3c;          /* +0x3C not written */
    int32_t normalSliceSize;     /* +0x40 AVC, computed (lib 0x110ac) */
    int32_t worstSliceSize;      /* +0x44 */
    int32_t mjpg_thumbNailEnable;/* +0x48 */
    int32_t mjpg_sourceFormat;   /* +0x4C */
    int32_t reserved50;          /* +0x50 set to 1 on mx6 (lib 0x11310) */
    int32_t profile;             /* +0x54 0x1EC & 0xff  (profile_idc) */
    int32_t level;               /* +0x58 (0x1EC>>8) & 0xff (level_idc) */
    int32_t interlace;           /* +0x5C bit16; stock: +4 frames instead of +2 (dec 0x2f44) */
    int32_t constraint_set_flag[4]; /* +0x60..+0x6C bits 19..22 */
    int32_t direct8x8Flag;       /* +0x70 bit17 */
    int32_t vc1_psf;             /* +0x74 bit18 */
    int32_t aspectRateInfo;      /* +0x78 0x1C8 */
    uint32_t errorcode;          /* +0x7C 0x1E0, only on SEQ_INIT failure */
    int32_t bitRate;             /* +0x80 0x1B4 */
    uint32_t vp8ScaleInfo;       /* +0x84 bitfield */
    uint32_t reserved88;         /* +0x88 */
    int32_t mbInfoBufSize;       /* +0x8C = 0x100   (report buffer sizes, lib 0x112d4) */
    int32_t mvInfoBufSize;       /* +0x90 = 0x4000 */
    int32_t frameBufStatBufSize; /* +0x94 = 0x20000 */
} DecInitialInfo;                /* 0x98 */

/* DecBufInfo, 0x1C bytes: 5th argument of vpu_DecRegisterFrameBuffer (lib 0x11c18,
 * 0x1259c-0x1267c).  Stock (dec 0x31f0-0x329c): slice buffer = 3rd IOGetPhyMem 0x80000;
 * maxMbX = stride>>4, maxMbY = alignedH>>4, maxMbNum = (stride*alignedH)>>8.
 * If maxMbNum == 0 the lib substitutes 0x78 x 0x44 (1920x1088). */
typedef struct {
    vpu_phys_t avcSliceBufBase; /* +0x00 -> 0x188 (AVC, mx6) */
    int32_t avcSliceBufSize;    /* +0x04 -> 0x18C = size>>10 */
    vpu_phys_t vp8MbDataBase;   /* +0x08 VP8 only */
    int32_t vp8MbDataSize;      /* +0x0C */
    int32_t maxMbX;             /* +0x10 -> 0x1B8 bits 8..15 */
    int32_t maxMbY;             /* +0x14 -> 0x1B8 bits 0..7 */
    int32_t maxMbNum;           /* +0x18 -> 0x1B8 bits 16..31 */
} DecBufInfo;                   /* 0x1C */

/* DecParam, 0x34 bytes (lib memcpy 0x34 to inst+0x2b60, 0x13524-0x13530, unconditionally after
 * the handle check: a NULL pointer faults, EV_DEC_START).  Stock: all zero
 * (dec 0x3540-0x3590, 0x381c-0x383c).  mx6 non-MJPEG reads only +0x0C,+0x10,+0x14. */
typedef struct {
    int32_t prescanEnable;      /* +0x00 mx27 only */
    int32_t prescanMode;        /* +0x04 mx27 only */
    int32_t dispReorderBuf;     /* +0x08 mx27 only */
    int32_t iframeSearchEnable; /* +0x0C ==1 -> 0x194 bit2 (lib 0x145dc) */
    int32_t skipframeMode;      /* +0x10 -> 0x194 bits 3.. */
    int32_t skipframeNum;       /* +0x14 -> 0x198 */
    int32_t chunkSize;          /* +0x18 filePlay/MJPEG only */
    int32_t picStartByteOffset; /* +0x1C */
    vpu_phys_t picStreamBufferAddr; /* +0x20 */
    int32_t mjpegScaleDownRatioWidth;  /* +0x24 */
    int32_t mjpegScaleDownRatioHeight; /* +0x28 */
    vpu_phys_t phyJpgChunkBase; /* +0x2C */
    uint32_t virtJpgChunkBase;  /* +0x30 */
} DecParam;                     /* 0x34 */

/* DecOutputInfo, 0x134 bytes (lib memset 0x134 at 0x14d10; dec memset 0x134 at 0x35a4).
 * Writers: lib vpu_DecGetOutputInfo 0x14ae4-0x167e0. */
typedef struct {
    int32_t indexFrameDisplay;  /* +0x00 0x1C4: >=0 frame to show (now locked until
                                 *  vpu_DecClrDispFlag); -1 none; -2/-3 no display (dec 0x391c) */
    int32_t indexFrameDecoded;  /* +0x04 0x1DC: <0 nothing decoded (e.g. no free frame) */
    int32_t NumDecFrameBuf;     /* +0x08 0x1EC */
    int32_t picType;            /* +0x0C 0x1CC & 7 (VC1 adv: &0x3f) */
    int32_t picTypeFirst;       /* +0x10 (0x1CC>>3)&7 */
    int32_t idrFlg;             /* +0x14 (0x1CC>>6)&3 */
    int32_t numOfErrMBs;        /* +0x18 0x1C8; stock only counts/logs it (dec 0x3ca0) */
    int32_t reserved1c;         /* +0x1C */
    int32_t vc1_fields[3];      /* +0x20..+0x28 VC1 only */
    int32_t reserved2c;         /* +0x2C 0x1D4 on non-mx6 only */
    int32_t notSufficientPsBuffer;    /* +0x30 (0x1D8>>3)&1, AVC; stock logs (dec 0x3aa4) */
    int32_t notSufficientSliceBuffer; /* +0x34 (0x1D8>>2)&1, AVC; stock logs (dec 0x3a64) */
    int32_t decodingSuccess;    /* +0x38 0x1D8&1; |0x10 if bitstreamMode && (0x1D8&0x10)
                                 *  [EXT/INF: "not enough bitstream, rolled back, feed more and
                                 *  call StartOneFrame again"]; stock only logs ==0 (dec 0x3ae4) */
    int32_t interlacedFrame;    /* +0x3C (0x1CC>>18)&1 */
    int32_t mp2_field;          /* +0x40 MPEG2 only */
    int32_t h264Npf;            /* +0x44 (0x1CC>>16)&3 */
    int32_t pictureStructure;   /* +0x48 (0x1CC>>19)&3 */
    int32_t topFieldFirst;      /* +0x4C */
    int32_t repeatFirstField;   /* +0x50 */
    int32_t progressiveFrame;   /* +0x54 */
    int32_t fieldSequence;      /* +0x58 */
    int32_t decPicHeight;       /* +0x5C 0x1BC & 0xffff */
    int32_t decPicWidth;        /* +0x60 0x1BC >> 16 */
    int32_t decPicCropLeft;     /* +0x64 AVC 0x1E0>>16 */
    int32_t decPicCropTop;      /* +0x68 0x1E4>>16 */
    int32_t decPicCropRight;    /* +0x6C decPicWidth  - (0x1E0&0xffff) */
    int32_t decPicCropBottom;   /* +0x70 decPicHeight - (0x1E4&0xffff) */
    int32_t aspectRateInfo;     /* +0x74 0x1F0 */
    uint32_t frameRateRes;      /* +0x78 0x1F4 */
    uint32_t frameRateDiv;      /* +0x7C 0x1F8 (x2 for AVC) */
    uint8_t vp8_info[8];        /* +0x80 */
    int32_t mvc_fields[2];      /* +0x88 */
    int32_t avcFpaSeiExist;     /* +0x90 AVC frame-packing SEI (0x19C..0x1A4) */
    uint8_t avcFpaSei[0xD8 - 0x94]; /* +0x94..+0xD7 */
    vpu_phys_t frameStartPos;   /* +0xD8 RD_PTR before the picture (phys) */
    vpu_phys_t frameEndPos;     /* +0xDC 0x120 RD_PTR after */
    int32_t consumedByte;       /* +0xE0 with ring wrap */
    uint8_t reportInfo[0x134 - 0xE4]; /* +0xE4.. MB/MV/frame-status reports (not enabled) */
} DecOutputInfo;                /* 0x134 */

/* Decoder API (dlsym names).  Semantics [OBS]:
 *  vpu_DecOpen(&h, &op): GetCodecInstance from the SYSTEM-WIDE pool, allocates the
 *    instance context with IOGetPhyMem (0x14000, +0x80000 for AVC on mx6, lib 0xef74-0xefe0).
 *  vpu_DecSetEscSeqInit(h, 1/0): sets/clears BIT_STREAM_PARAM bit0 (lib 0xfe68).  Stock wraps
 *    GetInitialInfo with (h,1) ... (h,0) (dec 0x2e64-0x2e88): lets SEQ_INIT escape instead of
 *    waiting for data.  The library itself polls BUSY with NO timeout (lib 0x1093c).
 *  vpu_DecGetInitialInfo(h, &info): -10 if ring empty (lib 0x10338); SEQ_INIT; 0x1C0<0 -> -18
 *    (+swreset); 0x1C0==0 -> info.errorcode, -1; with bitstreamMode and 0x1C0&0x10 -> -1 [INF:
 *    need more data]; -12 if called twice.
 *  vpu_DecRegisterFrameBuffer(h, fb[], num, stride, DecBufInfo*): -10 no initial info,
 *    -12 twice, -7 fb NULL, -8 num < minFrameBufferCount, -9 stride < picWidth or stride&7.
 *  vpu_DecGetBitstreamBuffer(h, &rd, &wr, &room): ring buffer, PHYS pointers; rd from 0x120 if
 *    the instance is loaded in BIT else saved; room = free bytes minus 0x801 guard (lib 0x12bb4).
 *  vpu_DecUpdateBitstreamBuffer(h, n): n>0: wr += n with wrap (caller copies the wrapping part
 *    to the buffer start itself, like dec imxmmf_fill_input 0x3ec8-0x3f50); -3 if it would pass
 *    rd.  n==0: sets end-of-stream flag (STREAM_PARAM bit2), cleared by the next n>0.
 *  vpu_DecStartOneFrame(h, &param): param is copied whole, never NULL; -10 if no frame
 *    buffers; TAKES the cross-process VPU mutex
 *    (lib 0x1366c) and RETURNS WITHOUT WAITING; pendingInst = h (lib 0x14a50).
 *  vpu_WaitForInt(ms): devctl WAIT4INT(ms) then result = (BUSY 0x160 == 0) ? 0 : -1
 *    regardless of the devctl result (lib 0x7a10-0x7ab8).
 *  vpu_DecGetOutputInfo(h, &out): -10 if nothing pending, -2 if another instance pending;
 *    if still BUSY it only logs and reads garbage (lib 0x150e8); 0x1D8<0 -> swreset, -18;
 *    always clears pendingInst and RELEASES the VPU mutex (lib 0x1672c-0x16750).
 *  vpu_DecClrDispFlag(h, idx): clears bit idx of FRM_DIS_FLG (lib 0x16b00); -3 bad idx.
 *  vpu_DecBitBufferFlush(h): DEC_BUF_FLUSH (cmd 8), rd = wr = buffer start (lib 0x16950).
 *  vpu_DecClose(h): -6 if h is pending (call GetOutputInfo first); SEQ_END (cmd 2, BUSY poll
 *    without timeout) if initial info was obtained; frees context; FreeCodecInstance.
 *  vpu_DecGiveCommand: not imported by the stock decoder plug-in (dec PLT 0x1958-0x1bc8).
 */
typedef int (*pfn_vpu_DecOpen)(vpu_handle_t *h, DecOpenParam *op);
typedef int (*pfn_vpu_DecSetEscSeqInit)(vpu_handle_t h, int escape);
typedef int (*pfn_vpu_DecGetInitialInfo)(vpu_handle_t h, DecInitialInfo *info);
typedef int (*pfn_vpu_DecRegisterFrameBuffer)(vpu_handle_t h, FrameBuffer *fb, int num,
                                              int stride, DecBufInfo *bi);
typedef int (*pfn_vpu_DecGetBitstreamBuffer)(vpu_handle_t h, vpu_phys_t *rd, vpu_phys_t *wr,
                                             uint32_t *room);
typedef int (*pfn_vpu_DecUpdateBitstreamBuffer)(vpu_handle_t h, uint32_t size);
typedef int (*pfn_vpu_DecStartOneFrame)(vpu_handle_t h, DecParam *p);
typedef int (*pfn_vpu_DecGetOutputInfo)(vpu_handle_t h, DecOutputInfo *out);
typedef int (*pfn_vpu_DecClrDispFlag)(vpu_handle_t h, int index);
typedef int (*pfn_vpu_DecBitBufferFlush)(vpu_handle_t h);
typedef int (*pfn_vpu_DecClose)(vpu_handle_t h);

/* Stock decoder sequence (imx6_video_decoder.so) [OBS]:
 *  open  (imxmmf_open 0x2390): vpu_Init(0); vpu_GetVersionInfo(&v); IOGetPhyMem 0x200000
 *        (bitstream), 0x80000 (psSave), 0x80000 (slice) (0x295c/0x29b8/0x2a6c); vpu_DecOpen.
 *  init  (imxmmf_init 0x2de4): fill ring with stream; DecSetEscSeqInit(h,1);
 *        DecGetInitialInfo; DecSetEscSeqInit(h,0); allocate min+2 frames; DecRegisterFrameBuffer
 *        (h, fb, num, stride=align16(w), &DecBufInfo).
 *  feed  (imxmmf_fill_input 0x3e60): DecGetBitstreamBuffer -> memcpy into virt(wr) with wrap ->
 *        DecUpdateBitstreamBuffer(h, n).  Partial acceptance, returns bytes taken.
 *  frame (imxmmf_decode 0x3534): for each output buffer given back by the renderer:
 *        DecClrDispFlag(h, idx); push input; [len==0 -> DecUpdateBitstreamBuffer(h,0)];
 *        DecStartOneFrame(h, &zeroParam); vpu_WaitForInt(2000) (on failure only logs and
 *        continues); DecGetOutputInfo(h,&out); if out.indexFrameDisplay >= 0 hand frame
 *        indexFrameDisplay downstream and mark it "displayed".
 *  close (imxmmf_close 0x1f28): return displayed frames; DecClose; if -6: DecGetOutputInfo +
 *        DecClose again; IOFreePhyMem x3 (no IOFreeVirtMem); vpu_UnInit.
 */

/* ------------------------------------------------------------------------------------ */
/* 4. Encoder                                                                              */

/* EncOpenParam, 0x848 bytes (copied whole, doc 3.2 / lib 0x943c).  Stock values from enc
 * imx6mmf_open 0x2270-0x2354 (struct at sp+0x28).  Range checks lib CheckEncOpenParam
 * 0x18a30; tail fields mapped by SEQ_INIT writes lib 0xaacc-0xadd4. */
typedef struct {
    vpu_phys_t bitstreamBuffer;   /* +0x000 4-byte aligned (lib 0x18a7c) */
    uint32_t bitstreamBufferSize; /* +0x004 multiple of 1024; stock align1K(w*h*3/2) */
    int32_t bitstreamFormat;      /* +0x008 VPU_STD_AVC -> codecMode 8 */
    int32_t picWidth;             /* +0x00C >= 32 */
    int32_t picHeight;            /* +0x010 >= 16 */
    uint32_t frameRateInfo;       /* +0x014 stock cfg[0]/cfg[1] (fps) */
    int32_t bitRate;              /* +0x018 0..32767 (kbps [EXT]); 0 = no RC, then
                                   *  EncParam.quantParam 0..51 is checked (lib 0x19210) */
    int32_t initialDelay;         /* +0x01C stock 0 */
    int32_t vbvBufferSize;        /* +0x020 stock 0 */
    int32_t enableAutoSkip;       /* +0x024 stock 0 */
    int32_t gopSize;              /* +0x028 stock cfg[8] */
    int32_t linear2TiledEnable;   /* +0x02C -> encInfo+0x89c (lib 0x95dc); stock 0 */
    int32_t mapType;              /* +0x030 stock 0 = linear */
    int32_t sliceMode;            /* +0x034 cfg[0x10] */
    int32_t sliceSizeMode;        /* +0x038 cfg[0x14] */
    int32_t sliceSize;            /* +0x03C cfg[0x18] */
    int32_t intraRefresh;         /* +0x040 cfg[0x1c] */
    int32_t sliceReport, mbReport, mbQpReport; /* +0x044..+0x04C stock 0 */
    int32_t rcIntraQp;            /* +0x050 stock -1 */
    int32_t chromaInterleave;     /* +0x054 1 = NV12 source (stock when input is NV12) */
    int32_t dynamicAllocEnable;   /* +0x058 stock 0 */
    int32_t ringBufferEnable;     /* +0x05C stock 0 -> linear stream buffer, every picture and
                                   *  header written from bitstreamBuffer start (doc 3.2) */
    /* EncAvcParam (union with MPEG4/H263/MJPEG params, up to +0x81C) */
    int32_t avc_constrainedIntraPredFlag; /* +0x060 */
    int32_t avc_disableDeblk;             /* +0x064 0..2 */
    int32_t avc_deblkFilterOffsetAlpha;   /* +0x068 stock 6 (-6..6) */
    int32_t avc_deblkFilterOffsetBeta;    /* +0x06C stock 0 */
    int32_t avc_chromaQpOffset;           /* +0x070 stock 10 (-12..12) */
    int32_t avc_audEnable;                /* +0x074 */
    int32_t avc_fmoEnable;                /* +0x078 */
    int32_t avc_fmoSliceNum;              /* +0x07C */
    int32_t avc_fmoType;                  /* +0x080 */
    int32_t avc_fmoSliceSaveBufSize;      /* +0x084 */
    int32_t avc_frameCroppingFlag;        /* +0x088 SPS crop on ENCODE_HEADER (lib 0x19424) */
    int32_t avc_frameCropLeft;            /* +0x08C -> 0x18C hi */
    int32_t avc_frameCropRight;           /* +0x090 -> 0x18C lo; DM: 14 */
    int32_t avc_frameCropTop;             /* +0x094 -> 0x190 hi */
    int32_t avc_frameCropBottom;          /* +0x098 -> 0x190 lo; DM: 8 (units [UNK], doc 3.2) */
    uint8_t stdParamRest[0x81C - 0x09C];  /* rest of the union, stock 0 */
    int32_t userQpMin;                    /* +0x81C } -> 0x1C8 (lib 0xab4c) */
    int32_t userQpMax;                    /* +0x820 } */
    int32_t userQpMinEnable;              /* +0x824 */
    int32_t userQpMaxEnable;              /* +0x828 */
    uint32_t userGamma;                   /* +0x82C stock 0x6000 -> 0x1CC */
    int32_t RcIntervalMode;               /* +0x830 stock 1  -> 0x1D0 | MbInterval<<2 */
    int32_t MbInterval;                   /* +0x834 stock 0 */
    int32_t avcIntra16x16OnlyModeEnable;  /* +0x838 -> 0x188 bit9 */
    int32_t MESearchRange;                /* +0x83C stock 3  -> 0x1D8 | MEUseZeroPmv<<2 */
    int32_t MEUseZeroPmv;                 /* +0x840 */
    int32_t IntraCostWeight;              /* +0x844 -> 0x1D4 */
} EncOpenParam;                           /* 0x848 */

/* EncInitialInfo, 0x10 bytes (enc zeroes sp+0x40..0x4c, 0x1d84-0x1da0; doc 4.1). */
typedef struct {
    int32_t minFrameBufferCount; /* +0x0 AVC: 2 (doc, lib 0xaf2c) */
    int32_t mbInfoBufSize;       /* +0x4 0x4000 */
    int32_t mvInfoBufSize;       /* +0x8 0x4000 */
    int32_t sliceInfoBufSize;    /* +0xC 0x20000 */
} EncInitialInfo;

/* EncParam, 0x24 bytes.  Reads: lib CheckEncParam 0x19170, EncStartOneFrame 0xcc7c-0xcf18.
 * Stock (enc imx6mmf_encode 0x1a18-0x1ae0, struct at sp+0x60). */
typedef struct {
    FrameBuffer *sourceFrame; /* +0x00 ANY FrameBuffer, need not be registered (see below) */
    int32_t encTopOffset;     /* +0x04 stock 0 */
    int32_t encLeftOffset;    /* +0x08 stock 0 */
    int32_t forceIPicture;    /* +0x0C 0/1 -> 0x194 bit1 */
    int32_t skipPicture;      /* +0x10 0/1 -> 0x194 bit0 (no source needed) */
    int32_t quantParam;       /* +0x14 -> 0x18C; range-checked only if bitRate == 0 */
    vpu_phys_t picStreamBufferAddr; /* +0x18 used only if dynamicAllocEnable */
    int32_t picStreamBufferSize;    /* +0x1C */
    int32_t enableAutoSkip;   /* +0x20 mx27 only; stock 1 */
} EncParam;

/* EncOutputInfo, 0x58 bytes (enc memset 0x58, 0x1a58).  lib vpu_EncGetOutputInfo 0xd454. */
typedef struct {
    vpu_phys_t bitstreamBuffer; /* +0x00 = stream buffer start (encInfo+0x85c) */
    uint32_t bitstreamSize;     /* +0x04 WR_PTR(0x124) - start; stock copies from virt start */
    int32_t bitstreamWrapAround;/* +0x08 0x1D0; stock: !=0 -> frame error (enc 0x1b1c) */
    int32_t skipEncoded;        /* +0x0C (0x1C4>>2)&1 */
    int32_t picType;            /* +0x10 0x1C4&3: 0 = I (stock flags keyframe), 1 = P */
    int32_t numOfSlices;        /* +0x14 0x1CC */
    uint32_t reserved18;        /* +0x18 0x1C8 */
    uint8_t reportInfo[0x58 - 0x1C];
} EncOutputInfo;

/* EncHeaderParam, 0x14 bytes (enc imx6mmf_get_headers 0x1808, sp+0xc). lib EncodeHeader
 * 0x19298: on mx6 linear buffer the header is written at the stream buffer START; output
 * +0 = 0x184 (start), +4 = WR_PTR - start.  userProfileLevel* ignored on mx6. */
typedef struct {
    vpu_phys_t buf;  /* +0x00 out */
    int32_t size;    /* +0x04 out */
    int32_t headerType; /* +0x08 0 = SPS (gets frameCropping), 1 = PPS */
    int32_t userProfileLevelEnable;     /* +0x0C */
    int32_t userProfileLevelIndication; /* +0x10 */
} EncHeaderParam;

/* vpu_EncGiveCommand command numbers [OBS doc 3.1/4.4, lib table 0xdf5c]. Observed cases take the
 * cross-process VPU mutex (lib 0xe2a8/0xe350): never call while a frame is pending. */
enum {
    VPU_ENC_PUT_AVC_HEADER = 0x0F, /* arg EncHeaderParam* */
    VPU_ENC_SET_SLICE_INFO = 0x19,
    VPU_ENC_SET_GOP_NUMBER = 0x1A,
    VPU_ENC_SET_BITRATE = 0x1C,
    VPU_ENC_SET_FRAME_RATE = 0x1D,
};

/* Encoder API [OBS]:
 *  vpu_EncRegisterFrameBuffer(h, fb[], num, frameBufStride, sourceBufStride, subSampBaseA,
 *    subSampBaseB, EncExtBufInfo*): -8 num < minFrameBufferCount, -9 stride&7 or 0
 *    (lib 0xb1a0-0xb1d8).  sourceBufStride used only on mx27; subSampA/B -> 0x188/0x18C;
 *    ext info only for MVC/MPEG4-DP (may be NULL).  Stock (enc 0x1dd8-0x2028): allocates
 *    min+2 frames, each mmap64(PROT 0xB00, MAP_PHYS|MAP_ANON) w*align16(h)*3/2 bytes,
 *    strideY=w, strideC=w/2, Cb=Y+w*alignH, Cr=Cb+w*alignH/4, MvCol=0; registers num=min,
 *    subSampA = fb[min].bufY, subSampB = fb[min+1].bufY, ext = NULL.
 *  vpu_EncStartOneFrame(h, &p): on AVC/0x63 does vpu_mx6_swreset(0) first (lib 0xc528); takes
 *    the VPU mutex; writes 0x180 = src->myIndex, 0x184 = src->strideY,
 *    0x1A8 = bufY + top*strideY + left, 0x1AC = bufCb + (top/2)*strideC + left/2,
 *    0x1B0 = bufCr + (top/2)*strideC + left/2 (lib 0xcd1c-0xce18); PIC_RUN; returns at once.
 *    -> The source frame is NOT validated against the registered set: the stock plug-in itself
 *       passes a screen buffer with myIndex = ctx+0x28 = minFrameBufferCount + 2, the frames it
 *       ALLOCATED, not the min it registered (enc 0x1dfc -> 0x1e58 -> 0x1a84, EV_ENC_SRC).
 *    -> For NV12 do NOT use encLeftOffset: the lib adds left/2 to the CbCr address (correct
 *       for planar, wrong for interleaved).  Precompute bufY/bufCb of the rectangle yourself
 *       and pass offsets 0; strideY = full decoded-frame stride (per frame, any value).
 *    Alignment of the rectangle: [UNK] in code (no lib check).  Recommended [INF]: left
 *       multiple of 16, top even, stride multiple of 16 (decoder stride already is).
 *  vpu_EncGetOutputInfo(h, &o): mirrors DecGetOutputInfo (-10/-2, 0x1D8<0 -> -18, releases
 *    mutex).  Stock calls it only if vpu_WaitForInt(100) >= 0 (enc 0x1afc): a timeout leaves
 *    the mutex held by that thread - do not copy that.
 *  vpu_EncClose(h): -6 if pending; SEQ_END (cmd 2) + BUSY poll; frees; FreeCodecInstance.
 */
typedef int (*pfn_vpu_EncOpen)(vpu_handle_t *h, EncOpenParam *op);
typedef int (*pfn_vpu_EncGetInitialInfo)(vpu_handle_t h, EncInitialInfo *info);
typedef int (*pfn_vpu_EncRegisterFrameBuffer)(vpu_handle_t h, FrameBuffer *fb, int num,
                                              int frameBufStride, int sourceBufStride,
                                              vpu_phys_t subSampBaseA, vpu_phys_t subSampBaseB,
                                              void *extBufInfo);
typedef int (*pfn_vpu_EncGiveCommand)(vpu_handle_t h, int cmd, void *param);
typedef int (*pfn_vpu_EncStartOneFrame)(vpu_handle_t h, EncParam *p);
typedef int (*pfn_vpu_EncGetOutputInfo)(vpu_handle_t h, EncOutputInfo *o);
typedef int (*pfn_vpu_EncClose)(vpu_handle_t h);

/* ------------------------------------------------------------------------------------ */
/* 5. Init / locking / sharing [OBS]                                                       */
/*  vpu_Init(void *unused): IOSystemInit: if this process already has /dev/mxc_vpu open ->   */
/*    vpu_active_num++ and return (lib 0x4834-0x48f8).  Else open, map regs, map shared mem  */
/*    "/imx6vpu-shared-mem-<mxcvpu pid>" (system-wide: mutexes at +4/+0xC, 32 CodecInst x    */
/*    0x2B94 at +0x14, pendingInst at +0x57294, "BIT loaded" flag at +0x57298;               */
/*    lib 0x7e84, 0x181e8, 0x1b224), GET_WORK_ADDR 0x7C000 shared work buffer.  Firmware is  */
/*    loaded only if BIT is not running (lib 0x7e98 -> DownloadBitCodeTable 0x17fe4 ->       */
/*    LoadBitCodeTable 0x1792c, the only reader of VPU_FW_PATH; EV_FW_PATH); file from        */
/*    $VPU_FW_PATH or /lib/firmware/vpu - on the unit only /fw/vpu/vpu_fw_imx6q.bin exists    */
/*    and cpu.conf names VPU_FW_PATH=/fw/vpu for mm-renderer only, but the startup script     */
/*    exports it before the service launcher, whose spawn passes its environ on: SAL has it.  */
/*  vpu_UnInit(): IOSystemShutdown: refcount-1; at 0: munmap work buf + regs, close(fd)      */
/*    -> mxcvpu frees ALL physical blocks of this process (lib 0x4f8c-0x5430). No HW reset.  */
/*  Cross-process lock: LockVpu = pthread_mutex_timedlock on the shared pshared mutex,       */
/*    timeout = getenv("VPU_MUTEX_TIMEOUT") s, default 10 (lib 0x1b3cc-0x1b418, 0x1b4b8).    */
/*    Held from Dec/EncStartOneFrame until Dec/EncGetOutputInfo; also taken by Open,         */
/*    GetInitialInfo, RegisterFrameBuffer, GiveCommand, BitBufferFlush, Close.  Mutex owner  */
/*    death is revived by mxcvpu (SyncMutexEvent/mutex_revive, doc 1.3).                     */
/*  Interrupt: one IRQ, only PIC_RUN raises it (INT_ENABLE=8, doc 2.2); mxcvpu serves        */
/*    WAIT4INT in its single dispatch thread -> while one client waits, every other client's */
/*    devctl (alloc, wait) blocks (doc 1.2).                                                 */
/*  Users: only imx6_video_decoder.so (in mm-renderer) and imx6_video_encoder.so (in DM via  */
/*    OpenMAX AL) link libmxcvpu; SAL does not -> in SAL we are the only in-process user.    */
typedef int (*pfn_vpu_Init)(void *unused);
typedef int (*pfn_vpu_UnInit)(void);
typedef int (*pfn_vpu_WaitForInt)(int timeout_ms);
typedef int (*pfn_vpu_IsBusy)(void); /* = (BUSY 0x160 != 0), lib 0x6e64 */
typedef int (*pfn_IOGetPhyMem)(vpu_mem_desc *d);
typedef int (*pfn_IOFreePhyMem)(vpu_mem_desc *d);
typedef int (*pfn_IOGetVirtMem)(vpu_mem_desc *d);  /* returns d->virt_uaddr */
typedef int (*pfn_IOFreeVirtMem)(vpu_mem_desc *d);

/* ------------------------------------------------------------------------------------ */
/* 6. Pitfalls / safe sequence                                                             */
/*  - Exactly one Start..GetOutputInfo pair in flight per process thread: any other call     */
/*    that takes the VPU mutex (other instance's Start, GiveCommand, Close...) from the same */
/*    thread before GetOutputInfo deadlocks until VPU_MUTEX_TIMEOUT (-17).                   */
/*  - After vpu_WaitForInt ALWAYS call GetOutputInfo, even on timeout (decoder plug-in does, */
/*    encoder plug-in does not).  On timeout prefer looping vpu_WaitForInt while            */
/*    vpu_IsBusy() for a bounded time first: GetOutputInfo on a busy VPU returns garbage but */
/*    releases the mutex and pendingInst.                                                    */
/*  - GetInitialInfo/Close/RegisterFrameBuffer poll BUSY with no timeout while holding the   */
/*    system-wide mutex: a hung BIT stalls mm-renderer and DM too.  Use DecSetEscSeqInit(1)  */
/*    and feed SPS+PPS+IDR before GetInitialInfo.                                            */
/*  - Never vpu_UnInit/exit with an instance open: closing the fd frees our frame/bitstream  */
/*    memory while BIT may still write it, and the CodecInst slot (32 system-wide) stays     */
/*    inUse until mxcvpu restarts [INF].  DecClose/EncClose only after GetOutputInfo.        */
/*  - Keep every displayed decoder frame locked (no ClrDispFlag) until its encode finished;  */
/*    clear it right after, or the decoder runs out of frames (indexFrameDecoded < 0).       */
/*  - Decoded frames, bitstream and encoder buffers from IOGetPhyMem are uncached: CPU reads */
/*    of whole frames are slow; the encoder reads the decoded frame directly (zero copy).    */
/*  - Safe cleanup: [if pending: vpu_WaitForInt + GetOutputInfo] -> EncClose -> DecClose     */
/*    (retry after GetOutputInfo on -6) -> IOFreePhyMem of own blocks -> vpu_UnInit.         */
/*                                                                                           */
/* Proposed transcoder sequence (derived from the stock sequences above, untested):          */
/*  (VPU_FW_PATH=/fw/vpu inherited) vpu_Init(0);                                             */
/*  DEC: IOGetPhyMem ring 0x200000, slice 0x80000 (+psSave 0x80000 like stock);              */
/*       DecOpen{AVC, ring, 0x200000, reorder=0 for low latency, NV12=1, mapType 0,          */
/*       bitstreamMode 1}; feed SPS/PPS/IDR via GetBitstreamBuffer/memcpy/Update;            */
/*       SetEscSeqInit(1); GetInitialInfo; SetEscSeqInit(0);                                 */
/*       n = minFrameBufferCount+2 frames of stride*alignH*7/4 (IOGetPhyMem each, layout 2.); */
/*       DecRegisterFrameBuffer(h, fb, n, stride, {slice, 0x80000, 0, 0, stride>>4,          */
/*       alignH>>4, stride*alignH>>8}).                                                      */
/*  ENC: IOGetPhyMem stream 0x90000 (1024x384x1.5 = 0x90000); EncOpen{stream, 0x90000, AVC,  */
/*       1024, 384, fps, kbps, gop, rcIntraQp -1, chromaInterleave 1, alpha 6, chromaQp 10,  */
/*       crop flag 1 {0,14,0,8}, userGamma 0x6000, RcIntervalMode 1, MESearchRange 3};       */
/*       EncGetInitialInfo; min+2 frames 1024x384 NV12 (IOGetPhyMem); EncRegisterFrameBuffer */
/*       (h, fb, min, 1024, 1024, fb[min].bufY, fb[min+1].bufY, NULL).                       */
/*  LOOP: feed one access unit; DecStartOneFrame(zero DecParam); WaitForInt(2000);           */
/*       DecGetOutputInfo; if decodingSuccess&0x10 feed more and restart [INF];              */
/*       k = indexFrameDisplay >= 0 -> src = {strideY=stride, myIndex=min+2,                 */
/*       bufY = fb[k].bufY + top*stride + left, bufCb = fb[k].bufCb + (top/2)*stride + left};*/
/*       if IDR wanted: GiveCommand(0x0F,{type 0}) copy SPS from stream virt start,          */
/*       GiveCommand(0x0F,{type 1}) copy PPS, then forceIPicture=1;                          */
/*       EncStartOneFrame; WaitForInt(100); EncGetOutputInfo; copy bitstreamSize bytes from  */
/*       stream virt start (Annex-B, 00 00 00 01); DecClrDispFlag(h, k).                     */
/* ------------------------------------------------------------------------------------ */

#endif /* MCN_VPU_ABI_H */
