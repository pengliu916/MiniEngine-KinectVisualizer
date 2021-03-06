#ifndef DEF_SURFBUF
#error "DEF_SURFBUF() undefined"
#endif

DEF_SURFBUF(KINECT_COLOR,       COLOR_SIZE, DXGI_FORMAT_R11G11B10_FLOAT,    DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(KINECT_DEPTH,       DEPTH_SIZE, DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(KINECT_INFRA,       DEPTH_SIZE, DXGI_FORMAT_R11G11B10_FLOAT,    DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(KINECT_DEPTH_VIS,   DEPTH_SIZE, DXGI_FORMAT_R11G11B10_FLOAT,    DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(CONFIDENCE,         DEPTH_SIZE, DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(KINECT_NORMAL,      DEPTH_SIZE, DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(TSDF_NORMAL,        DEPTH_SIZE, DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(FILTERED_DEPTH,     DEPTH_SIZE, DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(TSDF_DEPTH,         DEPTH_SIZE, DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(VISUAL_NORMAL,      VARI_SIZE1, DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(VISUAL_DEPTH,       VARI_SIZE1, DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_D32_FLOAT)

DEF_SURFBUF(DEBUG_A_DEPTH,      DEPTH_SIZE, DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(DEBUG_A_NORMAL,     DEPTH_SIZE, DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(DEBUG_B_DEPTH,      DEPTH_SIZE, DXGI_FORMAT_R16_UNORM,          DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(DEBUG_B_NORMAL,     DEPTH_SIZE, DXGI_FORMAT_R10G10B10A2_UNORM,  DXGI_FORMAT_UNKNOWN)
DEF_SURFBUF(DEBUG_CONFIDENCE,   DEPTH_SIZE, DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_UNKNOWN)
