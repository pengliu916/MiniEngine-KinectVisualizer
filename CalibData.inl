#if !__hlsl
typedef DirectX::XMUINT2 uint2;
typedef DirectX::XMFLOAT2 float2;
typedef DirectX::XMFLOAT4 float4;
typedef DirectX::XMMATRIX matrix;
#endif

#define DEPTH_RESO uint2(512, 424)
#define COLOR_RESO uint2(1920, 1080)

// Calibration data for Kinect2 (at home) from "Mon May  9 00:06:42 2016"
#define DEPTH_K float4(8.1123228034613387e-02f, -2.5401647716173742e-01f, \
8.5823592746789967e-02f, 0.f)
#define DEPTH_P float2(-7.1939346256350565e-04f, -3.9918163217997978e-04f)
// OpenCV camera uses +z forward, -y up
// This program uses camera with -z forward, y up
// thus we need to multiply -1 for fx to the original OpenCV calib data
#define DEPTH_F float2(-3.6154786682149080e+02f, 3.6089181073659796e+02f)
#define DEPTH_C float2(2.5835355135786824e+02f, 2.0575487215776474e+02f)

#define COLOR_K float4(2.9149065825195806e-02f, -2.9654300241936486e-02f, \
-5.7307859602833576e-03f, 0.f)
#define COLOR_P float2(-8.0282793629378685e-04f, -1.0993038057764051e-03f)
// OpenCV camera uses +z forward, -y up
// This program uses camera with -z forward, y up
// thus we need to multiply -1 for fx to the original OpenCV calib data
#define COLOR_F float2(-1.0500229488335692e+03f,1.0497918256490759e+03f)
#define COLOR_C float2(9.7136112531037020e+02f, 5.4918051628416595e+02f)

// Already transposed since DirectX load is column major which means 
// first 4 will be loaded as first column
#define DEPTH2COLOR_MAT matrix( \
9.9999863070816453e-01f, 1.0114393597790731e-03f,\
-1.3097985407500218e-03f, 0.f,\
-1.0118969315633594e-03f, 9.9999942722596702e-01f,\
-3.4872960540394365e-04f, 0.f,\
1.3094450716825987e-03f, 3.5005450901571112e-04f,\
9.9999908140730054e-01f, 0.f,\
5.1627068568015556e-02f, -7.7893035962837452e-05f,\
3.8187016837237385e-03f, 1.f)
