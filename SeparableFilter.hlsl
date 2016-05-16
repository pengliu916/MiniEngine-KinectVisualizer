#define KERNEL_RADIUS               ( 16 )   // Must be an even number


// Defines that control the CS logic of the kernel
#define KERNEL_DIAMETER             ( KERNEL_RADIUS * 2 + 1 )
#define RUN_LINES                   ( 2 )   // Needs to match g_uRunLines in SeparableFilter11.cpp
#define RUN_SIZE                    ( 128 ) // Needs to match g_uRunSize in SeparableFilter11.cpp
#define KERNEL_DIAMETER_MINUS_ONE   ( KERNEL_DIAMETER - 1 )
#define RUN_SIZE_PLUS_KERNEL        ( RUN_SIZE + KERNEL_DIAMETER_MINUS_ONE )
#define PIXELS_PER_THREAD           ( 4 )
#define NUM_THREADS                 ( RUN_SIZE / PIXELS_PER_THREAD )
#define LOAD_PER_THREAD             ( RUN_SIZE_PLUS_KERNEL / NUM_THREADS )
#define EXTRA_SAMPLES               ( RUN_SIZE_PLUS_KERNEL - ( NUM_THREADS * SAMPLES_PER_THREAD ) )

#if Horizontal_Pass
#define input_data
#else
#endif // Horizontal_Pass

//--------------------------------------------------------------------------------------
// Get a Gaussian weight
//--------------------------------------------------------------------------------------
#define GAUSSIAN_WEIGHT( _fX, _fDeviation, _fWeight ) \
    _fWeight = 1.0f / sqrt( 2.0f * PI * _fDeviation * _fDeviation ); \
    _fWeight *= exp( -( _fX * _fX ) / ( 2.0f * _fDeviation * _fDeviation ) ); 

//--------------------------------------------------------------------------------------
// LDS definition and access macros
//--------------------------------------------------------------------------------------
groupshared uint g_LDS[RUN_LINES][RUN_SIZE_PLUS_KERNEL]

#define WRITE_TO_LDS( _Data, _iLineOffset, _iPixelOffset) \
	g_LDS[_iLineOffset][_iPixelOffset] = _Data;

#define READ_FROM_LDS(_Data, _iLineOffset, _iPixelOffset) \
	_Data = g_LDS[_iLineOffset][_iPixelOffset];

//--------------------------------------------------------------------------------------
// Buffer definition and access function
//--------------------------------------------------------------------------------------
Texture2D<uint> DepthMap : register(t0);

#define SAMPLE(_i2xy) DepthMap[_i2xy]

float4 g_f4OutputSize;


//--------------------------------------------------------------------------------------
// Defines the filter kernel logic. User supplies macro's for custom filter
//--------------------------------------------------------------------------------------
void ComputeFilterKernel( int iPixelOffset, int iLineOffset, int2 i2Center, int2 i2Inc )
{
	CS_Output O = (CS_Output)0;
	KernelData KD[PIXELS_PER_THREAD];
	int iPixel, iIteration;
	RAWDataItem RDI[PIXELS_PER_THREAD];

	// Read the kernel center values in from the LDS
	[unroll]
	for (iPixel = 0; iPixel < PIXELS_PER_THREAD; ++iPixel)
	{
		READ_FROM_LDS( iLineOffset, (iPixelOffset + KERNEL_RADIUS + iPixel), RDI[iPixel] )
	}

	// Macro defines what happens at the kernel center
	KERNEL_CENTER( KD, iPixel, PIXELS_PER_THREAD, O, RDI )

		// Prime the GPRs for the first half of the kernel
		[unroll]
	for (iPixel = 0; iPixel < PIXELS_PER_THREAD; ++iPixel)
	{
		READ_FROM_LDS( iLineOffset, (iPixelOffset + iPixel), RDI[iPixel] )
	}

	// Increment the LDS offset by PIXELS_PER_THREAD
	iPixelOffset += PIXELS_PER_THREAD;

	// First half of the kernel
	[unroll]
	for (iIteration = 0; iIteration < KERNEL_RADIUS; iIteration += STEP_SIZE)
	{
		// Macro defines what happens for each kernel iteration
		KERNEL_ITERATION( iIteration, KD, iPixel, PIXELS_PER_THREAD, O, RDI )

			// Macro to cache LDS reads in GPRs
			CACHE_LDS_READS( iIteration, iLineOffset, iPixelOffset, RDI )
	}

	// Prime the GPRs for the second half of the kernel
	[unroll]
	for (iPixel = 0; iPixel < PIXELS_PER_THREAD; ++iPixel)
	{
		READ_FROM_LDS( iLineOffset, (iPixelOffset - PIXELS_PER_THREAD + iIteration + 1 + iPixel), RDI[iPixel] )
	}

	// Second half of the kernel
	[unroll]
	for (iIteration = KERNEL_RADIUS + 1; iIteration < KERNEL_DIAMETER; iIteration += STEP_SIZE)
	{
		// Macro defines what happens for each kernel iteration
		KERNEL_ITERATION( iIteration, KD, iPixel, PIXELS_PER_THREAD, O, RDI )

			// Macro to cache LDS reads in GPRs
			CACHE_LDS_READS( iIteration, iLineOffset, iPixelOffset, RDI )
	}

	// Macros define final weighting and output
	KERNEL_FINAL_WEIGHT( KD, iPixel, PIXELS_PER_THREAD, O )
		KERNEL_OUTPUT( i2Center, i2Inc, iPixel, PIXELS_PER_THREAD, O, KD )
}


[numthreads( NUM_THREADS, RUN_LINES, 1 )]
void cs_filter_main( uint3 Gid : SV_GroupID, uint3 GTid : SV_GroupThreadID )
{
	int iLoadOffset = GTid.x * LOAD_PER_THREAD;
	int iLineOffset = GTid.y;

	int2 i2GroupCoord = int2((Gid.x * RUN_SIZE) - KERNEL_RADIUS, Gid.y * RUN_LINES);
	int2 i2Coord = int2( i2GroupCoord.x + iLoadOffset, i2GroupCoord.y );

	[unroll] for (int i = 0; i < LOAD_PER_THREAD; ++i)
	{
		WRITE_TO_LDS(SAMPLE(i2Coord + int2(i,GTid.y)), iLineOffset, iLoadOffset + i)
	}

	if (GTid.x < EXTRA_SAMPLES)
	{
		WRITE_TO_LDS(SAMPLE(i2GroupCoord + int2(RUN_SIZE_PLUS_KERNEL - 1 - GTid.x, GTid.y)), iLineOffset, RUN_SIZE_PLUS_KERNEL - 1 - GTid.x)
	}

	GroupMemoryBarrierWithGroupSync();

	int iPixelOffset = GTid.x * PIXELS_PER_THREAD;
	i2Coord = int2(i2GroupCoord.x + iPixelOffset + KERNEL_RADIUS, i2GroupCoord.y + GTid.y);

	if (i2Coord.x < g_f4OutputSize.x)
	{
		ComputeFilterKernal( iPixelOffset, iLineOffset, i2Coord, int2(1, 0) );
	}
}