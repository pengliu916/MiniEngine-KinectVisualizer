
// Calibration data from "Mon May  9 00:06:42 2016"
#define KINECT2_DEPTH_K float4(8.1123228034613387e-02, -2.5401647716173742e-01, 8.5823592746789967e-02, 0)
#define KINECT2_DEPTH_P float2(-7.1939346256350565e-04, -3.9918163217997978e-04)
#define KINECT2_DEPTH_F float2(3.6154786682149080e+02, 3.6089181073659796e+02)
#define KINECT2_DEPTH_C float2(2.5835355135786824e+02, 2.0575487215776474e+02)

#define KINECT2_COLOR_K float4(2.9149065825195806e-02, -2.9654300241936486e-02, -5.7307859602833576e-03, 0)
#define KINECT2_COLOR_P float2(-8.0282793629378685e-04, -1.0993038057764051e-03)
#define KINECT2_COLOR_F float2(1.0500229488335692e+03,1.0497918256490759e+03)
#define KINECT2_COLOR_C float2(9.7136112531037020e+02, 5.4918051628416595e+02)

// Already transposed since DirectX load is column major which means first 4 will be loaded as first column
#define	KINECT2_DEPTH2COLOR_MAT	\
matrix(	9.9999863070816453e-01,		1.0114393597790731e-03,		-1.3097985407500218e-03,	0.0, \
		-1.0118969315633594e-03,	9.9999942722596702e-01,		-3.4872960540394365e-04,	0.0, \
		1.3094450716825987e-03,		3.5005450901571112e-04,		9.9999908140730054e-01,		0.0, \
		5.1627068568015556e-02,		-7.7893035962837452e-05,	3.8187016837237385e-03,		1.0)